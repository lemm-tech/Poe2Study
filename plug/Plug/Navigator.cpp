#include "Navigator.h"
#include "../Common/AppConfig.h"
#include "../Common/Logger.h"
#include "BmpWriter.h"
#include "NamedPipeClie.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <share.h>
#include <windows.h>

namespace usecase
{
namespace
{
std::mutex g_state, g_operation, g_trackerMutex, g_fetchMutex;

// Per-navigation trace. A dedicated file survives GUI log rotation and can be
// tailed while the run is active; each event is flushed immediately.
class NavTrace
{
public:
    NavTrace()
    {
        static std::atomic<uint32_t> next{1};
        uint32_t value = next.fetch_add(1, std::memory_order_relaxed);
        m_runId = static_cast<uint16_t>(value & 0xFFFFu);
        if (!m_runId)
            m_runId = 1;
        m_started = GetTickCount64();
    }

    ~NavTrace()
    {
        if (m_file)
        {
            fflush(m_file);
            fclose(m_file);
        }
    }

    bool Open()
    {
        char exe[MAX_PATH]{};
        const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (!len || len >= MAX_PATH)
            return false;
        std::string base(exe, len);
        const size_t slash = base.find_last_of("\\/");
        if (slash == std::string::npos)
            return false;
        base.resize(slash + 1);
        const std::string dir = app::Config().RuntimeFile("nav_logs");
        if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;

        SYSTEMTIME now{};
        GetLocalTime(&now);
        char name[128]{};
        _snprintf_s(name, sizeof(name), _TRUNCATE,
                    "nav_trace_%04u%02u%02u_%02u%02u%02u_%lu_%04X.csv",
                    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                    static_cast<unsigned long>(GetCurrentProcessId()), m_runId);
        m_path = dir + "\\" + name;
        m_file = _fsopen(m_path.c_str(), "wb", _SH_DENYWR);
        if (!m_file)
            return false;
        fwrite("\xEF\xBB\xBF", 1, 3, m_file);
        fputs("timestamp,elapsed_ms,run_id,event,detail\n", m_file);
        fflush(m_file);
        Event("TRACE_OPEN", "pid=%lu thread=%lu exe=%s",
              static_cast<unsigned long>(GetCurrentProcessId()),
              static_cast<unsigned long>(GetCurrentThreadId()), exe);
        return true;
    }

    uint16_t RunId() const { return m_runId; }
    uint64_t StartedAt() const { return m_started; }
    const std::string& Path() const { return m_path; }

    void Event(const char* event, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        WriteEvent(event, format, args, true);
        va_end(args);
    }

    // Large waypoint lists are buffered as a single block so diagnostics do not
    // add one disk flush per point before issuing the next movement call.
    void EventBuffered(const char* event, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        WriteEvent(event, format, args, false);
        va_end(args);
    }

    void Flush()
    {
        if (m_file) fflush(m_file);
    }

private:
    void WriteEvent(const char* event, const char* format, va_list args, bool flush)
    {
        if (!m_file)
            return;
        char detail[3072]{};
        _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, format, args);

        SYSTEMTIME now{};
        GetLocalTime(&now);
        fprintf(m_file, "%04u-%02u-%02uT%02u:%02u:%02u.%03u,%llu,%04X,%s,\"",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                static_cast<unsigned long long>(GetTickCount64() - m_started), m_runId, event ? event : "UNKNOWN");
        for (const char* p = detail; *p; ++p)
        {
            if (*p == '\"')
                fputc('\"', m_file);
            if (*p == '\r')
                fputs("\\r", m_file);
            else if (*p == '\n')
                fputs("\\n", m_file);
            else
                fputc(*p, m_file);
        }
        fputs("\"\n", m_file);
        if (flush) fflush(m_file);
    }

