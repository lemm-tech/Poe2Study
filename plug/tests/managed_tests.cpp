#include "../Common/AppConfig.h"
#include "../Common/PipeIo.h"
#include "../Monitor/NamedPipeServer.h"
#include "../Plug/NamedPipeClie.h"
#include "../Plug/Navigator.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
using namespace managed;
int pass = 0, fail = 0;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        bool ok = !!(x);                                                                                               \
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", #x);                                                                 \
        ok ? ++pass : ++fail;                                                                                          \
    } while (0)
template <class F> bool Wait(F f, DWORD ms = 4000)
{
    auto end = GetTickCount64() + ms;
    do
    {
        if (f())
            return true;
        Sleep(10);
    } while (GetTickCount64() < end);
    return false;
}
struct EventAt
{
    MessageType type;
    uint64_t at;
    int32_t x, y;
};
std::mutex state;
std::vector<EventAt> timeline;
std::atomic<int> walks{0}, positions{0}, slowRows{0};
std::atomic<bool> slowMap{false};
int32_t x = 10, y = 10, tx = 10, ty = 10;
Route saved;
NamedPipeServer *server = nullptr;
void Handler(MessageType type, const void *data, size_t n, const Route &route)
{
    if (type == MT_PositionReq)
    {
        NavStatus p{};
        {
            std::lock_guard<std::mutex> l(state);
            x = tx;
            y = ty;
            p.x = x;
            p.y = y;
            saved = route;
            timeline.push_back({type, GetTickCount64(), x, y});
        }
        ++positions;
        server->Reply(route, MT_PositionReply, &p, sizeof(p));
    }
    else if (type == MT_WalkCall)
    {
        WalkCallReq r{};
        if (n != sizeof(r))
            return;
        memcpy(&r, data, n);
        WalkCallAck ack{};
        {
            std::lock_guard<std::mutex> l(state);
            tx = r.x;
            ty = r.y;
            timeline.push_back({type, GetTickCount64(), tx, ty});
        }
        ++walks;
        ack.targetX = r.x;
        ack.targetY = r.y;
        ack.ok = 0;
        ack.result = 0;
        WalkCallAckMessage msg(ack);
        server->SendPackageTo(route, &msg);
    }
    else if (type == MT_Text)
    {
        server->Reply(route, MT_Ack, data, n);
    }
    else if (type == MT_MapReq)
    {
        if (slowMap)
        {
            uint8_t row[MSG_LEN]{};
            for (int i = 0; i < 2500; ++i)
                if (!server->Reply(route, MT_MapRow, row, sizeof(row)))
                    break;
        }
        else
        {
            MapMeta m{};
            m.width = 23;
            m.height = 23;
            m.rowStride = 12;
            MapMetaMessage meta(m);
            server->SendPackageTo(route, &meta);
        }
    }
}
struct Raw
{
    HANDLE h = INVALID_HANDLE_VALUE, stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Welcome welcome{};
    uint64_t seq = 0, req = 0;
    ~Raw()
    {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        CloseHandle(stop);
    }
    bool Open(const std::wstring &name, Role role, uint64_t owner)
    {
        if (!WaitNamedPipeW(name.c_str(), 2000))
            return false;
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                        nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        Hello hello;
        hello.role = role;
        hello.owner = owner;
        hello.processId = GetCurrentProcessId();
        auto b = Frame(MT_ManagedHello, &hello, sizeof(hello));
        if (!pipeio::Transfer(h, b.data(), DWORD(b.size()), true, stop))
            return false;
        PacketHeader head{};
        if (!pipeio::Transfer(h, &head, sizeof(head), false, stop) || head.m_MsgType != MT_ManagedWelcome ||
            head.m_len != sizeof(welcome))
            return false;
        return pipeio::Transfer(h, &welcome, sizeof(welcome), false, stop);
    }
    bool Request(MessageType t, uint64_t fakeClient = 0)
    {
        Envelope e;
        e.flags = managed::Request;
        e.clientId = fakeClient ? fakeClient : welcome.clientId;
        e.sessionId = welcome.sessionId;
        e.requestId = ++req;
        e.sequence = ++seq;
        e.type = t;
        auto b = Frame(e, nullptr, 0);
        return pipeio::Transfer(h, b.data(), DWORD(b.size()), true, stop);
    }
};
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--observer"))
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, nullptr, 0);
        std::wstring target(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, &target[0], n);
        target.pop_back();
        pipe::NamedPipeClie observerOnly(target, nullptr, Role::Observer);
        if (!observerOnly.Initialize())
            return 2;
        PacketHeader req{};
        req.m_MsgType = MT_PositionReq;
        std::vector<uint8_t> response;
        bool ok = observerOnly.Request(&req, MT_PositionReply, response);
        WalkCallReq w{};
        WalkCallMessage walk(w);
        ok = ok && !observerOnly.Request(&walk, MT_WalkCallAck, response);
        observerOnly.Uninitialize();
        pipe::StopManagedConnections();
        return ok ? 0 : 3;
    }
    std::string shortName = "Poe2ManagedTest_" + std::to_string(GetCurrentProcessId());
    app::Config().pipeName = shortName;
    std::wstring name = app::Config().PipeFullName();
    auto instance = std::make_unique<NamedPipeServer>(name, Handler);
    server = instance.get();
    CHECK(instance->Initialize());
    CHECK(pipe::StartManagedConnections(name));
    auto initial = pipe::GetConnectionSnapshot();
    CHECK(initial.ready && initial.gamePid == GetCurrentProcessId());
    CHECK(initial.controllerId && initial.trackerId && initial.dataId && initial.controllerId != initial.trackerId &&
          initial.controllerId != initial.dataId);
    std::atomic<int> events{0}, logs{0};
    pipe::NamedPipeClie tracker(
        name,
        [&](MessageType t, const void *, size_t) {
            if (t == MT_NavStatus)
                ++events;
        },
        Role::Tracker);
    CHECK(tracker.Initialize());
    pipe::NamedPipeClie controller(name, nullptr, Role::Controller);
    CHECK(controller.Initialize());
    pipe::NamedPipeClie data(
        name,
        [&](MessageType t, const void *, size_t) {
            if (t == MT_MapRow)
            {
                ++slowRows;
                Sleep(2);
            }
        },
        Role::Data);
    CHECK(data.Initialize());
    pipe::NamedPipeClie observer(
        name,
        [&](MessageType t, const void *, size_t) {
            if (t == MT_LogEvent)
                ++logs;
        },
        Role::Observer);
    CHECK(observer.Initialize());
    for (int i = 0; i < 25; ++i)
    {
        pipe::NamedPipeClie scope(name, nullptr);
        if (!scope.Initialize() || scope.ClientId() != initial.controllerId)
            ++fail;
    }
    CHECK(pipe::GetConnectionSnapshot().Same(initial));
    CHECK(!pipe::StartManagedConnections(name + L"_wrong", 10));
    NavStatus event{};
    event.x = 123;
    event.y = 456;
    instance->Publish(MT_NavStatus, &event, sizeof(event));
    CHECK(Wait([&] { return events.load() > 0; }));
    PacketHeader position{};
    position.m_MsgType = MT_PositionReq;
    std::vector<uint8_t> body;
    CHECK(controller.Request(&position, MT_PositionReply, body) && body.size() == sizeof(NavStatus));
    CHECK(Wait([&] { return logs.load() > 0; }));
    WalkCallReq w{};
    w.x = 40;
    w.y = 10;
    WalkCallMessage walk(w);
    int before = walks;
    CHECK(!data.Request(&walk, MT_WalkCallAck, body, 500));
    CHECK(!observer.Request(&walk, MT_WalkCallAck, body, 500));
    CHECK(walks == before);
    {
        Raw second;
        CHECK(second.Open(name, Role::Controller, pipeio::NewId()));
        CHECK(second.welcome.error == Error::ControllerOwned && second.welcome.clientId == 0);
    }
    {
        Raw forged;
        CHECK(forged.Open(name, Role::Observer, pipeio::NewId()));
        CHECK(forged.welcome.error == Error::Ok);
        CHECK(forged.Request(MT_WalkCall, initial.controllerId));
        Sleep(100);
        CHECK(walks == before);
    }
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --observer \"" + name + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        bool created = CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                      &si, &pi) != FALSE;
        CHECK(created);
        if (created)
        {
            DWORD rc = 99;
            CHECK(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0);
            GetExitCodeProcess(pi.hProcess, &rc);
            CHECK(rc == 0);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    // Concurrent request IDs must return their own unique data, even on one role.
    std::vector<std::future<bool>> requests;
    for (uint64_t i = 1; i <= 24; ++i)
        requests.push_back(std::async(std::launch::async, [&, i] {
            auto packet = Frame(MT_Text, &i, sizeof(i));
            std::vector<uint8_t> result;
            if (!controller.Request(packet.data(), MT_Ack, result, 3000) || result.size() != sizeof(i))
                return false;
            uint64_t got = 0;
            memcpy(&got, result.data(), sizeof(got));
            return got == i;
        }));
    bool matched = true;
    for (auto &r : requests)
        matched = r.get() && matched;
    CHECK(matched);
    // Large Data stream and intentionally slow consumption must not stall C/T.
    slowMap = true;
    MapReqMessage map;
    CHECK(data.SendPackage(&map));
    CHECK(Wait([&] { return slowRows.load() > 10; }));
    auto start = GetTickCount64();
    CHECK(controller.Request(&position, MT_PositionReply, body, 1000));
    CHECK(GetTickCount64() - start < 500);
    int eventBefore = events;
    instance->Publish(MT_NavStatus, &event, sizeof(event));
    CHECK(Wait([&] { return events.load() > eventBefore; }, 500));
    CHECK(Wait([&] { return slowRows.load() == 2500; }, 50000));
    slowMap = false;
    // A silent raw client expires while active registered roles stay alive.
    {
        Raw silent;
        CHECK(silent.Open(name, Role::Observer, pipeio::NewId()));
        Route old{silent.welcome.clientId, silent.welcome.sessionId, 1};
        CHECK(instance->IsRouteCurrent(old));
        CHECK(Wait([&] { return !instance->IsRouteCurrent(old); }, 11500));
    }
    CHECK(pipe::GetConnectionSnapshot().Same(initial));
    // Exercise the real navigation loop with deterministic synthetic coordinates.
    {
        std::lock_guard<std::mutex> l(state);
        x = tx = 10;
        y = ty = 10;
        timeline.clear();
    }
    walks = 0;
    positions = 0;
    mapdata::CostBuffer cost(mapdata::Geometry(mapdata::GridSize{23, 2}));
    cost.Raw().assign(cost.Size(), 1);
    usecase::NavigateOptions options;
    options.goalX = 400;
    options.goalY = 10;
    options.stepDistance = 126;
    options.replanMs = 150;
    options.arriveTolerance = 13;
    options.presetCost = &cost;
    usecase::NavTelemetry telemetry;
    CHECK(usecase::Navigate(options, &telemetry) == 0);
    CHECK(telemetry.arrived && walks == 3 && positions >= 4 && telemetry.replans == 1);
    bool ordered = true;
    uint64_t lastCall = 0;
    int pairs = 0;
    {
        std::lock_guard<std::mutex> l(state);
        for (auto &t : timeline)
        {
            if (t.type == MT_WalkCall)
                lastCall = t.at;
            if (t.type == MT_PositionReq && lastCall)
            {
                printf("INTERVAL call_to_position_ms=%llu\n", t.at - lastCall);
                ordered = ordered && (t.at - lastCall >= options.pollMs);
                ++pairs;
                lastCall = 0;
            }
        }
    }
    CHECK(ordered && pairs == 3);
    printf("SCHEDULE: cached route -> WalkCall -> fresh position observation -> next WalkCall\n");
    usecase::NavigateOptions fromIni;
    CHECK(usecase::LoadNavOptionsFromIni(fromIni) >= 3);
    CHECK(fromIni.stepDistance == 126 && fromIni.replanMs == 800 && fromIni.arriveTolerance == 13);
    // Same named endpoint, new server incarnation: old routes and handles must fail.
    Route oldRoute;
    {
        std::lock_guard<std::mutex> l(state);
        oldRoute = saved;
    }
    instance->Uninitialize();
    CHECK(Wait([&] { return !pipe::GetConnectionSnapshot().ready; }));
    CHECK(!controller.IsConnected());
    instance = std::make_unique<NamedPipeServer>(name, Handler);
    server = instance.get();
    CHECK(instance->Initialize());
    CHECK(Wait([&] { return pipe::GetConnectionSnapshot().ready; }, 7000));
    auto next = pipe::GetConnectionSnapshot();
    CHECK(next.sessionId != initial.sessionId);
    CHECK(!instance->Reply(oldRoute, MT_Ack, nullptr, 0));
    CHECK(!controller.SendPackage(&walk));
    int noReplay = walks;
    Sleep(250);
    CHECK(walks == noReplay);
    pipe::NamedPipeClie rebound(name, nullptr);
    CHECK(rebound.Initialize());
    CHECK(rebound.Request(&position, MT_PositionReply, body));
    CHECK(!next.Same(initial));
    options.presetSessionId = initial.sessionId;
    int count = walks;
    CHECK(usecase::Navigate(options, &telemetry) == 2);
    CHECK(walks == count);
    tracker.Uninitialize();
    controller.Uninitialize();
    data.Uninitialize();
    observer.Uninitialize();
    rebound.Uninitialize();
    pipe::StopManagedConnections();
    instance->Uninitialize();
    printf("RESULT passed=%d failed=%d\n", pass, fail);
    return fail ? 1 : 0;
}
