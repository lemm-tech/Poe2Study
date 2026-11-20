#include "NamedPipeClie.h"
#include "../Common/Logger.h"
#include "../Common/PipeIo.h"
#include "../Common/GameTarget.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
using namespace managed;
namespace pipe
{
namespace {
std::atomic<uint32_t> expectedGamePid{0};
std::atomic<uint64_t> expectedGameStart{0};
bool ExpectedServer(HANDLE handle) {
    const DWORD expected = expectedGamePid.load();
    if (!expected) return true;
    ULONG server = 0;
    return GetNamedPipeServerProcessId(handle, &server) && server == expected &&
           gametarget::Alive(server, expectedGameStart.load());
}
}
void SetExpectedGameIdentity(uint32_t pid, uint64_t start) {
    expectedGameStart.store(start);
    expectedGamePid.store(pid);
}
struct ClientState
{
    std::mutex mutex;
    bool active = true;
    DispatchCallback callback;
};
struct Pending
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false, ok = false;
    MessageType expected = MT_Ack;
    bool synchronous = false;
    std::vector<uint8_t> body;
    std::weak_ptr<ClientState> scope;
};
class Transport
{
    struct Out
    {
        uint64_t client = 0, session = 0;
        std::vector<uint8_t> bytes;
        uint64_t generation = 0;
    };
    std::wstring name;
    Role role;
    uint64_t owner;
    std::weak_ptr<Transport> controller;
    std::atomic_bool running{false}, socketReady{false};
    std::atomic<uint32_t> registrationError{uint32_t(Error::Unavailable)};
    HANDLE stop = nullptr;
    std::atomic<HANDLE> handle{INVALID_HANDLE_VALUE};
    std::atomic<uint64_t> generation{0};
    std::thread reader, writer;
    std::mutex mutex, writeMutex;
    std::condition_variable cv;
    Welcome identity{};
    uint64_t txSequence = 0, rxSequence = 0, nextRequest = 0, lastReceive = 0, walkRequest = 0;
    std::deque<Out> out;
    std::map<uint64_t, std::shared_ptr<Pending>> pending;
    std::vector<std::weak_ptr<ClientState>> listeners;
    void Drop()
    {
        socketReady = false;
        if (handle != INVALID_HANDLE_VALUE)
            CancelIoEx(handle, nullptr);
        std::map<uint64_t, std::shared_ptr<Pending>> old;
        {
            std::lock_guard<std::mutex> l(mutex);
            identity = Welcome{};
            walkRequest = 0;
            out.clear();
            old.swap(pending);
            cv.notify_all();
        }
        for (auto &v : old)
        {
            std::lock_guard<std::mutex> l(v.second->mutex);
            v.second->done = true;
            v.second->ok = false;
            v.second->cv.notify_all();
        }
        std::lock_guard<std::mutex> w(writeMutex);
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
    void Notify(MessageType t, const void *p, size_t n, const std::weak_ptr<ClientState> &weak)
    {
        if (auto s = weak.lock())
        {
            std::lock_guard<std::mutex> l(s->mutex);
            if (s->active && s->callback)
                s->callback(t, p, n);
        }
    }
    bool Handle(MessageType t, const void *p, size_t n)
    {
        if (t == MT_ManagedWelcome)
        {
            Welcome w;
            if (n != sizeof(w))
                return false;
            memcpy(&w, p, n);
            ULONG server = 0;
            const bool valid = ExpectedServer(handle) && GetNamedPipeServerProcessId(handle, &server) && w.magic == kMagic &&
                               w.version == kVersion && w.role == role && w.owner == owner &&
                               w.gamePid == server && w.error == Error::Ok && w.clientId && w.sessionId;
            if (!valid)
            {
                // 记录服务端拒绝原因；GUI 用它区分 ControllerOwned 和普通的“还没连上”。
                registrationError.store(uint32_t(w.error == Error::Ok ? Error::BadProtocol : w.error));
                LOGW("[Managed] register role=%s rejected error=%u (role registration refused)", RoleName(role),
                     unsigned(w.error));
                return false;
            }
            registrationError.store(uint32_t(Error::Ok));
            {
                std::lock_guard<std::mutex> l(mutex);
                if (identity.clientId)
                    return false;
                identity = w;
                rxSequence = txSequence = nextRequest = 0;
                lastReceive = GetTickCount64();
                cv.notify_all();
            }
            LOGI("[Managed] ready role=%s client=%llu session=%llu gamePid=%u", RoleName(role), w.clientId, w.sessionId,
                 w.gamePid);
            return true;
        }
        if (t != MT_ManagedPacket)
            return false;
        Envelope e;
        const uint8_t *body = nullptr;
        if (!Decode(p, n, e, body))
            return false;
        std::shared_ptr<Pending> request;
        std::vector<std::weak_ptr<ClientState>> subs;
        {
            std::lock_guard<std::mutex> l(mutex);
            if (!identity.clientId || e.clientId != identity.clientId || e.sessionId != identity.sessionId ||
                e.sequence != rxSequence + 1)
                return false;
            rxSequence = e.sequence;
            lastReceive = GetTickCount64();
            if (e.requestId == walkRequest &&
                (e.type == MT_WalkCallAck || e.type == MT_ManagedError || (e.flags & Final)))
                walkRequest = 0;
            if (e.flags & Event)
            {
                if (e.requestId || e.flags != Event || !EventMask(e.type))
                    return false;
                for (auto i = listeners.begin(); i != listeners.end();)
                    if (i->expired())
                        i = listeners.erase(i);
                    else
                    {
                        subs.push_back(*i);
                        ++i;
                    }
            }
            else
            {
                if ((e.flags != Response && e.flags != (Response | Final)) || !e.requestId)
                    return false;
                auto i = pending.find(e.requestId);
                if (i != pending.end())
                    request = i->second;
            }
        }
        if (e.flags & Event)
        {
            for (auto &s : subs)
                Notify(e.type, body, e.payloadLength, s);
            return true;
        }
        if (!request)
            return true;
        bool remove = (e.flags & Final) != 0;
        {
            std::lock_guard<std::mutex> l(request->mutex);
            if (e.type == MT_ManagedError)
            {
                request->done = true;
                request->ok = false;
                remove = true;
                ErrorReply error{};
                if (e.payloadLength == sizeof(error))
                {
                    memcpy(&error, body, sizeof(error));
                    LOGW("[Managed] request=%llu rejected=%u type=%u", e.requestId, unsigned(error.code),
                         unsigned(error.rejectedType));
                }
            }
            else if (request->synchronous && e.type == request->expected)
            {
                request->body.assign(body, body + e.payloadLength);
                request->ok = true;
                request->done = true;
                remove = true;
            }
            else if (!request->synchronous && e.type != MT_RequestDone)
                Notify(e.type, body, e.payloadLength, request->scope);
            if (e.flags & Final)
            {
                request->done = true;
                if (!request->synchronous)
                    request->ok = true;
            }
            request->cv.notify_all();
        }
        if (remove)
        {
            std::lock_guard<std::mutex> l(mutex);
            pending.erase(e.requestId);
        }
        return true;
    }
    void ReadLoop()
    {
        while (running)
        {
            if (role != Role::Controller && role != Role::Observer)
            {
                auto c = controller.lock();
                if (!c || !c->Identity().clientId)
                {
                    WaitForSingleObject(stop, 50);
                    continue;
                }
            }
            if (!WaitNamedPipeW(name.c_str(), 200))
            {
                WaitForSingleObject(stop, 200);
                continue;
            }
            HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                WaitForSingleObject(stop, 200);
                continue;
            }
            // Reject a misrouted/old server before sending even the role-registration packet.
            if (!ExpectedServer(h)) {
                CloseHandle(h);
                registrationError.store(uint32_t(Error::BadProtocol));
                WaitForSingleObject(stop, 200);
                continue;
            }
            {
                std::lock_guard<std::mutex> w(writeMutex);
                handle = h;
                ++generation;
                socketReady = true;
            }
            registrationError.store(uint32_t(Error::Unavailable));
            Hello hello;
            hello.role = role;
            hello.owner = owner;
            hello.processId = GetCurrentProcessId();
            hello.subscriptions = DefaultSubscriptions(role);
            {
                std::lock_guard<std::mutex> l(mutex);
                out.push_back({0, 0, Frame(MT_ManagedHello, &hello, sizeof(hello)), generation.load()});
                lastReceive = GetTickCount64();
                cv.notify_all();
            }
            uint64_t lastHeartbeat = GetTickCount64();
            {
                pipeio::Reader input(h, stop);
                StreamDecoder decoder;
                while (running && socketReady)
                {
                    const uint64_t now = GetTickCount64();
                    const Welcome id = Identity();
                    uint64_t last = 0;
                    {
                        std::lock_guard<std::mutex> l(mutex);
                        last = lastReceive;
                    }
                    const DWORD limit = id.clientId ? kHeartbeatTimeoutMs : 3000;
                    const uint64_t elapsed = now - last;
                    if (elapsed >= limit)
                        break;
                    if (id.clientId && now - lastHeartbeat >= kHeartbeatMs)
                    {
                        PacketHeader heart{};
                        heart.m_MsgType = MT_Heartbeat;
                        Submit(&heart, nullptr);
                        lastHeartbeat = now;
                    }
                    DWORD timeout = (std::min)(DWORD(1000), DWORD(limit - elapsed));
                    if (id.clientId)
                        timeout = (std::min)(timeout, DWORD(kHeartbeatMs - (now - lastHeartbeat)));
                    DWORD received = 0;
                    const auto result = input.Read(timeout, received);
                    if (result == pipeio::ReadResult::Failed)
                        break;
                    if (result == pipeio::ReadResult::Data &&
                        !decoder.Feed(input.Data(), received, [this](MessageType t, const void *p, size_t n) {
                            return running && socketReady && Handle(t, p, n);
                        }))
                    {
                        break;
                    }
                }
            }
            // Cancel/drain the read before Drop closes the underlying handle.
            Drop();
            if (running)
            {
                LOGW("[Managed] disconnected role=%s; commands are not replayed", RoleName(role));
                WaitForSingleObject(stop, 500);
            }
        }
    }
    void WriteLoop()
    {
        while (running)
        {
            Out item;
            {
                std::unique_lock<std::mutex> l(mutex);
                cv.wait(l, [&] { return !running || !out.empty(); });
                if (!running)
                    break;
                item = std::move(out.front());
                out.pop_front();
                if (item.client && (identity.clientId != item.client || identity.sessionId != item.session))
                    continue;
            }
            std::lock_guard<std::mutex> w(writeMutex);
            // Recheck after acquiring the write lock: Drop may have reconnected
            // while this already-dequeued frame was waiting for that lock.
            if (!socketReady || item.generation != generation.load())
                continue;
            if (!pipeio::Transfer(handle, item.bytes.data(), DWORD(item.bytes.size()), true, stop))
            {
                socketReady = false;
                CancelIoEx(handle, nullptr);
            }
        }
    }