    FILE* m_file = nullptr;
    uint64_t m_started = 0;
    uint16_t m_runId = 0;
    std::string m_path;
};

uint64_t CostFingerprint(const mapdata::CostBuffer& cost)
{
    // FNV-1a 64-bit fingerprint of the exact received cost buffer.
    uint64_t hash = 14695981039346656037ull;
    const auto& raw = cost.Raw();
    for (uint8_t byte : raw)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

int NearestRoutePoint(const std::vector<NavBatchChunk>& points, int32_t x, int32_t y, double& distance)
{
    int best = -1;
    double bestSq = -1.0;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const double dx = static_cast<double>(points[i].x) - x;
        const double dy = static_cast<double>(points[i].y) - y;
        const double d2 = dx * dx + dy * dy;
        if (bestSq < 0.0 || d2 < bestSq)
        {
            bestSq = d2;
            best = static_cast<int>(i);
        }
    }
    distance = bestSq < 0.0 ? -1.0 : std::sqrt(bestSq);
    return best;
}
pipe::ConnectionSnapshot g_positionConnection;
mapsink::PipeSink g_sink;
std::atomic_bool g_mapReady{false}, g_stop{false};
NavStatus g_pos{};
uint64_t g_posAt = 0, g_posSerial = 0;
std::unique_ptr<pipe::NamedPipeClie> g_tracker;
void Signal(int)
{
    g_stop = true;
}
void OnMessage(MessageType type, const void *data, size_t size)
{
    std::lock_guard<std::mutex> lock(g_state);
    if (type == MT_NavStatus && size >= sizeof(NavStatus))
    {
        memcpy(&g_pos, data, sizeof(g_pos));
        g_posAt = GetTickCount64();
        ++g_posSerial;
        g_positionConnection = pipe::GetConnectionSnapshot();
    }
    else if (type == MT_MapMeta && size >= sizeof(MapMeta))
    {
        MapMeta v{};
        memcpy(&v, data, sizeof(v));
        mapdata::MapFrame::Meta m{};
        m.width = v.width;
        m.height = v.height;
        m.rowStride = v.rowStride;
        m.layer = v.layer;
        std::string e;
        g_mapReady = false;
        if (!g_sink.Begin(m, e))
            LOGE("[PixelNav] map: %s", e.c_str());
    }
    else if (type == MT_MapRow && size >= sizeof(MapRowHeader))
    {
        MapRowHeader h{};
        memcpy(&h, data, sizeof(h));
        mapdata::MapFrame::RowSlice row;
        row.y = h.rowIndex;
        row.off = h.offset;
        row.data = static_cast<const uint8_t *>(data) + sizeof(h);
        row.len = uint32_t(size - sizeof(h));
        if (g_sink.Feed(row) && g_sink.Complete())
            g_mapReady = true;
    }
}
bool Position(NavStatus &p, uint64_t *serial = nullptr)
{
    std::lock_guard<std::mutex> l(g_state);
    if (!g_posAt || GetTickCount64() - g_posAt > 500 || !g_positionConnection.Same(pipe::GetConnectionSnapshot()))
        return false;
    p = g_pos;
    if (serial)
        *serial = g_posSerial;
    return true;
}
bool Stopped(std::atomic_bool *ext)
{
    return g_stop.load() || (ext && ext->load());
}
bool Pause(uint32_t ms, std::atomic_bool *ext)
{
    uint64_t end = GetTickCount64() + ms;
    while (GetTickCount64() < end)
    {
        if (Stopped(ext))
            return false;
        Sleep(10);
    }
    return !Stopped(ext);
}
bool ReadCurrent(pipe::NamedPipeClie &c, NavStatus &p, std::atomic_bool *ext, NavTrace* trace, const char* phase)
{
    if (Stopped(ext))
    {
        if (trace) trace->Event("POSITION_CANCELLED", "phase=%s", phase ? phase : "unknown");
        return false;
    }
    PacketHeader req{};
    req.m_MsgType = MT_PositionReq;
    std::vector<uint8_t> body;
    const uint64_t started = GetTickCount64();
    const bool received = c.Request(&req, MT_PositionReply, body, 2000);
    const uint64_t elapsed = GetTickCount64() - started;
    if (!received || body.size() != sizeof(p))
    {
        if (trace) trace->Event("POSITION_REQUEST_FAILED",
                                "phase=%s received=%d bytes=%zu expected=%zu connected=%d elapsed_ms=%llu",
                                phase ? phase : "unknown", received ? 1 : 0, body.size(), sizeof(p),
                                c.IsConnected() ? 1 : 0, static_cast<unsigned long long>(elapsed));
        return false;
    }
    memcpy(&p, body.data(), sizeof(p));
    if (trace) trace->Event("POSITION_REPLY", "phase=%s x=%d y=%d state=%u elapsed_ms=%llu",
                            phase ? phase : "unknown", p.x, p.y, static_cast<unsigned>(p.state),
                            static_cast<unsigned long long>(elapsed));
    return !Stopped(ext);
}
int Fetch(pipe::NamedPipeClie &c, mapdata::CostBuffer &out, uint32_t seconds, std::atomic_bool *ext, NavTrace* trace = nullptr)
{
    std::lock_guard<std::mutex> fetch(g_fetchMutex);
    g_mapReady = false;
    MapReqMessage req;
    const uint64_t started = GetTickCount64();
    if (trace) trace->Event("MAP_REQUEST", "timeout_sec=%u connected=%d", seconds, c.IsConnected() ? 1 : 0);
    if (!c.SendPackage(&req))
    {
        if (trace) trace->Event("MAP_REQUEST_FAILED", "stage=send connected=%d", c.IsConnected() ? 1 : 0);
        return 1;
    }
    uint64_t end = GetTickCount64() + uint64_t(seconds) * 1000;
    while (!g_mapReady)
    {
        if (GetTickCount64() > end || !c.IsConnected() || !Pause(20, ext))
        {
            if (trace) trace->Event("MAP_REQUEST_FAILED",
                                    "stage=wait ready=%d connected=%d stopped=%d elapsed_ms=%llu",
                                    g_mapReady.load() ? 1 : 0, c.IsConnected() ? 1 : 0, Stopped(ext) ? 1 : 0,
                                    static_cast<unsigned long long>(GetTickCount64() - started));
            return 2;
        }
    }
    {
        std::lock_guard<std::mutex> l(g_state);
        out = g_sink.Result();
    }
    if (trace) trace->Event("MAP_RECEIVED", "empty=%d bytes=%zu world=%ux%u cost_hash=%016llX elapsed_ms=%llu",
                            out.Empty() ? 1 : 0, out.Size(), out.Empty() ? 0 : out.Geo().WorldW(),
                            out.Empty() ? 0 : out.Geo().WorldH(),
                            static_cast<unsigned long long>(out.Empty() ? 0 : CostFingerprint(out)),
                            static_cast<unsigned long long>(GetTickCount64() - started));
    return out.Empty() ? 2 : 0;
}
bool SendWalk(pipe::NamedPipeClie &c, nav::PixelPoint target, std::atomic_bool *ext, uint64_t &issuedAt,
              NavTrace* trace, uint16_t callId)
{
    if (Stopped(ext))
    {
        if (trace) trace->Event("WALK_CANCELLED", "call=%u target=(%d,%d)", callId, target.x, target.y);
        return false;
    }
    WalkCallReq req{};
    req.x = target.x;
    req.y = target.y;
    req.traceRunId = trace ? trace->RunId() : 0;
    req.traceCallId = callId;
    WalkCallMessage msg(req);
    issuedAt = GetTickCount64();
    if (trace) trace->Event("WALK_REQUEST", "call=%u target=(%d,%d) connected=%d", callId, target.x, target.y,
                            c.IsConnected() ? 1 : 0);
    std::vector<uint8_t> body;
    const uint64_t requestAt = GetTickCount64();
    const bool received = c.Request(&msg, MT_WalkCallAck, body, 3000);
    const uint64_t requestElapsed = GetTickCount64() - requestAt;
    if (!received || body.size() != sizeof(WalkCallAck))
    {
        if (trace) trace->Event("WALK_ACK_MISSING",
                                "call=%u received=%d bytes=%zu expected=%zu connected=%d elapsed_ms=%llu",
                                callId, received ? 1 : 0, body.size(), sizeof(WalkCallAck), c.IsConnected() ? 1 : 0,
                                static_cast<unsigned long long>(requestElapsed));
        LOGW("[PixelNav][run=%04X call=%u] walk ack missing received=%d bytes=%zu elapsed=%llu connected=%d",
             trace ? trace->RunId() : 0, callId, received ? 1 : 0, body.size(),
             static_cast<unsigned long long>(requestElapsed), c.IsConnected() ? 1 : 0);
        return false;
    }
    WalkCallAck ack{};
    memcpy(&ack, body.data(), sizeof(ack));
    const bool targetMatches = ack.targetX == target.x && ack.targetY == target.y;
    if (trace) trace->Event("WALK_ACK",
                            "call=%u requested=(%d,%d) ack_target=(%d,%d) ok=%d result=%d player_after=(%d,%d) dist_before=%d dist_after=%d call_elapsed_ms=%u pipe_elapsed_ms=%llu target_match=%d",
                            callId, target.x, target.y, ack.targetX, ack.targetY, ack.ok, ack.result,
                            ack.playerX, ack.playerY, ack.distBefore, ack.distAfter, ack.elapsedMs,
                            static_cast<unsigned long long>(requestElapsed), targetMatches ? 1 : 0);
    LOGI("[PixelNav][run=%04X call=%u] call (%d,%d) ok=%d result=%d player_after=(%d,%d) dist=%d->%d call_ms=%u pipe_ms=%llu",
         trace ? trace->RunId() : 0, callId, target.x, target.y, ack.ok, ack.result, ack.playerX, ack.playerY,
         ack.distBefore, ack.distAfter, ack.elapsedMs, static_cast<unsigned long long>(requestElapsed));
    issuedAt = GetTickCount64(); // T begins after the actual call acknowledgement, not queue submission.
    return targetMatches && ack.ok == 0 && ack.result == 0 && !Stopped(ext);
}
void SavePath(const mapdata::CostBuffer &cost, const std::vector<NavBatchChunk> &pts, const std::string &path)
{
    if (path.empty() || cost.Empty())
        return;
    uint32_t w = cost.Geo().WorldW(), h = cost.Geo().WorldH();
    std::vector<uint8_t> b(cost.Size());
    for (size_t i = 0; i < b.size(); ++i)
        b[i] = cost.Raw()[i] ? 220 : 0;
    for (auto p : pts)
        if (cost.Geo().Contains(p.x, p.y))
            b[size_t(p.y) * w + p.x] = 120;
    bmp::WriteGray8(path, b.data(), w, h);
}
} // namespace

bool PlanPath(const mapdata::CostBuffer &cost, nav::PixelMap *preset, int32_t x, int32_t y, int32_t gx, int32_t gy,
              const NavigateOptions &opt, std::vector<NavBatchChunk> &out, int32_t &outX, int32_t &outY,
              std::string &err)
{
    out.clear();
    err.clear();
    outX = gx;
    outY = gy;
    nav::PixelMap local;
    nav::PixelMap *map = preset;
    if (!map || map->Empty())
    {
        if (!local.BuildFrom(cost))
        {
            err = "empty white map";
            return false;
        }
        map = &local;
    }
    if (!map->Passable(gx, gy))
    {
        err = "goal is black/outside; select a white coordinate";
        return false;
    }
    nav::PixelPath path;
    if (!nav::FindPixelPath(*map, {x, y}, {gx, gy}, path))
    {
        err = path.error;
        return false;
    }
    nav::PixelRoute route;
    if (!route.Build(*map, path.points)) { err = "route smoothing failed"; return false; }
    const auto &pts = route.Points();
    if (pts.empty() || pts.size() > opt.maxWaypoints)
    {
        err = "invalid waypoint budget";
        return false;
    }
    for (auto p : pts)
        out.push_back({p.x, p.y});
    LOGI("[PixelNav] white=%zu expanded=%zu raw=%zu targets=%zu length=%.1f", map->PassableCount(), path.expanded,
         path.points.size(), pts.size(), path.length);
    return true;
}
int FetchCostBuffer(mapdata::CostBuffer &out, const char *note, std::atomic_bool *stopFlag)
{

    pipe::NamedPipeClie c(app::Config().PipeFullName(), OnMessage, managed::Role::Data);
    if (!c.Initialize())
        return 1;
    int rc = Fetch(c, out, 30, stopFlag);
    c.Uninitialize();
    if (!rc)
        LOGI("[PixelNav] map %ux%u %s", out.Geo().WorldW(), out.Geo().WorldH(), note ? note : "");
    return rc;
}
bool StartPlayerTracker(const std::string &name)
{
    std::lock_guard<std::mutex> l(g_trackerMutex);
    if (g_tracker)
        return true;
    int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (n < 2)
        return false;
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &w[0], n);
    w.pop_back();
    auto c = std::make_unique<pipe::NamedPipeClie>(w, OnMessage, managed::Role::Tracker);
    if (!c->Initialize())
        return false;
    g_tracker = std::move(c);
    return true;
}
void StopPlayerTracker()
{
    std::lock_guard<std::mutex> l(g_trackerMutex);
    g_tracker.reset();
    std::lock_guard<std::mutex> s(g_state);
    g_posAt = 0;
    g_positionConnection = {};
}
bool GetLastPlayerPos(int32_t &x, int32_t &y)
{
    NavStatus p{};
    if (!Position(p))
        return false;
    x = p.x;
    y = p.y;
    return true;
}

