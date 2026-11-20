#pragma once
#include "Navigator.h"
#include "Surroundings.h"
#include "WorldArea.h"
#include "../Common/ComboSequence.h"
#include "../Common/RitualCombat.h"
#include "../Common/EncounterRoute.h"
#include "../Common/PortalReturn.h"
#include <atomic>
#include <mutex>

namespace ritual {
enum class PointState { Pending, Approaching, Confirming, Clearing, Interacting, Activating, Chasing, Fighting, Completed, Attention, Unreachable };
const char* StateText(PointState state);
struct Options {
    bool planOnly = false;
    poe::WorldAreaSnapshot expectedArea;
    int32_t interactDistance = 18;
    int32_t associationDistance = 24;
    int32_t arenaRadius = 80;
    int32_t preclearRadius = 120; // Provisional; the game's blocking radius is unconfirmed.
    uint32_t scanMs = 400, loadTimeoutSec = 20, battleTimeoutSec = 240;
    bool fightElites = false;
};
using PointKind = encounterroute::Kind;
struct Point {
    PointKind kind = PointKind::Ritual;
    poe::EntitySnapshot object;
    ritualstate::Snapshot ritual;
    nav::PixelPoint approach{};
    PointState state = PointState::Pending;
    uint32_t order = 0;
    double pathDistance = -1;
    bool interactable = false, light = false;
    bool activationSent = false;
    bool activationConfirmed = false, blocker = false;
    uint32_t activationAttempts = 0;
    nav::PixelPoint activationPosition{};
    std::string detail;
};
struct Status {
    bool running = false, paused = false, planOnly = false;
    uint64_t revision = 0;
    poe::WorldAreaSnapshot area;
    std::vector<Point> points;
    std::vector<nav::PixelPoint> route;
    int active = -1;
    bool showCombatCircle = false;
    ritualcombat::Circle combatCircle;
    portalreturn::Ledger returnHome;
    std::string text = "尚未规划祭祀路线";
    std::wstring diagnosticPath;
};
// Run is synchronous and belongs to the GUI's single navigation worker.
class Automation {
public:
    bool Prepare(const Options&, std::string& error);
    int Run(const Options&, const std::vector<combo::Row>&, std::atomic_bool& stop,
            const std::atomic<usecase::NavStopReason>* reason = nullptr);
    void StartFailed(const std::string& error);
    void SetPaused(bool paused);
    bool MarkCompleted(int32_t x, int32_t y, std::string& error);
    bool ResetInteraction(int32_t x, int32_t y, std::string& error);
    Status GetStatus() const;
    bool GetCombatCircle(const poe::WorldAreaSnapshot&, ritualcombat::Circle&) const;
private:
    mutable std::mutex mutex_;
    std::atomic_bool pause_{false};
    Status status_;
};
Automation& MainAutomation();
} // namespace ritual
