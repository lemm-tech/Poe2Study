#include "NamedPipeServer.h"
#include "../Common/PipeIo.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
using namespace managed;
NamedPipeServer::NamedPipeServer(const std::wstring &n, DispatchCallback c)
    : m_name(pipeio::Normalize(n)), m_callback(std::move(c))
{
}
NamedPipeServer::~NamedPipeServer()
{
    Uninitialize();
}
bool NamedPipeServer::Initialize()
{
    if (m_running)
        return true;
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop)
        return false;
    m_session = pipeio::NewId();
    m_nextClient = 0;
    for (size_t i = 0; i < kPoolSize; ++i)
    {
        auto s = std::make_unique<Slot>();
        s->pipe =
            CreateNamedPipeW(m_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, kPoolSize, 65536, 65536, 0, nullptr);
        if (s->pipe == INVALID_HANDLE_VALUE)
        {
            LOGI("[Managed] CreateNamedPipe failed error=%lu", GetLastError());
            m_slots.push_back(std::move(s));
            Uninitialize();
            return false;
        }
        m_slots.push_back(std::move(s));
    }
    m_running = true;
    try
    {
        for (auto &v : m_slots)
        {
            Slot *s = v.get();
            s->writer = std::thread([this, s] { WriteLoop(*s); });
            s->worker = std::thread([this, s] { WorkLoop(*s); });
            s->reader = std::thread([this, s] { ReadLoop(*s); });
        }
    }
    catch (...)
    {
        LOGI("[Managed] could not start pipe threads");
        Uninitialize();
        return false;
    }
    LOGI("[Managed] session=%llu pid=%lu slots=%zu", m_session, GetCurrentProcessId(), kPoolSize);
    return true;
}
bool NamedPipeServer::Uninitialize()
{
    m_running = false;
    if (m_stop)
        SetEvent(m_stop);
    for (auto &s : m_slots)
    {
        {
            std::lock_guard<std::mutex> l(s->queueMutex);
            s->fault = true;
        }
        s->cv.notify_all();
        if (s->pipe != INVALID_HANDLE_VALUE)
            CancelIoEx(s->pipe, nullptr);
    }
    for (auto &s : m_slots)
    {
        if (s->reader.joinable())
            s->reader.join();
        if (s->worker.joinable())
            s->worker.join();
        if (s->writer.joinable())
            s->writer.join();
        if (s->pipe != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(s->pipe);
            CloseHandle(s->pipe);
        }
    }
    m_slots.clear();
    if (m_stop)
    {
        CloseHandle(m_stop);
        m_stop = nullptr;
    }
    return true;
}
NamedPipeServer::Slot *NamedPipeServer::FindLocked(const Route &r) const
{
    if (!r.clientId || r.sessionId != m_session)
        return nullptr;
    for (auto &s : m_slots)
        if (s->connected && !s->fault && s->identity.clientId == r.clientId && s->identity.sessionId == r.sessionId)
            return s.get();
    return nullptr;
}
bool NamedPipeServer::IsRouteCurrent(const Route &r) const
{
    std::lock_guard<std::mutex> l(m_registry);
    return FindLocked(r) != nullptr;
}
bool NamedPipeServer::IsConnected() const
{
    std::lock_guard<std::mutex> l(m_registry);
    for (auto &s : m_slots)
        if (s->connected && !s->fault && s->identity.clientId)
            return true;
    return false;
}
void NamedPipeServer::Drop(Slot &s)
{
    {
        std::lock_guard<std::mutex> l(m_registry);
        auto old = s.identity;
        if (old.clientId)
            LOGI("[Managed] disconnect role=%s client=%llu", RoleName(old.role), old.clientId);
        s.connected = false;
        s.identity = Welcome{};
        if (old.clientId && old.role == Role::Controller)
            for (auto &v : m_slots)
                if (v.get() != &s && v->identity.owner == old.owner && v->peerPid == s.peerPid &&
                    v->identity.role != Role::Observer)
                    v->fault = true;
    }
    {
        std::lock_guard<std::mutex> l(s.queueMutex);
        s.out.clear();
        s.tasks.clear();
    }
    s.cv.notify_all();
    CancelIoEx(s.pipe, nullptr);
    std::lock_guard<std::mutex> w(s.writeMutex);
    DisconnectNamedPipe(s.pipe);
}
bool NamedPipeServer::Enqueue(Slot &s, Out o)
{
    if (o.bytes.empty())
        return false;
    std::unique_lock<std::mutex> l(s.queueMutex);
    const auto ready = [&] { return !m_running || s.fault || !s.connected ||
                                   o.generation != s.generation.load() || s.out.size() < kQueueLimit; };
    // WELCOME is queued while holding the registry lock to preserve ordering.
    // It must never wait for a slow peer and stall every other connection.
    if (!ready() && (!o.route.clientId || !s.cv.wait_for(l, std::chrono::seconds(3), ready)))
    {
        s.fault = true;
        return false;
    }
    if (!m_running || s.fault || !s.connected || o.generation != s.generation.load())
        return false;
    s.out.push_back(std::move(o));
    s.cv.notify_all();
    return true;
}
bool NamedPipeServer::Reply(const Route &r, MessageType t, const void *p, size_t n, bool final)
{
    Slot *s;
    std::vector<uint8_t> bytes;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> l(m_registry);
        s = FindLocked(r);
        if (!s || n > MSG_LEN || (n && !p))
            return false;
        Envelope e;
        e.flags = Response | (final ? Final : 0);
        e.clientId = r.clientId;
        e.sessionId = r.sessionId;
        e.requestId = r.requestId;
        e.type = t;
        bytes = Frame(e, p, n);
        generation = s->generation.load();
    }
    return Enqueue(*s, {r, std::move(bytes), generation});
}
bool NamedPipeServer::SendPackageTo(const Route &r, const void *msg)
{
    if (!msg)
        return false;
    PacketHeader h{};
    memcpy(&h, msg, HEADER_LEN);
    return Reply(r, h.m_MsgType, static_cast<const uint8_t *>(msg) + HEADER_LEN, h.m_len);
}
void NamedPipeServer::Publish(MessageType t, const void *p, size_t n)
{
    if (n > MSG_LEN || (n && !p))
        return;
    std::vector<Route> routes;
    {
        std::lock_guard<std::mutex> l(m_registry);
        for (auto &s : m_slots)
            if (s->connected && !s->fault && s->identity.clientId && (s->subscriptions & EventMask(t)))
                routes.push_back({s->identity.clientId, m_session, 0});
    }
    for (auto &r : routes)
    {
        Slot *s;
        std::vector<uint8_t> bytes;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> l(m_registry);
            s = FindLocked(r);
            if (!s)
                continue;
            Envelope e;
            e.flags = Event;
            e.clientId = r.clientId;
            e.sessionId = m_session;
            e.type = t;
            bytes = Frame(e, p, n);
            generation = s->generation.load();
        }
        // A slow observer must never block the navigation/game thread.
        std::lock_guard<std::mutex> l(s->queueMutex);
        if (!s->connected || s->fault || generation != s->generation.load())
            continue;
        if (s->out.size() >= kQueueLimit)
        {
            s->fault = true;
            continue;
        }
        s->out.push_back({r, std::move(bytes), generation});
        s->cv.notify_all();
    }
}
void NamedPipeServer::PublishPackage(const void *msg)
{
    if (!msg)
        return;
    PacketHeader h{};
    memcpy(&h, msg, HEADER_LEN);
    Publish(h.m_MsgType, static_cast<const uint8_t *>(msg) + HEADER_LEN, h.m_len);
}
void NamedPipeServer::WriteLoop(Slot &s)
{
    while (m_running)
    {
        Out o;
        {
            std::unique_lock<std::mutex> l(s.queueMutex);
            s.cv.wait(l, [&] { return !m_running || !s.out.empty(); });
            if (!m_running)
                break;
            o = std::move(s.out.front());
            s.out.pop_front();
            s.cv.notify_all();
        }
        std::lock_guard<std::mutex> w(s.writeMutex);
        if (!s.connected || s.fault || o.generation != s.generation.load())
            continue;
        if (o.route.clientId)
        {
            std::lock_guard<std::mutex> l(m_registry);
            if (FindLocked(o.route) != &s)
                continue;
            Envelope e{};
            memcpy(&e, o.bytes.data() + HEADER_LEN, sizeof(e));
            e.sequence = ++s.txSequence;
            memcpy(o.bytes.data() + HEADER_LEN, &e, sizeof(e));
        }
        if (!pipeio::Transfer(s.pipe, o.bytes.data(), DWORD(o.bytes.size()), true, m_stop))
        {
            s.fault = true;
            CancelIoEx(s.pipe, nullptr);
        }
    }
}
void NamedPipeServer::WorkLoop(Slot &s)
{
    while (m_running)
    {
        Task t;
        {
            std::unique_lock<std::mutex> l(s.queueMutex);
            s.cv.wait(l, [&] { return !m_running || !s.tasks.empty(); });
            if (!m_running)
                break;
            t = std::move(s.tasks.front());
            s.tasks.pop_front();
        }
        if (!IsRouteCurrent(t.route))
            continue;
        try
        {
            char audit[160];
            int count = snprintf(audit, sizeof(audit), "request client=%llu request=%llu type=%u", t.route.clientId,
                                 t.route.requestId, unsigned(t.type));
            if (count > 0)
                Publish(MT_LogEvent, audit, size_t(count));
            if (m_callback)
                m_callback(t.type, t.body.empty() ? nullptr : t.body.data(), t.body.size(), t.route);
            {
                std::lock_guard<std::mutex> l(m_registry);
                if (t.type == MT_WalkCall && FindLocked(t.route) == &s)
                    s.walkBusy = false;
            }
            Reply(t.route, MT_RequestDone, nullptr, 0, true);
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> l(m_registry);
                if (t.type == MT_WalkCall && FindLocked(t.route) == &s)
                    s.walkBusy = false;
            }
            ErrorReply e{Error::Unavailable, t.type};
            Reply(t.route, MT_ManagedError, &e, sizeof(e), true);
        }
    }
}
bool NamedPipeServer::Handle(Slot &s, MessageType t, const void *p, size_t n)
{
    if (t == MT_ManagedHello)
    {
        Hello h{};
        if (n != sizeof(h))
            return false;
        memcpy(&h, p, sizeof(h));
        Welcome w;
        bool queued = false;
        w.role = h.role;
        w.owner = h.owner;
        w.sessionId = m_session;
        w.gamePid = GetCurrentProcessId();
        {
            std::lock_guard<std::mutex> l(m_registry);
            if (s.identity.clientId || h.magic != kMagic || h.version != kVersion || h.processId != s.peerPid ||
                !h.owner || h.role < Role::Controller || h.role > Role::Observer ||
                !CanSubscribe(h.role, h.subscriptions))
                return false;
            bool controller = false;
            for (auto &v : m_slots)
                if (v->connected && !v->fault && v->identity.clientId)
                {
                    if (v->identity.role == Role::Controller)
                    {
                        if (v->identity.owner == h.owner && v->peerPid == s.peerPid)
                            controller = true;
                        if (h.role == Role::Controller)
                            w.error = Error::ControllerOwned;
                    }
                    if (h.role != Role::Observer && h.role != Role::Controller && v->identity.role == h.role &&
                        v->identity.owner == h.owner && v->peerPid == s.peerPid)
                        w.error = Error::RoleOwned;
                }
            if (h.role != Role::Controller && h.role != Role::Observer && !controller)
                w.error = Error::ControllerOwned;
            if (w.error == Error::Ok)
            {
                w.clientId = ++m_nextClient;
                s.identity = w;
                s.subscriptions = h.subscriptions;
                s.rxSequence = s.rxRequest = s.txSequence = 0;
                s.lastHeartbeat = GetTickCount64();
            }
            // Publish also takes m_registry. Queue WELCOME before making this
            // identity visible to another connection's concurrent event publish.
            auto bytes = Frame(MT_ManagedWelcome, &w, sizeof(w));
            queued = Enqueue(s, {{}, std::move(bytes), s.generation.load()});
        }
        LOGI("[Managed] register role=%s client=%llu session=%llu pid=%u error=%u", RoleName(w.role), w.clientId,
             w.sessionId, w.gamePid, unsigned(w.error));
        if (queued && w.error == Error::Ok)
        {
            GameStatus status{w.gamePid, w.sessionId, 1};
            Publish(MT_GameStatus, &status, sizeof(status));
        }
        return queued;
    }
    if (t != MT_ManagedPacket)
        return false;
    Envelope e;
    const uint8_t *body = nullptr;
    if (!Decode(p, n, e, body))
        return false;
    Route r{e.clientId, e.sessionId, e.requestId};
    Error err = Error::Ok;
    {
        std::lock_guard<std::mutex> l(m_registry);
        if (FindLocked(r) != &s || e.flags != Request || !e.requestId)
            return false;
        if (e.sequence != s.rxSequence + 1 || e.requestId <= s.rxRequest)
            err = Error::BadSequence;
        s.rxSequence = e.sequence;
        s.rxRequest = e.requestId;
        if (!Allowed(s.identity.role, e.type))
            err = Error::Forbidden;
        if (err == Error::Ok && e.type == MT_WalkCall)
        {
            if (s.walkBusy)
                err = Error::Busy;
            else
                s.walkBusy = true;
        }
    }
    if (err != Error::Ok)
    {
        ErrorReply x{err, e.type};
        Reply(r, MT_ManagedError, &x, sizeof(x), true);
        return true;
    }
    if (e.type == MT_Heartbeat)
    {
        if (n != sizeof(Envelope))
            return false;
        {
            std::lock_guard<std::mutex> l(m_registry);
            s.lastHeartbeat = GetTickCount64();
        }
        return Reply(r, MT_Heartbeat, nullptr, 0, true);
    }
    if (e.type == MT_Subscribe)
    {
        uint32_t mask = 0;
        if (e.payloadLength != sizeof(mask))
            return false;
        memcpy(&mask, body, sizeof(mask));
        {
            std::lock_guard<std::mutex> l(m_registry);
            if (!CanSubscribe(s.identity.role, mask))
                err = Error::Forbidden;
            else
                s.subscriptions = mask;
        }
        if (err != Error::Ok)
        {
            ErrorReply x{err, e.type};
            return Reply(r, MT_ManagedError, &x, sizeof(x), true);
        }
        return Reply(r, MT_RequestDone, nullptr, 0, true);
    }
    Task task{r, e.type, std::vector<uint8_t>(body, body + e.payloadLength)};
    {
        std::lock_guard<std::mutex> l(s.queueMutex);
        if (s.tasks.size() >= 128)
        {
            ErrorReply x{Error::Busy, e.type};
            // Do not enqueue a reply while holding queueMutex.
            s.fault = true;
            return false;
        }
        s.tasks.push_back(std::move(task));
    }
    s.cv.notify_all();
    return true;
}
void NamedPipeServer::ReadLoop(Slot &s)
{
    while (m_running)
    {
        s.fault = false;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
        {
            LOGI("[Managed] could not create connection event error=%lu", GetLastError());
            WaitForSingleObject(m_stop, 200);
            continue;
        }
        BOOL accepted = ConnectNamedPipe(s.pipe, &ov);
        DWORD error = accepted ? 0 : GetLastError();
        if (!accepted && error == ERROR_IO_PENDING)
        {
            HANDLE ev[2] = {m_stop, ov.hEvent};
            DWORD w = WaitForMultipleObjects(2, ev, FALSE, INFINITE);
            DWORD got = 0;
            if (w != WAIT_OBJECT_0 + 1)
            {
                CancelIoEx(s.pipe, &ov);
                GetOverlappedResult(s.pipe, &ov, &got, TRUE);
                CloseHandle(ov.hEvent);
                break;
            }
            accepted = GetOverlappedResult(s.pipe, &ov, &got, FALSE);
        }
        else if (error == ERROR_PIPE_CONNECTED)
            accepted = TRUE;
        CloseHandle(ov.hEvent);
        if (!m_running)
            break;
        if (!accepted)
        {
            WaitForSingleObject(m_stop, 20);
            continue;
        }
        {
            std::lock_guard<std::mutex> l(m_registry);
            s.identity = Welcome{};
            s.connected = true;
            ++s.generation;
            s.walkBusy = false;
            s.lastHeartbeat = GetTickCount64();
            ULONG pid = 0;
            if (!GetNamedPipeClientProcessId(s.pipe, &pid) || !pid)
                s.fault = true;
            s.peerPid = pid;
        }
        {
            pipeio::Reader input(s.pipe, m_stop);
            StreamDecoder decoder;
            while (m_running && s.connected && !s.fault)
            {
                DWORD timeout = 1000;
                {
                    std::lock_guard<std::mutex> l(m_registry);
                    const uint64_t elapsed = GetTickCount64() - s.lastHeartbeat;
                    const DWORD limit = s.identity.clientId ? kHeartbeatTimeoutMs : 3000;
                    if (elapsed >= limit)
                        s.fault = true;
                    else
                        timeout = (std::min)(timeout, DWORD(limit - elapsed));
                }
                if (s.fault)
                    break;
                DWORD received = 0;
                const auto result = input.Read(timeout, received);
                if (result == pipeio::ReadResult::Failed)
                    break;
                if (result == pipeio::ReadResult::Data &&
                    !decoder.Feed(input.Data(), received, [this, &s](MessageType t, const void *p, size_t n) {
                        return m_running && !s.fault && Handle(s, t, p, n);
                    }))
                {
                    s.fault = true;
                    break;
                }
            }
        }
        // Reader drains its pending I/O before the slot can disconnect/reconnect.
        Drop(s);
    }
}