int Navigate(const NavigateOptions &opt, NavTelemetry *telemetry, std::atomic_bool *ext,
             const std::atomic<NavStopReason> *stopReason)
{
    std::lock_guard<std::mutex> operation(g_operation);
    nav::PixelPoint goal{opt.follow ? 0 : opt.goalX, opt.follow ? 0 : opt.goalY};
    follow::Feedback followFeedback;
    NavTrace trace;
    const bool traceReady = trace.Open();
    if (traceReady)
        LOGI("[PixelNav][run=%04X] trace=%s", trace.RunId(), trace.Path().c_str());
    else
        LOGE("[PixelNav][run=%04X] 无法创建逐轮诊断日志（exe目录下 nav_logs）；继续使用 navgui_log.txt", trace.RunId());

    NavTelemetry t;
    t.running = true;
    t.goalX = opt.goalX;
    t.goalY = opt.goalY;
    NavStatus lastPos{};
    bool haveLastPos = false;
    pipe::NamedPipeClie* stopClient = nullptr;
    bool finalStopSent = opt.dryRun;
    nav::PixelPoint lastWalkTarget{};
    bool haveWalkTarget = false;
    std::vector<NavBatchChunk> lastRoute;
    uint32_t callCount = 0;
    uint32_t planAttempts = 0;
    uint64_t previousSampleTick = 0;
    int32_t previousGoalDistance = -1;
    auto* stationary = !opt.dryRun && !opt.singleShot && !opt.follow ? opt.stationaryRecovery : nullptr;
    struct RecoveryGuard {
        StationaryRecovery* recovery;
        ~RecoveryGuard() { if (recovery) recovery->timer.Suspend(); }
    } recoveryGuard{stationary};

    auto publish = [&]() {
        if (telemetry)
            *telemetry = t;
        if (opt.follow && opt.follow->publish) {
            followFeedback.player = {t.playerX, t.playerY};
            followFeedback.plans = t.replans; followFeedback.calls = callCount;
            opt.follow->publish(followFeedback);
        }
    };
    auto logPosition = [&](const char* phase, const NavStatus& p) {
        const uint64_t now = GetTickCount64();
        const int32_t goalDistance = int32_t(std::lround(nav::PixelDistance({p.x, p.y}, goal)));
        const int32_t dx = haveLastPos ? p.x - lastPos.x : 0;
        const int32_t dy = haveLastPos ? p.y - lastPos.y : 0;
        const int32_t progress = previousGoalDistance < 0 ? 0 : previousGoalDistance - goalDistance;
        double routeDistance = -1.0;
        const int nearestRoute = NearestRoutePoint(lastRoute, p.x, p.y, routeDistance);
        const uint64_t sampleGap = previousSampleTick ? now - previousSampleTick : 0;
        trace.Event("POSITION_SAMPLE",
                    "phase=%s x=%d y=%d dx=%d dy=%d goal=(%d,%d) dist_goal=%d goal_progress=%d sample_gap_ms=%llu route_nearest=%d route_distance=%.2f route_count=%zu walk_target=%s(%d,%d) state=%u",
                    phase ? phase : "unknown", p.x, p.y, dx, dy, goal.x, goal.y, goalDistance, progress,
                    static_cast<unsigned long long>(sampleGap), nearestRoute, routeDistance, lastRoute.size(),
                    haveWalkTarget ? "set" : "none", lastWalkTarget.x, lastWalkTarget.y,
                    static_cast<unsigned>(p.state));
        previousSampleTick = now;
        previousGoalDistance = goalDistance;
        lastPos = p;
        haveLastPos = true;
        t.playerX = p.x;
        t.playerY = p.y;
        t.distToGoal = goalDistance;
    };
    publish();
    trace.Event("RUN_START",
                "goal=(%d,%d) dry_run=%d step_distance=%d replan_ms=%u arrive_tolerance=%d map_timeout_sec=%u walk_timeout_sec=%u stall_sec=%u max_no_path=%u max_waypoints=%zu single_shot=%d preset_map=%d",
                opt.goalX, opt.goalY, opt.dryRun ? 1 : 0, opt.stepDistance, opt.replanMs,
                static_cast<int>(opt.arriveTolerance), opt.mapTimeoutSec, opt.walkTimeoutSec, opt.stallSec,
                opt.maxNoPath, opt.maxWaypoints, opt.singleShot ? 1 : 0, opt.presetCost ? 1 : 0);
    LOGI("[PixelNav][run=%04X] start goal=(%d,%d) dry=%d step=%d replan=%u tolerance=%d stall=%u no_path=%u",
         trace.RunId(), opt.goalX, opt.goalY, opt.dryRun ? 1 : 0, opt.stepDistance,
         opt.replanMs, static_cast<int>(opt.arriveTolerance), opt.stallSec, opt.maxNoPath);

    auto finish = [&](int rc, const char *why) {
        t.running = false;
        t.note = why ? why : "unknown";
        t.arrived = rc == 0 && !opt.dryRun;
        t.stuck = rc == 4;
        t.state = rc == 0 ? (opt.dryRun ? NAV_IDLE : NAV_ARRIVED) : (rc == 4 ? NAV_STUCK : NAV_FAILED);
        const int32_t finalDistance = haveLastPos
            ? int32_t(std::lround(nav::PixelDistance({lastPos.x, lastPos.y}, goal))) : -1;
        if (stopClient && !opt.dryRun && !finalStopSent)
        {
            NavStopMessage s;
            const bool sent = stopClient->SendPackage(&s);
            trace.Event("FINAL_NAV_STOP", "sent=%d", sent ? 1 : 0);
            finalStopSent = true;
        }
        trace.Event("RUN_FINISH",
                    "rc=%d state=%u reason=%s final_pos=%s(%d,%d) goal=(%d,%d) dist_goal=%d replans=%u calls=%u no_path_attempts=%u elapsed_ms=%llu arrived=%d stuck=%d",
                    rc, t.state, t.note.c_str(), haveLastPos ? "known" : "unknown",
                    haveLastPos ? lastPos.x : 0, haveLastPos ? lastPos.y : 0, goal.x, goal.y,
                    finalDistance, t.replans, callCount, planAttempts,
                    static_cast<unsigned long long>(GetTickCount64() - trace.StartedAt()), t.arrived ? 1 : 0, t.stuck ? 1 : 0);
        publish();
        LOGI("[PixelNav][run=%04X] finish rc=%d state=%u reason=%s pos=(%d,%d) dist_goal=%d replans=%u calls=%u elapsed_ms=%llu",
             trace.RunId(), rc, t.state, t.note.c_str(), haveLastPos ? lastPos.x : 0,
             haveLastPos ? lastPos.y : 0, finalDistance, t.replans, callCount,
             static_cast<unsigned long long>(GetTickCount64() - trace.StartedAt()));
        return rc;
    };

    std::string followOptionError;
    if ((opt.follow && (!opt.follow->sample || (opt.follow->loot && !opt.follow->pick) ||
        !follow::Validate(opt.follow->options, followOptionError) || opt.dryRun || opt.singleShot)) ||
        (!opt.follow && (opt.goalX < 0 || opt.goalY < 0)) || opt.stepDistance < 1 || opt.arriveTolerance < 1 || opt.replanMs < 150 ||
        !opt.stallSec || !opt.walkTimeoutSec || opt.pollMs < 20 || opt.pollMs > 1000 ||
        opt.minCallMs < 100 || opt.stepWaitMs < opt.minCallMs || opt.startupMs > opt.stepWaitMs ||
        !opt.stillSamples || !opt.stallLimit || !opt.positionRetries || !opt.recoveryLimit ||
        !opt.maxNoPath || !opt.maxWaypoints || (stationary && !stationary->perform) ||
        opt.waypointTolerance < 1 || opt.deviateDistance < 1)
    {
        trace.Event("OPTIONS_INVALID", "goal=(%d,%d) step=%d replan=%u tolerance=%d stall=%u timeout=%u",
                    opt.goalX, opt.goalY, opt.stepDistance, opt.replanMs, static_cast<int>(opt.arriveTolerance),
                    opt.stallSec, opt.walkTimeoutSec);
        return finish(2, "invalid world-coordinate navigation options");
    }
    g_stop = false;
    auto old = std::signal(SIGINT, Signal);
    struct SignalGuard
    {
        decltype(old) f;
        ~SignalGuard()
        {
            std::signal(SIGINT, f);
        }
    } guard{old};

    pipe::NamedPipeClie c(app::Config().PipeFullName(), OnMessage);
    const uint64_t connectAt = GetTickCount64();
    if (!c.Initialize())
    {
        trace.Event("PIPE_CONNECT_FAILED", "elapsed_ms=%llu", static_cast<unsigned long long>(GetTickCount64() - connectAt));
        return finish(1, "managed controller unavailable");
    }
    const auto connection = pipe::GetConnectionSnapshot();
    trace.Event("PIPE_CONNECTED", "game_pid=%lu session_id=%llu client_session=%llu ready=%d",
                static_cast<unsigned long>(connection.gamePid), static_cast<unsigned long long>(connection.sessionId),
                static_cast<unsigned long long>(c.SessionId()), connection.ready ? 1 : 0);
    if (opt.presetSessionId && opt.presetSessionId != connection.sessionId)
    {
        trace.Event("SESSION_MISMATCH", "preset_session=%llu active_session=%llu",
                    static_cast<unsigned long long>(opt.presetSessionId),
                    static_cast<unsigned long long>(connection.sessionId));
        return finish(2, "preset map belongs to an old game session");
    }
    stopClient = &c;
    struct StopGuard
    {
        pipe::NamedPipeClie &c;
        NavTrace& trace;
        bool& alreadySent;
        ~StopGuard()
        {
            if (!alreadySent)
            {
                NavStopMessage s;
                const bool sent = c.SendPackage(&s);
                trace.Event("FINAL_NAV_STOP", "sent=%d", sent ? 1 : 0);
                alreadySent = true;
            }
        }
    } stop{c, trace, finalStopSent};
    // Disable the old Monitor path state machine before sending bare calls.
    if (!opt.dryRun)
    {
        NavStopMessage s;
        const bool sent = c.SendPackage(&s);
        trace.Event("LEGACY_NAV_STOP", "sent=%d", sent ? 1 : 0);
        if (!sent)
            return finish(1, "stop send failed");
    }

    mapdata::CostBuffer cost;
    if (opt.presetCost)
    {
        cost = *opt.presetCost;
        trace.Event("MAP_SOURCE", "type=preset bytes=%zu", cost.Size());
    }
    else
    {
        pipe::NamedPipeClie data(app::Config().PipeFullName(), OnMessage, managed::Role::Data);
        const uint64_t dataConnectAt = GetTickCount64();
        if (!data.Initialize())
        {
            trace.Event("MAP_DATA_CONNECT_FAILED", "elapsed_ms=%llu", static_cast<unsigned long long>(GetTickCount64() - dataConnectAt));
            return finish(2, "map unavailable");
        }
        trace.Event("MAP_DATA_CONNECTED", "client_session=%llu elapsed_ms=%llu",
                    static_cast<unsigned long long>(data.SessionId()),
                    static_cast<unsigned long long>(GetTickCount64() - dataConnectAt));
        const int fetchRc = Fetch(data, cost, opt.mapTimeoutSec, ext, &trace);
        if (fetchRc)
        {
            trace.Event("MAP_FETCH_FAILED", "rc=%d", fetchRc);
            return finish(2, "map unavailable");
        }
    }

    nav::PixelMap map;
    const uint64_t mapBuildAt = GetTickCount64();
    if (opt.presetPixelMap) {
        map = *opt.presetPixelMap;
        if (map.Empty() || map.Width() != cost.Geo().WorldW() || map.Height() != cost.Geo().WorldH()) {
            trace.Event("MAP_PRESET_INVALID", "preset=%ux%u cost=%ux%u", map.Width(), map.Height(),
                        cost.Geo().WorldW(), cost.Geo().WorldH());
            return finish(2, "preset pixel map dimensions do not match cost buffer");
        }
    }
    else if (!map.BuildFrom(cost))
    {
        trace.Event("MAP_BUILD_FAILED", "empty=%d bytes=%zu", cost.Empty() ? 1 : 0, cost.Size());
        return finish(2, "no white nodes");
    }
    trace.Event("MAP_READY", "world=%ux%u bytes=%zu passable=%zu cost_hash=%016llX build_ms=%llu",
                cost.Geo().WorldW(), cost.Geo().WorldH(), cost.Size(), map.PassableCount(),
                static_cast<unsigned long long>(CostFingerprint(cost)),
                static_cast<unsigned long long>(GetTickCount64() - mapBuildAt));
    if (!opt.follow && !map.Passable(goal.x, goal.y))
    {
        trace.Event("GOAL_NOT_PASSABLE", "goal=(%d,%d) inside=%d", goal.x, goal.y,
                    cost.Geo().Contains(goal.x, goal.y) ? 1 : 0);
        return finish(2, "goal is black/outside");
    }
    trace.Event(opt.follow ? "GOAL_DYNAMIC" : "GOAL_VALID", "goal=(%d,%d)", goal.x, goal.y);
    if (opt.follow) trace.Event("FOLLOW_OPTIONS", "leader=%s distance=%d resume=%d sample_ms=%u replan_ms=%u threshold=%d stale_ms=%u",
        opt.follow->options.leaderName.c_str(), opt.follow->options.distance, opt.follow->options.ResumeDistance(),
        opt.follow->options.sampleMs, opt.follow->options.replanMs, opt.follow->options.moveThreshold, opt.follow->options.StaleMs());

    auto goalReached = [&](nav::PixelPoint p) {
        return !opt.follow && nav::PixelDistance(p,goal)<=opt.arriveTolerance && nav::PixelLineClear(map,p,goal);
    };
    auto cancellation = [&]() {
        NavStopReason reason = stopReason ? stopReason->load() : NavStopReason::None;
        trace.Event("CANCELLED", "external_stop=%d signal_stop=%d source=%u",
                    ext && ext->load() ? 1 : 0, g_stop.load() ? 1 : 0, unsigned(reason));
        const char *why = reason == NavStopReason::User ? "cancelled by user" :
                          reason == NavStopReason::ConnectionChanged ? "cancelled: connection changed" :
                          reason == NavStopReason::WindowClosed ? "cancelled: window closed" : "cancelled";
        return finish(3, why);
    };
    auto abortRead = [&](const char *why) {
        if (Stopped(ext)) return cancellation();
        if (!c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot()))
            return finish(1, "connection/PID/session changed; navigation stopped");
        return finish(4, why);
    };
    // A position request is read-only and may be retried. A timed-out WalkCall is
    // NOT replayed: it may already have executed, even if the acknowledgement was lost.
    auto readPosition = [&](NavStatus &p, const char *phase) {
        for (uint32_t attempt = 0; attempt < opt.positionRetries; ++attempt)
        {
            if (Stopped(ext) || !c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot())) return false;
            if (ReadCurrent(c, p, ext, &trace, phase)) {
                if (stationary) stationary->timer.Observe(GetTickCount64(), {p.x, p.y});
                return true;
            }
            if (stationary) stationary->timer.Reset(); // Unobserved time is not stationary evidence.
            trace.Event("POSITION_RETRY", "phase=%s attempt=%u/%u", phase, attempt+1, opt.positionRetries);
            if (attempt + 1 < opt.positionRetries && !Pause(opt.pollMs, ext)) return false;
        }
        return false;
    };
    NavStatus pos{};
    if (!readPosition(pos, "initial")) return abortRead("fresh coordinate retries exhausted");
    logPosition("initial", pos);
    trace.Event("POLICY", "name=route_feedback_v4 poll_ms=%u lead_ms=%u step_wait_ms=%u startup_ms=%u min_call_ms=%u waypoint_tolerance=%d deviate_distance=%d still_samples=%u stall_limit=%u position_retries=%u recovery_limit=%u",
                opt.pollMs, opt.leadMs, opt.stepWaitMs, opt.startupMs, opt.minCallMs, opt.waypointTolerance,
                opt.deviateDistance, opt.stillSamples, opt.stallLimit, opt.positionRetries, opt.recoveryLimit);
    nav::PixelRoute route;
    nav::TemporaryObstacles obstacles;
    nav::MotionOptions motionOptions;
    motionOptions.leadMs = opt.leadMs;
    motionOptions.startupMs = opt.startupMs;
    motionOptions.waitMaxMs = opt.stepWaitMs;
    motionOptions.minCallMs = opt.minCallMs;
    motionOptions.stillSamples = opt.stillSamples;
    motionOptions.reachRadius = opt.waypointTolerance;
    uint64_t begin = GetTickCount64(), lastProgress = begin, lastIssued = 0;
    double bestRemaining = std::numeric_limits<double>::infinity();
    nav::PixelPoint progressAnchor{pos.x, pos.y};
    uint32_t noPath = 0, stalledSteps = 0, recoveries = 0, blackAttempts = 0;
    double observedSpeed = 0;
    bool singleSent = false;
    auto observeProgress = [&]() {
        nav::PixelPoint p{pos.x, pos.y};
        // A moving leader may keep the distance constant and reset route length.
        // Actual player displacement is progress across all follow replans.
        if (opt.follow && nav::PixelDistance(p, progressAnchor) > 2) {
            progressAnchor = p; lastProgress = GetTickCount64();
        }
        if (!route.Empty())
        {
            nav::PixelBlocked active = [&](nav::PixelPoint q) {
                if ((q.x==p.x && q.y==p.y) || (q.x==goal.x && q.y==goal.y)) return false;
                return obstacles.Contains(q,GetTickCount64());
            };
            route.Advance(map, p, opt.waypointTolerance, active);
            double remaining = route.Remaining(p);
            if (remaining + 2 < bestRemaining)
            {
                if (std::isfinite(bestRemaining)) lastProgress = GetTickCount64();
                bestRemaining = remaining;
                trace.Event("ROUTE_PROGRESS", "index=%zu remaining=%.2f pos=(%d,%d)", route.Index(), remaining, p.x, p.y);
            }
        }
        else if (nav::PixelDistance(p, progressAnchor) > 2)
        {
            lastProgress = GetTickCount64();
            progressAnchor = p;
        }
    };
    // -1: keep this step; -2: a hold ended, discard its old motion/target;
    // non-negative: Navigate has finished and the caller must return that code.
    auto controlGate = [&]() -> int {
        if (!opt.control) return -1;
        bool held = false;
        uint64_t holdStarted = 0;
        std::string lastNote;
        while (true) {
            if (Stopped(ext)) return cancellation();
            if (!c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot()))
                return finish(1, "connection/PID/session changed; navigation control stopped");
            const uint64_t commandStarted = GetTickCount64();
            const auto command = opt.control({pos.x, pos.y});
            // Control may perform I/O, including waiting for a skill-stop reply.
            // An intervening stop/session change takes precedence over its result.
            if (Stopped(ext)) return cancellation();
            if (!c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot()))
                return finish(1, "connection/PID/session changed during navigation control");
            if (command.directive == NavDirective::Fail)
                return finish(1, command.note.empty() ? "navigation control failed" : command.note.c_str());
            if (command.directive == NavDirective::Arrived)
                return finish(0, command.note.empty() ? "navigation control completed" : command.note.c_str());
            const uint64_t now = GetTickCount64();
            if (command.directive != NavDirective::Hold) {
                if (!held) return -1;
                begin += now - holdStarted; // A pause consumes neither timeout nor stall budget.
                lastProgress = now;
                progressAnchor = {pos.x, pos.y};
                t.note.clear();
                trace.Event("CONTROL_RESUME", "pos=(%d,%d) held_ms=%llu", pos.x, pos.y,
                            static_cast<unsigned long long>(now - holdStarted));
                publish();
                return -2;
            }
            if (!held) {
                if (stationary) stationary->timer.Reset();
                NavStopMessage message;
                if (!c.SendPackage(&message)) return finish(1, "navigation hold stop send failed");
                held = true; holdStarted = commandStarted;
                route.Clear(); lastRoute.clear(); obstacles.Clear();
                bestRemaining = std::numeric_limits<double>::infinity();
                stalledSteps = recoveries = noPath = blackAttempts = 0;
                observedSpeed = 0; haveWalkTarget = false;
                trace.Event("CONTROL_HOLD", "pos=(%d,%d) note=%s", pos.x, pos.y, command.note.c_str());
            }
            lastProgress = now;
            if (command.note != lastNote) {
                lastNote = command.note; t.note = command.note; publish();
            }
            if (!Pause(opt.pollMs, ext)) return cancellation();
            if (!readPosition(pos, "control_hold")) return abortRead("fresh coordinate retries exhausted during hold");
            logPosition("control_hold", pos);
        }
    };
    auto watchdog = [&]() {
        if (Stopped(ext)) return cancellation();
        if (!c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot()))
            return finish(1, "connection/PID/session changed; navigation stopped");
        uint64_t now = GetTickCount64();
        if (!opt.follow && now - begin >= uint64_t(opt.walkTimeoutSec) * 1000) return finish(4, "navigation timeout");
        if (now - lastProgress >= uint64_t(opt.stallSec) * 1000)
        {
            trace.Event("STALL_TIMEOUT", "pos=(%d,%d) recoveries=%u stalled_steps=%u", pos.x, pos.y, recoveries, stalledSteps);
            return finish(4, "no route progress after bounded recovery");
        }
        return -1;
    };
    auto recoverStationary = [&](nav::PixelPoint toward) -> int {
        if (!stationary || !stationary->timer.Due() ||
            nav::PixelDistance({pos.x, pos.y}, toward) < 1) return -1;
        if (Stopped(ext)) return cancellation();
        if (!c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot()))
            return finish(1, "connection/PID/session changed before stationary recovery");
        stationary->timer.Reset(); // Consume before sending; no replay after an ambiguous reply.
        const nav::PixelPoint frozen{pos.x, pos.y};
        NavStopMessage message;
        if (!c.SendPackage(&message)) return finish(1, "stationary recovery navigation stop failed");
        // The position reply on this same connection is a barrier after NavStop.
        // It also catches movement between detecting the stall and stopping.
        if (!readPosition(pos, "stationary_recovery_stop")) return abortRead("coordinate read failed before stationary recovery");
        logPosition("stationary_recovery_stop", pos);
        trace.Event("STATIONARY_RECOVERY", "still_ms=5000 pos=(%d,%d) toward=(%d,%d)", pos.x, pos.y, toward.x, toward.y);
        const auto recovered = pos.x == frozen.x && pos.y == frozen.y
            ? stationary->perform(c, frozen, toward)
            : NavControl{NavDirective::Continue, "coordinates changed before recovery; roll skipped"};
        if (Stopped(ext)) return cancellation();
        if (!c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot()))
            return finish(1, "connection/PID/session changed during stationary recovery");
        if (recovered.directive == NavDirective::Fail)
            return finish(1, recovered.note.empty() ? "stationary recovery failed" : recovered.note.c_str());
        route.Clear(); lastRoute.clear(); obstacles.Clear(); haveWalkTarget = false;
        bestRemaining = std::numeric_limits<double>::infinity(); observedSpeed = 0;
        stalledSteps = recoveries = noPath = blackAttempts = 0;
        if (!readPosition(pos, "stationary_recovery")) return abortRead("coordinate read failed after stationary recovery");
        logPosition("stationary_recovery", pos);
        progressAnchor = {pos.x, pos.y}; lastProgress = lastIssued = GetTickCount64();
        stationary->timer.Reset();
        trace.Event("STATIONARY_RECOVERY_END", "pos=(%d,%d) directive=%u note=%s", pos.x, pos.y,
                    unsigned(recovered.directive), recovered.note.c_str());
        return -2; // Replan from the new position, never reuse this motion window.
    };
    auto failedPlan = [&](const char *why) {
        trace.Event("PLAN_FAILED", "attempt=%u retry=%u/%u error=%s", planAttempts, noPath+1, opt.maxNoPath, why);
        ++noPath;
        route.Clear();
        bestRemaining = std::numeric_limits<double>::infinity();
    };
    uint64_t followMapToken = 0, followSequence = 0, lastFollowPlanAt = 0;
    bool followHasGoal = false, followWaiting = false, followSeen = false;
    bool lootMode = false;
    bool portalMode = false, travelHolding = false;
    uint32_t lootGoalKey = 0;
    uint64_t lootGoalEntity = 0;
    std::string followError;
    auto followState = [&](follow::State state, const std::string& note = std::string()) {
        if (state != followFeedback.state || note != followFeedback.note)
            trace.Event("FOLLOW_STATE", "state=%u note=%s", unsigned(state), note.c_str());
        followFeedback.state = state; followFeedback.note = note; publish();
    };
    auto travelGate = [&](const follow::TravelSnapshot& travel) {
        if (travel.action == follow::TravelAction::ReloadScene) { followError = travel.note; return -2; }
        if (travel.action == follow::TravelAction::Failed) { followError = travel.note; return -1; }
        if (travel.action != follow::TravelAction::Wait) return 0;
        route.Clear(); lastRoute.clear(); followHasGoal = false; followWaiting = false; portalMode = false;
        lastProgress = GetTickCount64(); bestRemaining = std::numeric_limits<double>::infinity();
        if (!travelHolding) { NavStopMessage stop; c.SendPackage(&stop); travelHolding = true; }
        followState(follow::State::Travelling, travel.note); return 1;
    };
    // -2: discard this scene and rebuild navigation; -1: terminal.
    auto refreshFollow = [&]() -> int {
        if (!opt.follow) return 0;
        auto now = GetTickCount64();
        auto sample = opt.follow->sample();
        const auto& options = opt.follow->options;
        if (Stopped(ext)) return 1;
        if (sample.sequence != followSequence) {
            followSequence = sample.sequence;
            trace.Event("FOLLOW_SAMPLE", "seq=%llu valid=%d age_ms=%llu leader=(%d,%d) entity=0x%llX map=0x%llX note=%s",
                sample.sequence, sample.valid ? 1 : 0,
                sample.sampledAt && now >= sample.sampledAt ? now - sample.sampledAt : 0,
                sample.position.x, sample.position.y, sample.entity, sample.mapToken, sample.note.c_str());
        }
        if (sample.terminal) { followError = sample.note; return -1; }
        if (sample.sceneChanged) { followError = sample.note; return -2; }
        if (opt.follow->travel) {
            const int gate = travelGate(opt.follow->travel(sample, false));
            if (gate) return gate;
            // Metadata reads may take time; use the newest leader sample afterward.
            sample = opt.follow->sample(); now = GetTickCount64();
            if (sample.terminal) { followError = sample.note; return -1; }
            if (sample.sceneChanged) { followError = sample.note; return -2; }
        }
        if (sample.mapToken) {
            if ((followMapToken && followMapToken != sample.mapToken) || sample.width != map.Width() || sample.height != map.Height()) {
                followError = "地图身份或尺寸已变化，请重新启动跟随"; return -1;
            }
            followMapToken = sample.mapToken;
        }
        const nav::PixelPoint player{pos.x, pos.y};
        followFeedback.leader = sample.position;
        followFeedback.distance = follow::Fresh(sample, now, options.StaleMs()) ? nav::PixelDistance(player, sample.position) : -1;
        if (opt.follow->loot) {
            const auto item = opt.follow->loot(player);
            followFeedback.lootName = item.name;
            followFeedback.lootRemaining = item.remaining; followFeedback.lootResolved = item.resolved;
            followFeedback.lootSkipped = item.skipped;
            if (item.terminal) { followError = item.note; return -1; }
            if (item.active || item.waiting) {
                const bool changed = !lootMode || item.key != lootGoalKey || item.entity != lootGoalEntity;
                if (changed) {
                    route.Clear(); lastRoute.clear(); obstacles.Clear(); followHasGoal = false; followWaiting = false;
                    bestRemaining = std::numeric_limits<double>::infinity(); lastProgress = now;
                    stalledSteps = recoveries = noPath = blackAttempts = 0;
                    trace.Event("LOOT_PRIORITY", "key=0x%X entity=0x%llX name=%s remaining=%u", item.key, item.entity, item.name.c_str(), item.remaining);
                }
                lootMode = true; lootGoalKey = item.key; lootGoalEntity = item.entity;
                if (item.waiting) {
                    route.Clear(); lastRoute.clear(); followHasGoal = false; lastProgress = now;
                    followState(follow::State::LootWaiting, item.note); return 1;
                }
                // Drops can lie on a black pixel. Approach a white pixel at most 8 pixels away.
                nav::PixelPoint approach = item.position;
                if (!map.Passable(approach.x, approach.y)) {
                    double best = std::numeric_limits<double>::infinity();
                    for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
                        nav::PixelPoint point{item.position.x + dx, item.position.y + dy};
                        const double distance = nav::PixelDistance(point, item.position);
                        if (distance <= 8 && map.Passable(point.x, point.y) && distance < best) { best = distance; approach = point; }
                    }
                    if (!std::isfinite(best)) { followError = "物品附近没有可走区域：" + item.name; return -1; }
                }
                if (nav::PixelDistance(player, item.position) <= 12 && nav::PixelDistance(player, approach) <= 12 &&
                    nav::PixelLineClear(map, player, approach)) {
                    if (!opt.follow->pick || !opt.follow->pick(item.key, item.entity, followError)) return -1;
                    route.Clear(); lastRoute.clear(); followHasGoal = false; lastProgress = GetTickCount64();
                    followState(follow::State::LootWaiting, "等待地面物品消失：" + item.name); return 1;
                }
                if (!followHasGoal || goal.x != approach.x || goal.y != approach.y) {
                    goal = approach; t.goalX = goal.x; t.goalY = goal.y; followHasGoal = true;
                    route.Clear(); lastRoute.clear(); bestRemaining = std::numeric_limits<double>::infinity();
                    followState(follow::State::LootApproach, "正在接近物品：" + item.name); return 2;
                }
                followState(follow::State::LootApproach, "正在接近物品：" + item.name); return 0;
            }
        }
        if (lootMode) {
            trace.Event("LOOT_RESUME_FOLLOW", "resolved=%u", followFeedback.lootResolved);
            route.Clear(); lastRoute.clear(); obstacles.Clear(); followHasGoal = false; followWaiting = false;
            lootMode = false; lootGoalKey = 0; lootGoalEntity = 0; lastFollowPlanAt = 0;
            bestRemaining = std::numeric_limits<double>::infinity(); lastProgress = GetTickCount64();
            stalledSteps = recoveries = noPath = blackAttempts = 0;
        }
        if (!follow::Fresh(sample, now, options.StaleMs())) {
            if (opt.follow->travel) {
                // Finish any active pickup first, then cancel the previous walk
                // before potentially slow party/map reads or transfer requests.
                if (!travelHolding && !portalMode) { NavStopMessage stop; c.SendPackage(&stop); travelHolding = true; }
                const auto travel = opt.follow->travel(sample, true);
                const int gate = travelGate(travel);
                if (gate) return gate;
                if (travel.action == follow::TravelAction::Portal) {
                    nav::PixelPoint approach = travel.position;
                    if (!map.Passable(approach.x, approach.y)) {
                        double best = std::numeric_limits<double>::infinity();
                        for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
                            nav::PixelPoint point{travel.position.x + dx, travel.position.y + dy};
                            const auto distance = nav::PixelDistance(point, travel.position);
                            if (distance <= 8 && map.Passable(point.x, point.y) && distance < best) { best = distance; approach = point; }
                        }
                        if (!std::isfinite(best)) {
                            follow::TravelSnapshot wait; wait.action = follow::TravelAction::Wait;
                            wait.note = "传送门附近暂时没有可走区域，等待下一次扫描";
                            return travelGate(wait);
                        }
                    }
                    followFeedback.leader = travel.position;
                    followFeedback.distance = nav::PixelDistance(player, travel.position);
                    if (followFeedback.distance <= 12 && nav::PixelDistance(player, approach) <= 12 && nav::PixelLineClear(map, player, approach)) {
                        NavStopMessage stop; c.SendPackage(&stop); travelHolding = true;
                        if (!opt.follow->enterPortal) { followError = "缺少进门接口"; return -1; }
                        return travelGate(opt.follow->enterPortal(travel.key, travel.entity));
                    }
                    const bool changed = !portalMode || !followHasGoal || goal.x != approach.x || goal.y != approach.y;
                    portalMode = true; travelHolding = false; followWaiting = false;
                    if (changed) {
                        route.Clear(); lastRoute.clear(); obstacles.Clear();
                        goal = approach; t.goalX = goal.x; t.goalY = goal.y; followHasGoal = true;
                        bestRemaining = std::numeric_limits<double>::infinity(); lastProgress = GetTickCount64();
                        stalledSteps = recoveries = noPath = blackAttempts = 0;
                    }
                    followState(follow::State::Travelling, travel.note); return changed ? 2 : 0;
                }
            }
            route.Clear(); lastRoute.clear(); followHasGoal = false; followWaiting = false;
            lastProgress = now; bestRemaining = std::numeric_limits<double>::infinity();
            followFeedback.distance = -1;
            followState(followSeen ? follow::State::Lost : follow::State::Searching,
                        sample.note.empty() ? "等待新鲜的队长坐标" : sample.note);
            return 1;
        }
        if (portalMode) { route.Clear(); lastRoute.clear(); obstacles.Clear(); followHasGoal = false; portalMode = false; }
        travelHolding = false;
        if ((followMapToken && followMapToken != sample.mapToken) || sample.width != map.Width() || sample.height != map.Height()) {
            followError = "地图身份或尺寸已变化，请重新启动跟随"; return -1;
        }
        followMapToken = sample.mapToken; followSeen = true;
        if (!map.Passable(sample.position.x, sample.position.y)) {
            route.Clear(); lastRoute.clear(); followHasGoal = false; lastProgress = now;
            followState(follow::State::Lost, "队长坐标不在当前地图可走区域，等待有效位置"); return 1;
        }
        double reachable = std::numeric_limits<double>::infinity();
        if (nav::PixelLineClear(map, player, sample.position)) reachable = followFeedback.distance;
        else if (followHasGoal && !route.Empty() && nav::PixelLineClear(map, goal, sample.position))
            reachable = route.Remaining(player) + nav::PixelDistance(goal, sample.position);
        if (follow::HoldDistance(reachable, followWaiting, options, opt.waypointTolerance)) {
            followWaiting = true; lastProgress = now; stalledSteps = 0; recoveries = 0;
            followState(follow::State::Waiting); return 1;
        }
        followWaiting = false;
        const bool exhaustedTail = !route.Empty() && route.Remaining(player) <= options.distance + opt.waypointTolerance &&
            nav::PixelDistance(goal, sample.position) >= 1 && (!lastFollowPlanAt || now - lastFollowPlanAt >= options.replanMs);
        if (!followHasGoal || exhaustedTail || follow::RetargetDue(goal, sample.position, now, lastFollowPlanAt, options) ||
            (route.Empty() && (goal.x != sample.position.x || goal.y != sample.position.y))) {
            if (lastFollowPlanAt && now - lastFollowPlanAt < options.replanMs && route.Empty()) {
                followState(follow::State::Planning, "等待重规划间隔"); return 1;
            }
            goal = sample.position; followHasGoal = true;
            t.goalX = goal.x; t.goalY = goal.y;
            route.Clear(); lastRoute.clear(); bestRemaining = std::numeric_limits<double>::infinity();
            trace.Event("FOLLOW_RETARGET", "goal=(%d,%d) distance=%.2f min_replan_ms=%u", goal.x, goal.y,
                        followFeedback.distance, options.replanMs);
            followState(follow::State::Planning); return 2;
        }
        followState(follow::State::Following); return 0;
    };
    auto followStep = [&]() {
        if (!opt.follow || route.Empty() || lootMode || portalMode) return double(opt.stepDistance);
        return (std::min)(double(opt.stepDistance),
            (std::max)(1.0, route.Remaining({pos.x, pos.y}) - opt.follow->options.distance));
    };
    while (true)
    {
        const int controlResult = controlGate();
        if (controlResult >= 0) return controlResult;
        if (controlResult == -2) continue;
        const int followGate = refreshFollow();
        if (followGate < 0) return finish(followGate == -2 ? kFollowSceneReload : 1, followError.c_str());
        if (followGate == 1) {
            if (Stopped(ext)) return cancellation();
            if (!c.IsConnected() || !connection.Same(pipe::GetConnectionSnapshot()))
                return finish(1, "connection/PID/session changed; follow stopped");
            if (!Pause(opt.pollMs, ext)) return cancellation();
            if (!readPosition(pos, "follow_wait")) return abortRead("fresh coordinate retries exhausted");
            logPosition("follow_wait", pos); observeProgress(); continue;
        }
        int rc = watchdog();
        if (rc >= 0) return rc;
        nav::PixelPoint cur{pos.x, pos.y};
        if (goalReached(cur))
            return finish(0, "within world-coordinate arrival radius");
        if (haveWalkTarget) {
            rc = recoverStationary(lastWalkTarget);
            if (rc >= 0) return rc;
            if (rc == -2) continue;
        }
        if (opt.singleShot && singleSent)
        {
            if (!Pause(opt.pollMs, ext)) return cancellation();
            if (!readPosition(pos, "single_shot")) return abortRead("fresh coordinate retries exhausted");
            logPosition("single_shot", pos);
            // Diagnostic single-shot mode never reissues or replaces its only call.
            if (nav::PixelDistance({pos.x,pos.y}, progressAnchor) > 2)
            { progressAnchor={pos.x,pos.y}; lastProgress=GetTickCount64(); }
            continue;
        }
        obstacles.Expire(GetTickCount64());
        // The player and exact goal are never blackened by local recovery feedback.
        nav::PixelBlocked blocked = [&, cur](nav::PixelPoint p) {
            if ((p.x==cur.x && p.y==cur.y) || (p.x==goal.x && p.y==goal.y)) return false;
            return obstacles.Contains(p, GetTickCount64());
        };
        nav::PixelPoint target{};
        bool recoveringBlack = !map.Passable(cur.x, cur.y);
        if (recoveringBlack)
        {
            route.Clear();
            double nearest = std::numeric_limits<double>::infinity();
            for (int dy=-8; dy<=8; ++dy) for (int dx=-8; dx<=8; ++dx)
            {
                nav::PixelPoint p{cur.x+dx,cur.y+dy};
                double d = std::hypot(double(dx),double(dy));
                if (d > 0 && d <= 8 && d <= opt.stepDistance && d < nearest && map.Passable(p.x,p.y))
                { nearest=d; target=p; }
            }
            if (!std::isfinite(nearest)) return finish(4, "black start has no nearby white recovery point");
            trace.Event("RECOVERY_TARGET", "kind=black_start attempt=%u pos=(%d,%d) target=(%d,%d)", blackAttempts+1,cur.x,cur.y,target.x,target.y);
        }
        else
        {
            blackAttempts = 0;
            if (!route.Empty() && route.Deviation(cur) > opt.deviateDistance)
            {
                trace.Event("ROUTE_DEVIATION", "distance=%.2f threshold=%d index=%zu",route.Deviation(cur),opt.deviateDistance,route.Index());
                route.Clear();
            }
            if (route.Empty())
            {
                nav::PixelPath path;
                uint64_t planAt = GetTickCount64();
                if (opt.follow) lastFollowPlanAt = planAt;
                ++planAttempts;
                auto cancelled = [&]() { return Stopped(ext) || !connection.Same(pipe::GetConnectionSnapshot()); };
                if (!nav::FindPixelPath(map,cur,goal,path,cancelled,blocked) ||
                    !route.Build(map,path.points,blocked,cancelled) || route.Points().size() > opt.maxWaypoints)
                {
                    failedPlan(path.error.empty() ? "visibility route unavailable" : path.error.c_str());
                    if (opt.dryRun || noPath >= opt.maxNoPath) return abortRead("no usable route after planning retries");
                    if (!obstacles.Empty())
                    {
                        trace.Event("OBSTACLE_RELAX", "reason=no_route; original cost buffer unchanged");
                        obstacles.Clear();
                    }
                    if (!Pause(opt.replanMs,ext)) return cancellation();
                    if (!readPosition(pos,"plan_retry")) return abortRead("fresh coordinate retries exhausted");
                    logPosition("plan_retry",pos);
                    continue;
                }
                ++t.replans;
                if (opt.follow) followFeedback.planMs = GetTickCount64() - planAt;
                t.pathPoints = uint32_t(route.Points().size());
                bestRemaining = route.Remaining(cur);
                lastRoute.clear();
                for (auto p:route.Points()) lastRoute.push_back({p.x,p.y});
                trace.Event("PLAN_READY", "plan=%u start=(%d,%d) goal=(%d,%d) expanded=%zu raw_points=%zu waypoint_count=%zu path_length=%.3f elapsed_ms=%llu",
                    t.replans,cur.x,cur.y,goal.x,goal.y,path.expanded,path.points.size(),route.Points().size(),path.length,
                    static_cast<unsigned long long>(GetTickCount64()-planAt));
                for (size_t i=0;i<route.Points().size() && i<20000;++i)
                {
                    auto p=route.Points()[i];
                    trace.EventBuffered("WAYPOINT","plan=%u index=%zu x=%d y=%d passable=%d",t.replans,i,p.x,p.y,map.Passable(p.x,p.y));
                }
                trace.Flush();
                LOGI("[PixelNav][run=%04X] route plan=%u raw=%zu corners=%zu length=%.1f",trace.RunId(),t.replans,path.points.size(),route.Points().size(),path.length);
                if (opt.dryRun)
                {
                    SavePath(cost,lastRoute,opt.mapBmpPath);
                    return finish(0,"dry-run visibility route valid (no movement issued)");
                }
                // A* can take time while a previous movement is still in progress.
                if (!readPosition(pos,"after_plan")) return abortRead("fresh coordinate retries exhausted");
                logPosition("after_plan",pos);
                cur={pos.x,pos.y};
                const int controlResult = controlGate();
                if (controlResult >= 0) return controlResult;
                if (controlResult == -2) continue;
                const int gate = refreshFollow();
                if (gate < 0) return finish(gate == -2 ? kFollowSceneReload : 1, followError.c_str());
                if (gate) continue;
                if (goalReached(cur)) return finish(0,"within world-coordinate arrival radius");
                if (!map.Passable(cur.x,cur.y) || route.Deviation(cur)>opt.deviateDistance) {route.Clear();continue;}
            }
            observeProgress();
            if (!route.Target(map,cur,followStep(),opt.waypointTolerance,target,blocked,
                              opt.waypointTolerance + observedSpeed*opt.leadMs/1000.0))
            {
                failedPlan("no visible integer target within call radius");
                if (noPath>=opt.maxNoPath) return finish(4,"no usable movement target after planning retries");
                if (!Pause(opt.replanMs,ext)) return cancellation();
                if (!readPosition(pos,"target_retry")) return abortRead("fresh coordinate retries exhausted");
                logPosition("target_retry",pos);
                continue;
            }
        }
        if (opt.dryRun) return finish(4,"dry-run start is black/outside");
        const uint64_t sinceLastCall = GetTickCount64()-lastIssued;
        if (lastIssued && sinceLastCall<opt.minCallMs)
        {
            // Do not wait with a stale coordinate/target. Sample again at the next
            // observation tick; only command emission is rate limited.
            if (!Pause((std::min)(opt.pollMs,uint32_t(opt.minCallMs-sinceLastCall)),ext)) return cancellation();
            if (!readPosition(pos,"cadence")) return abortRead("fresh coordinate retries exhausted");
            logPosition("cadence",pos);
            observeProgress();
            continue;
        }
        const int controlBeforeSend = controlGate();
        if (controlBeforeSend >= 0) return controlBeforeSend;
        if (controlBeforeSend == -2) continue;
        rc=watchdog(); if(rc>=0)return rc;
        const int sendGate = refreshFollow();
        if (sendGate < 0) return finish(sendGate == -2 ? kFollowSceneReload : 1, followError.c_str());
        if (sendGate) continue;
        rc = recoverStationary(target);
        if (rc >= 0) return rc;
        if (rc == -2) continue;
        // Observation ticks waiting for minCallMs do not consume a move attempt.
        if (recoveringBlack && (++blackAttempts > opt.stallLimit || ++recoveries > opt.recoveryLimit))
            return finish(4, "black-start recovery budget exhausted");
        uint64_t issuedAt=0;
        if (opt.singleShot)
        {
            trace.Event("SINGLE_SHOT_TARGET_OVERRIDE","planned_target=(%d,%d) goal=(%d,%d)",target.x,target.y,goal.x,goal.y);
            target=goal;
        }
        uint16_t callId=uint16_t((callCount%65535u)+1u);
        ++callCount;
        trace.Event("ROUTE_TARGET","index=%zu pos=(%d,%d) target=(%d,%d) distance=%.2f chord_clear=%d recovery=%d",
                    route.Index(),cur.x,cur.y,target.x,target.y,nav::PixelDistance(cur,target),
                    nav::PixelLineClear(map,cur,target,blocked)?1:0,recoveringBlack?1:0);
        if (!SendWalk(c,target,ext,issuedAt,&trace,callId))
            return abortRead("walk call rejected or acknowledgement missing; not replayed");
        noPath=0;
        lastIssued=issuedAt;
        lastWalkTarget=target;
        haveWalkTarget=true;
        if (stationary && !stationary->timer.Active()) stationary->timer.Begin(issuedAt, cur);
        singleSent=true;
        ++t.seq; ++t.sentBatches; t.state=NAV_MOVING; publish();
        const size_t issuedRouteIndex = route.Index();
        nav::MotionWindow motion;
        motion.Begin(issuedAt,cur,target);
        while (true)
        {
            if (!Pause(opt.pollMs,ext)) return cancellation();
            if (!readPosition(pos,"motion")) return abortRead("fresh coordinate retries exhausted");
            logPosition("motion",pos);
            cur={pos.x,pos.y};
            observeProgress();
            const int controlDuringMotion = controlGate();
            if (controlDuringMotion >= 0) return controlDuringMotion;
            if (controlDuringMotion == -2) break;
            const int gate = refreshFollow();
            if (gate < 0) return finish(gate == -2 ? kFollowSceneReload : 1, followError.c_str());
            if (gate) break;
            rc=watchdog(); if(rc>=0)return rc;
            if (goalReached(cur))
                return finish(0,"within world-coordinate arrival radius");
            rc = recoverStationary(target);
            if (rc >= 0) return rc;
            if (rc == -2) break;
            auto decision=motion.Sample(GetTickCount64(),cur,motionOptions);
            if (motion.Speed()>0) observedSpeed=motion.Speed();
            trace.Event("MOTION_SAMPLE","call=%u index=%zu moved=%.2f speed=%.2f remain=%.2f decision=%u",
                        callId,route.Index(),motion.Moved(),motion.Speed(),nav::PixelDistance(cur,target),unsigned(decision));
            if (opt.singleShot) break;
            if (!map.Passable(cur.x,cur.y) && !recoveringBlack) break;
            if (!route.Empty() && route.Deviation(cur)>opt.deviateDistance) break;
            if (decision==nav::MotionResult::Still || decision==nav::MotionResult::Timeout)
            {
                stalledSteps = motion.Moved()<2 ? stalledSteps+1 : 0;
                trace.Event("STEP_WAIT_END","call=%u reason=%s moved=%.2f stalled=%u/%u",
                            callId,decision==nav::MotionResult::Still?"still":"timeout",motion.Moved(),stalledSteps,opt.stallLimit);
                if (stalledSteps>=opt.stallLimit)
                {
                    if (++recoveries>opt.recoveryLimit) return finish(4,"local recovery budget exhausted");
                    double d=nav::PixelDistance(cur,target);
                    if(d>2)
                    {
                        double k=(std::min)(2.0,d)/d;
                        nav::PixelPoint obstruction{int32_t(std::lround(cur.x+(target.x-cur.x)*k)),
                                                    int32_t(std::lround(cur.y+(target.y-cur.y)*k))};
                        obstacles.Add(obstruction,1,GetTickCount64()+15000);
                        trace.Event("TEMP_OBSTACLE","center=(%d,%d) radius=1 ttl_ms=15000 recovery=%u; current pixel and goal exempt",obstruction.x,obstruction.y,recoveries);
                    }
                    route.Clear();
                    stalledSteps=0;
                    bestRemaining=std::numeric_limits<double>::infinity();
                }
                break;
            }
            const bool routeAdvanced = !recoveringBlack && !route.Empty() && route.Index()>issuedRouteIndex;
            if (routeAdvanced || decision==nav::MotionResult::Reached || decision==nav::MotionResult::Lead)
            {
                if (motion.Moved()>=2) stalledSteps=0;
                if (recoveringBlack) break;
                nav::PixelPoint next{};
                const bool haveNext = route.Target(map,cur,followStep(),opt.waypointTolerance,next,blocked,
                                                    opt.waypointTolerance + observedSpeed*opt.leadMs/1000.0);
                if (haveNext && nav::PixelDistance(next,target)>=1)
                {
                    trace.Event("STEP_HANDOFF","call=%u reason=%s next=(%d,%d)",callId,
                                routeAdvanced?"route_advanced":decision==nav::MotionResult::Lead?"lead":"reached",next.x,next.y);
                    break;
                }
                if (decision == nav::MotionResult::Reached)
                {
                    // A reached target must never leave us in this inner loop
                    // forever. If route progress could not produce a different
                    // target (for example after WalkCall cut a corner), clear
                    // the cached route and let the outer loop replan from the
                    // fresh position instead of repeating a 1-pixel call.
                    trace.Event("TARGET_HANDOFF_STALL", "call=%u pos=(%d,%d) target=(%d,%d) route_index=%zu next=%s(%d,%d)",
                                callId,cur.x,cur.y,target.x,target.y,route.Index(),
                                haveNext?"set":"none",next.x,next.y);
                    route.Clear();
                    break;
                }
                // A required narrow corner is still ahead: observe it rather than
                // repeating the same target or skipping it through a wall.
            }
        }
    }
}