  public:
    Transport(std::wstring n, Role r, uint64_t o, std::weak_ptr<Transport> c = {})
        : name(std::move(n)), role(r), owner(o), controller(std::move(c))
    {
    }
    ~Transport()
    {
        Stop();
    }
    bool Start()
    {
        if (running.exchange(true))
            return true;
        stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop)
        {
            running = false;
            LOGW("[Managed] could not create stop event error=%lu", GetLastError());
            return false;
        }
        try
        {
            writer = std::thread([this] { WriteLoop(); });
            reader = std::thread([this] { ReadLoop(); });
        }
        catch (...)
        {
            LOGW("[Managed] could not start client threads role=%s", RoleName(role));
            Stop();
            return false;
        }
        return true;
    }
    void Stop()
    {
        if (!running.exchange(false))
            return;
        SetEvent(stop);
        {
            std::lock_guard<std::mutex> l(mutex);
            // Synchronize with the writer's wait predicate before notifying.
            cv.notify_all();
        }
        {
            std::lock_guard<std::mutex> w(writeMutex);
            if (handle != INVALID_HANDLE_VALUE)
                CancelIoEx(handle, nullptr);
        }
        if (reader.joinable())
            reader.join();
        if (writer.joinable())
            writer.join();
        Drop();
        CloseHandle(stop);
        stop = nullptr;
    }
    Welcome Identity()
    {
        std::lock_guard<std::mutex> l(mutex);
        return socketReady ? identity : Welcome{};
    }
    Error RegistrationError() const
    {
        return static_cast<Error>(registrationError.load());
    }
    void Listen(const std::shared_ptr<ClientState> &s)
    {
        std::lock_guard<std::mutex> l(mutex);
        listeners.push_back(s);
    }
    uint64_t Submit(const void *packet, const std::shared_ptr<Pending> &p, uint64_t boundClient = 0,
                    uint64_t boundSession = 0)
    {
        if (!packet)
            return 0;
        PacketHeader h{};
        memcpy(&h, packet, HEADER_LEN);
        if (h.m_len > MSG_LEN)
            return 0;
        std::lock_guard<std::mutex> l(mutex);
        if (!running || !socketReady || !identity.clientId || out.size() >= 512 || (p && pending.size() >= 512))
            return 0;
        if (boundClient && (identity.clientId != boundClient || identity.sessionId != boundSession))
            return 0;
        if (h.m_MsgType == MT_WalkCall && walkRequest)
            return 0;
        Envelope e;
        e.flags = Request;
        e.clientId = identity.clientId;
        e.sessionId = identity.sessionId;
        e.requestId = ++nextRequest;
        e.sequence = ++txSequence;
        e.type = h.m_MsgType;
        if (h.m_MsgType == MT_WalkCall)
            walkRequest = e.requestId;
        if (p)
            pending.emplace(e.requestId, p);
        out.push_back({e.clientId, e.sessionId, Frame(e, static_cast<const uint8_t *>(packet) + HEADER_LEN, h.m_len),
                       generation.load()});
        cv.notify_all();
        return e.requestId;
    }
    void Cancel(uint64_t id)
    {
        std::lock_guard<std::mutex> l(mutex);
        pending.erase(id);
    }
    void CancelScope(const std::shared_ptr<ClientState> &s)
    {
        std::lock_guard<std::mutex> l(mutex);
        for (auto i = pending.begin(); i != pending.end();)
            if (i->second->scope.lock() == s)
                i = pending.erase(i);
            else
                ++i;
    }
};
namespace
{
struct Manager
{
    std::mutex mutex;
    std::wstring name;
    uint64_t owner = pipeio::NewId();
    std::map<Role, std::shared_ptr<Transport>> connections;
    ~Manager()
    {
        Stop();
    }
    bool Start(const std::wstring &n)
    {
        std::lock_guard<std::mutex> l(mutex);
        auto normalized = pipeio::Normalize(n);
        if (!name.empty() && name != normalized)
            return false;
        name = normalized;
        for (Role r : {Role::Controller, Role::Tracker, Role::Data})
            if (!connections.count(r))
            {
                auto p = std::make_shared<Transport>(
                    name, r, owner, r == Role::Controller ? std::weak_ptr<Transport>{} : connections[Role::Controller]);
                if (!p->Start())
                    return false;
                connections[r] = p;
            }
        return true;
    }
    std::shared_ptr<Transport> Observer(const std::wstring &n)
    {
        std::lock_guard<std::mutex> l(mutex);
        auto normalized = pipeio::Normalize(n);
        if (!name.empty() && name != normalized)
            return {};
        name = normalized;
        auto &i = connections[Role::Observer];
        if (!i)
        {
            i = std::make_shared<Transport>(name, Role::Observer, owner);
            if (!i->Start())
                i.reset();
        }
        return i;
    }
    std::shared_ptr<Transport> Get(Role r)
    {
        std::lock_guard<std::mutex> l(mutex);
        auto i = connections.find(r);
        if (i != connections.end())
            return i->second;
        if (r == Role::Observer && !name.empty())
        {
            auto p = std::make_shared<Transport>(name, r, owner);
            if (!p->Start())
                return {};
            connections[r] = p;
            return p;
        }
        return {};
    }
    void Stop()
    {
        std::map<Role, std::shared_ptr<Transport>> old;
        {
            std::lock_guard<std::mutex> l(mutex);
            old.swap(connections);
            name.clear();
        }
        for (auto &p : old)
            p.second->Stop();
    }
} manager;
} // namespace
ConnectionSnapshot GetConnectionSnapshot()
{
    ConnectionSnapshot s;
    auto c = manager.Get(Role::Controller), t = manager.Get(Role::Tracker), d = manager.Get(Role::Data);
    if (c)
        s.controllerError = c->RegistrationError();
    if (!c || !t || !d)
        return s;
    auto a = c->Identity(), b = t->Identity(), v = d->Identity();
    s.gamePid = a.gamePid;
    s.sessionId = a.sessionId;
    s.controllerId = a.clientId;
    s.trackerId = b.clientId;
    s.dataId = v.clientId;
    s.ready = a.clientId && b.clientId && v.clientId && a.sessionId == b.sessionId && a.sessionId == v.sessionId &&
              a.gamePid == b.gamePid && a.gamePid == v.gamePid;
    return s;
}
bool StartManagedConnections(const std::wstring &n, uint32_t timeout)
{
    if (!manager.Start(n))
        return false;
    uint64_t end = GetTickCount64() + timeout;
    do
    {
        if (GetConnectionSnapshot().ready)
            return true;
        Sleep(10);
    } while (GetTickCount64() < end);
    return false;
}
void StopManagedConnections()
{
    manager.Stop();
}
NamedPipeClie::NamedPipeClie(const std::wstring &n, DispatchCallback c, Role r)
    : m_name(n), m_role(r), m_state(std::make_shared<ClientState>())
{
    m_state->callback = std::move(c);
}
NamedPipeClie::~NamedPipeClie()
{
    Uninitialize();
}
bool NamedPipeClie::Initialize()
{
    if (IsConnected())
        return true;
    if (m_role == Role::Observer)
        m_transport = manager.Observer(m_name);
    else
    {
        if (!StartManagedConnections(m_name))
            return false;
        m_transport = manager.Get(m_role);
    }
    if (!m_transport)
        return false;
    uint64_t end = GetTickCount64() + 3000;
    Welcome id;
    do
    {
        id = m_transport->Identity();
        if (id.clientId)
            break;
        Sleep(10);
    } while (GetTickCount64() < end);
    if (!id.clientId)
        return false;
    m_clientId = id.clientId;
    m_sessionId = id.sessionId;
    {
        std::lock_guard<std::mutex> l(m_state->mutex);
        m_state->active = true;
    }
    if (m_role == Role::Tracker || m_role == Role::Observer)
        m_transport->Listen(m_state);
    return true;
}
bool NamedPipeClie::Uninitialize()
{
    {
        std::lock_guard<std::mutex> l(m_state->mutex);
        m_state->active = false;
    }
    if (m_transport)
        m_transport->CancelScope(m_state);
    m_clientId = m_sessionId = 0;
    m_transport.reset();
    return true;
}
bool NamedPipeClie::IsConnected() const
{
    if (!m_transport || !m_clientId || (m_role != Role::Observer && !GetConnectionSnapshot().ready))
        return false;
    auto w = m_transport->Identity();
    return w.clientId == m_clientId && w.sessionId == m_sessionId;
}
bool NamedPipeClie::SendPackage(void *msg)
{
    if (!IsConnected())
        return false;
    auto p = std::make_shared<Pending>();
    p->scope = m_state;
    return m_transport->Submit(msg, p, m_clientId, m_sessionId) != 0;
}
bool NamedPipeClie::Request(const void *msg, MessageType expected, std::vector<uint8_t> &body, uint32_t timeout)
{
    body.clear();
    if (!IsConnected())
        return false;
    auto p = std::make_shared<Pending>();
    p->scope = m_state;
    p->synchronous = true;
    p->expected = expected;
    auto id = m_transport->Submit(msg, p, m_clientId, m_sessionId);
    if (!id)
        return false;
    bool ok = false;
    {
        std::unique_lock<std::mutex> l(p->mutex);
        p->cv.wait_for(l, std::chrono::milliseconds(timeout), [&] { return p->done; });
        ok = p->done && p->ok;
        if (ok)
            body = p->body;
    }
    m_transport->Cancel(id);
    return ok;
}
bool NamedPipeClie::SendText(const std::string &s)
{
    auto b = Frame(MT_Text, s.data(), (std::min)(s.size(), size_t(MSG_LEN)));
    return SendPackage(b.data());
}
bool NamedPipeClie::SendDataChunk(const void *p, size_t n)
{
    if (n && !p)
        return false;
    auto b = Frame(MT_DataChunk, p, (std::min)(n, size_t(MSG_LEN)));
    return SendPackage(b.data());
}
} // namespace pipe
