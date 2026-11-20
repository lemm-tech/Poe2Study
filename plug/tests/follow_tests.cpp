// Isolated end-to-end follow tests. No live game, DLL loading, or injection.
#include "../Plug/FollowRunner.h"
#include "../Plug/Navigator.h"
#include "../Plug/NamedPipeClie.h"
#include "../Monitor/NamedPipeServer.h"
#include "../Common/AppConfig.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <windows.h>

namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { const bool ok = !!(x); printf("[%s] %s\n", ok ? "PASS" : "FAIL", #x); fflush(stdout); ok ? ++passed : ++failed; } while (0)
struct World {
    std::mutex mutex;
    double x = 10, y = 10, tx = 10, ty = 10;
    int calls = 0;
    uint64_t tick = 0;
    std::vector<nav::PixelPoint> targets;
    std::vector<uint64_t> times;
    void Reset() { std::lock_guard<std::mutex> lock(mutex); x = tx = 10; y = ty = 10; calls = 0; targets.clear(); times.clear(); tick = GetTickCount64(); }
    void Advance() {
        const auto now = GetTickCount64(); const double d = std::hypot(tx - x, ty - y);
        const double move = (std::min)(d, (now - tick) * .2); tick = now;
        if (d > 0) { x += (tx - x) * move / d; y += (ty - y) * move / d; }
    }
    int Calls() { std::lock_guard<std::mutex> lock(mutex); return calls; }
} world;
NamedPipeServer* server = nullptr;
void Handle(MessageType type, const void* body, size_t size, const managed::Route& route) {
    if (type == MT_PositionReq) {
        NavStatus value{};
        { std::lock_guard<std::mutex> lock(world.mutex); world.Advance(); value.x = int32_t(std::lround(world.x)); value.y = int32_t(std::lround(world.y)); }
        server->Reply(route, MT_PositionReply, &value, sizeof(value));
    } else if (type == MT_WalkCall && size == sizeof(WalkCallReq)) {
        WalkCallReq request{}; std::memcpy(&request, body, size); WalkCallAck reply{};
        { std::lock_guard<std::mutex> lock(world.mutex); world.Advance(); world.tx = request.x; world.ty = request.y;
          ++world.calls; world.targets.push_back({request.x, request.y}); world.times.push_back(GetTickCount64()); }
        reply.targetX = request.x; reply.targetY = request.y;
        server->Reply(route, MT_WalkCallAck, &reply, sizeof(reply));
    }
}
template<class F> bool Until(F condition, uint32_t timeout = 4000) {
    const auto end = GetTickCount64() + timeout;
    while (GetTickCount64() < end) { if (condition()) return true; Sleep(10); }
    return condition();
}
struct Source {
    std::mutex mutex;
    follow::Snapshot value;
    follow::Feedback feedback;
    bool stale = false;
    follow::Channel channel;
    Source() {
        channel.options.leaderName = "队长测试Alpha"; channel.options.distance = 10;
        channel.options.moveThreshold = 5; channel.options.replanMs = 100;
        value.valid = true; value.position = {70, 10}; value.width = value.height = 92;
        value.entity = 123; value.mapToken = 456; value.sequence = 1;
        channel.sample = [&] { std::lock_guard<std::mutex> lock(mutex); auto out = value;
            out.sampledAt = GetTickCount64() - (stale ? 1000 : 0); return out; };
        channel.publish = [&](const follow::Feedback& next) { std::lock_guard<std::mutex> lock(mutex); feedback = next; };
    }
    void Target(int x, int y = 10) { std::lock_guard<std::mutex> lock(mutex); value.position = {x, y}; value.valid = true; ++value.sequence; }
    void Valid(bool valid) { std::lock_guard<std::mutex> lock(mutex); value.valid = valid; ++value.sequence; }
    follow::Feedback Get() { std::lock_guard<std::mutex> lock(mutex); return feedback; }
};
mapdata::CostBuffer OpenMap() {
    mapdata::CostBuffer cost; cost.Resize(mapdata::Geometry::FromGrid(4, 4));
    cost.Raw().assign(cost.Size(), 1); return cost;
}
usecase::NavigateOptions NavOptions(const mapdata::CostBuffer& map, const follow::Channel& channel) {
    usecase::NavigateOptions options; options.presetCost = &map; options.follow = &channel;
    options.pollMs = 20; options.minCallMs = 100; options.startupMs = 100;
    options.stepDistance = 25; options.waypointTolerance = 1; options.stepWaitMs = 1000;
    options.replanMs = 150; options.stallSec = 1; options.walkTimeoutSec = 1;
    return options;
}
} // namespace
int main() {
    std::string error; follow::Options defaults;
    CHECK(defaults.distance == 60 && defaults.ResumeDistance() == 90 && defaults.sampleMs == 100 && defaults.replanMs == 250);
    defaults.leaderName = "队长测试Alpha";
    CHECK(follow::SaveConfig(defaults, error));
    follow::Options loaded; CHECK(follow::LoadConfig(loaded, error) && loaded.leaderName == defaults.leaderName && loaded.distance == 60);
    defaults.distance = 85; CHECK(follow::SaveConfig(defaults, error));
    CHECK(follow::LoadConfig(loaded, error) && loaded.distance == 85 && loaded.ResumeDistance() == 127);
    defaults.distance = 0; CHECK(!follow::SaveConfig(defaults, error));
    follow::Snapshot sample; sample.valid = true; sample.sampledAt = 1000;
    CHECK(follow::Fresh(sample, 1300, 300) && !follow::Fresh(sample, 1301, 300) && !follow::Fresh(sample, 999, 300));
    defaults.distance = 60;
    CHECK(follow::HoldDistance(60, false, defaults) && !follow::HoldDistance(61, false, defaults));
    CHECK(follow::HoldDistance(89, true, defaults) && !follow::HoldDistance(91, true, defaults));
    CHECK(!follow::HoldDistance(std::numeric_limits<double>::infinity(), false, defaults));
    CHECK(!follow::RetargetDue({10,10}, {40,10}, 1100, 1000, defaults));
    CHECK(follow::RetargetDue({10,10}, {40,10}, 1300, 1000, defaults));

    app::Config().pipeName = "Poe2FollowTests_" + std::to_string(GetCurrentProcessId());
    const auto name = app::Config().PipeFullName();
    NamedPipeServer instance(name, Handle); server = &instance;
    if (!instance.Initialize() || !pipe::StartManagedConnections(name)) return 2;
    const auto map = OpenMap(); const auto original = map.Raw();
    {
        world.Reset(); Source source; std::atomic_bool stop{false}; usecase::NavTelemetry telemetry;
        auto options = NavOptions(map, source.channel);
        auto job = std::async(std::launch::async, [&] { return usecase::Navigate(options, &telemetry, &stop); });
        CHECK(Until([&] { return source.Get().state == follow::State::Waiting && world.Calls() > 0; }));
        CHECK(source.Get().distance >= 8 && source.Get().distance <= 12);
        Sleep(120); const auto before = world.Calls();
        // Waiting is persistent and exempt from both the 1s arrival/run timeout and stall watchdog.
        Sleep(1200); CHECK(world.Calls() == before && job.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
        source.Target(76); Sleep(250); CHECK(world.Calls() == before);
        source.Target(85); CHECK(Until([&] { return world.Calls() > before; }));
        CHECK(Until([&] { return source.Get().state == follow::State::Waiting && source.Get().plans >= 2; }));
        source.Valid(false); CHECK(Until([&] { return source.Get().state == follow::State::Lost; }));
        const auto lostCalls = world.Calls(); Sleep(200); CHECK(world.Calls() == lostCalls);
        source.Target(20); CHECK(Until([&] { return world.Calls() > lostCalls && source.Get().plans >= 3; }));
        { std::lock_guard<std::mutex> lock(source.mutex); source.value.mapToken++; }
        CHECK(Until([&] { return job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
        stop = true; CHECK(job.get() == 1 && telemetry.note.find("地图") != std::string::npos);
        bool cadence = true;
        { std::lock_guard<std::mutex> lock(world.mutex);
          for (size_t i = 1; i < world.times.size(); ++i) cadence &= world.times[i] - world.times[i-1] >= 100; }
        CHECK(cadence);
    }
    {
        world.Reset(); Source source; source.stale = true; std::atomic_bool stop{false}; usecase::NavTelemetry telemetry;
        auto options = NavOptions(map, source.channel);
        auto job = std::async(std::launch::async, [&] { return usecase::Navigate(options, &telemetry, &stop); });
        Sleep(250); CHECK(world.Calls() == 0);
        { std::lock_guard<std::mutex> lock(source.mutex); source.stale = false; }
        CHECK(Until([&] { return world.Calls() > 0; }));
        stop = true; CHECK(job.get() == 3);
        const auto count = world.Calls(); Sleep(150); CHECK(world.Calls() == count);
    }
    {
        // Close across a wall: proximity cannot suppress the required detour.
        auto wall = OpenMap(); wall.Raw().assign(wall.Size(), 0);
        for (int y = 10; y <= 40; ++y) { wall.Set(10, y, 1); wall.Set(20, y, 1); }
        for (int x = 10; x <= 20; ++x) wall.Set(x, 40, 1);
        world.Reset(); Source source; source.Target(20); source.channel.options.distance = 12;
        std::atomic_bool stop{false}; usecase::NavTelemetry telemetry;
        auto options = NavOptions(wall, source.channel); options.stepDistance = 8;
        auto job = std::async(std::launch::async, [&] { return usecase::Navigate(options, &telemetry, &stop); });
        CHECK(Until([&] { return world.Calls() > 0; }));
        { std::lock_guard<std::mutex> lock(world.mutex); CHECK(!world.targets.empty() && world.targets.front().x == 10 && world.targets.front().y > 10); }
        stop = true; CHECK(job.get() == 3);
    }
    CHECK(map.Raw() == original);
    pipe::StopManagedConnections(); instance.Uninitialize();
    printf("RESULT passed=%d failed=%d\n", passed, failed); return failed ? 1 : 0;
}