int LoadNavOptionsFromIni(NavigateOptions &opt, const char *filename)
{
    char mod[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, mod, MAX_PATH))
        return 0;
    std::string path(mod);
    path = app::Config().RuntimeFile(filename ? filename : "config.ini");
    int n = 0;
    auto read = [&](const char *key, int low, int high, int &value) {
        char b[64]{};
        GetPrivateProfileStringA("nav", key, "", b, sizeof(b), path.c_str());
        if (!*b)
            return;
        char *end = nullptr;
        long v = strtol(b, &end, 10);
        if (end == b || v < low || v > high)
            return;
        value = int(v);
        ++n;
    };
    int v = opt.stepDistance;
    read("stepDistance", 1, 1000, v);
    opt.stepDistance = v;
    v = int(opt.replanMs);
    read("replanMs", 150, 30000, v);
    opt.replanMs = uint32_t(v);
    v = opt.arriveTolerance;
    read("arriveTolerance", 1, 1000, v);
    opt.arriveTolerance = int16_t(v);
    auto u32 = [&](const char *key, uint32_t &field, int low, int high) {
        int value = int(field); read(key, low, high, value); field = uint32_t(value);
    };
    u32("pollMs",opt.pollMs,20,1000);
    u32("leadMs",opt.leadMs,0,1000);
    u32("stepWaitMs",opt.stepWaitMs,300,30000);
    u32("startupMs",opt.startupMs,100,5000);
    u32("minCallMs",opt.minCallMs,100,3000);
    u32("stillSamples",opt.stillSamples,2,20);
    u32("stallLimit",opt.stallLimit,2,20);
    u32("positionRetries",opt.positionRetries,1,5);
    u32("recoveryLimit",opt.recoveryLimit,1,100);
    v=opt.waypointTolerance; read("waypointTolerance",1,30,v); opt.waypointTolerance=v;
    v=opt.deviateDistance; read("deviateDistance",1,1000,v); opt.deviateDistance=v;
    return n;
}
} // namespace usecase
