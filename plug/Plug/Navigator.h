#pragma once
#include "../Common/RouteNavigation.h"
#include "../Common/FollowNavigation.h"
#include "../Common/StationaryRecovery.h"
#include "../Common/Protocal.h"
#include "MapSink.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace pipe { class NamedPipeClie; }
namespace usecase
{
constexpr int kFollowSceneReload = 6;
enum class NavDirective { Continue, Hold, Arrived, Fail };
struct NavControl {
    NavDirective directive = NavDirective::Continue;
    std::string note;
};
struct StationaryRecovery {
    nav::StationaryRecovery timer;
    // Runs after navigation is stopped, on its existing Controller connection.
    std::function<NavControl(pipe::NamedPipeClie&, nav::PixelPoint, nav::PixelPoint)> perform;
};
struct NavigateOptions
{
    int32_t goalX = -1, goalY = -1;
    bool dryRun = false;
    uint32_t replanMs = 800; // Backoff for failed planning/recovery, not a per-call blind wait.
    uint32_t pollMs = 80, leadMs = 120, stepWaitMs = 3000, startupMs = 350, minCallMs = 200;
    uint32_t stillSamples = 3, stallLimit = 3, positionRetries = 3, recoveryLimit = 8;
    int32_t waypointTolerance = 3, deviateDistance = 12;
    uint32_t mapTimeoutSec = 30, walkTimeoutSec = 600, maxNoPath = 20, stallSec = 30;
    int32_t stepDistance = 126;   // T1 world-coordinate distance; no CELL_SIZE conversion.
    int16_t arriveTolerance = 13; // T4 user measurement, 2026-11-10.
    size_t maxWaypoints = 100000;
    std::string mapBmpPath;
    const mapdata::CostBuffer *presetCost = nullptr;
    const nav::PixelMap *presetPixelMap = nullptr; // Shared immutable white-node map for multi-leg routes.
    uint64_t presetSessionId = 0;
    bool singleShot = false;
    const follow::Channel* follow = nullptr; // Optional moving target; lifetime spans Navigate().
    // Runs serially with movement, with a fresh position. Hold stops movement and
    // samples until released; resuming always replans from the latest position.
    std::function<NavControl(nav::PixelPoint)> control;
    StationaryRecovery* stationaryRecovery = nullptr; // Opt-in; owned by the ritual run.
};
int LoadNavOptionsFromIni(NavigateOptions &, const char *fileName = "config.ini");
struct NavTelemetry
{
    int32_t playerX = 0, playerY = 0, goalX = 0, goalY = 0, distToGoal = 0;
    uint32_t seq = 0, pathPoints = 0, replans = 0, sentBatches = 0, state = 0;
    bool arrived = false, stuck = false, running = false;
    std::string note;
};
enum class NavStopReason : uint32_t { None, User, ConnectionChanged, WindowClosed };
int Navigate(const NavigateOptions &, NavTelemetry *telemetry = nullptr, std::atomic_bool *stopFlag = nullptr,
             const std::atomic<NavStopReason> *stopReason = nullptr);
int FetchCostBuffer(mapdata::CostBuffer &, const char *note = nullptr, std::atomic_bool *stopFlag = nullptr);
bool StartPlayerTracker(const std::string &pipeName);
void StopPlayerTracker();
bool GetLastPlayerPos(int32_t &, int32_t &);
bool PlanPath(const mapdata::CostBuffer &, nav::PixelMap *preset, int32_t curX, int32_t curY, int32_t goalX,
              int32_t goalY, const NavigateOptions &, std::vector<NavBatchChunk> &, int32_t &outGoalX,
              int32_t &outGoalY, std::string &error);
} // namespace usecase
