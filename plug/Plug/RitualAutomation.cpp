#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RitualAutomation.h"
#include "../Common/EncounterRoute.h"
#include "UseCases.h"
#include "ComboRunner.h"
#include "MyOLAFunction.h"
#include "NamedPipeClie.h"
#include "../Common/Logger.h"
#include "../Common/RunDiagnostics.h"
#include "../Common/PlayerVitals.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

namespace ritual {
namespace {
constexpr const char* kObject = "Metadata/Terrain/Leagues/Ritual/RitualRuneObject";
constexpr const char* kInteractable = "Metadata/Terrain/Leagues/Ritual/RitualRuneInteractable";
constexpr const char* kLight = "Metadata/Terrain/Leagues/Ritual/RitualRuneLight";
bool IsBlocker(const std::string& metadata) {
    const auto slash = metadata.find_last_of("/\\");
    return metadata.compare(slash == std::string::npos ? 0 : slash + 1,
                            std::string::npos, "RitualBlocker") == 0;
}
constexpr double kInfinity = std::numeric_limits<double>::infinity();
nav::PixelPoint Position(const poe::EntitySnapshot& entity) { return {entity.x, entity.y}; }
std::string Coordinates(nav::PixelPoint point) {
    return "(" + std::to_string(point.x) + "," + std::to_string(point.y) + ")";
}
bool Validate(const Options& options, std::string& error) {
    error.clear();
    if (options.interactDistance < 6 || options.interactDistance > 40)
        error = "祭祀交互距离请填写 6–40（世界坐标单位）";
    else if (options.associationDistance < 1 || options.associationDistance > 46)
        error = "祭祀对象关联距离必须在 1–46 之间";
    else if (options.arenaRadius < 30 || options.arenaRadius > 500)
        error = "祭祀战斗半径请填写 30–500（世界坐标单位）";
    else if (options.preclearRadius < 30 || options.preclearRadius > 500)
        error = "祭祀开启前清怪半径请填写 30–500（世界坐标单位）";
    else if (options.scanMs < 200 || options.scanMs > 2000 || options.loadTimeoutSec < 3 ||
             options.loadTimeoutSec > 120 || options.battleTimeoutSec < 10 || options.battleTimeoutSec > 3600)
        error = "祭祀扫描或等待超时参数无效";
    return error.empty();
}
} // namespace

const char* StateText(PointState state) {
    switch (state) {
    case PointState::Pending: return "待前往";
    case PointState::Approaching: return "寻路接近";
    case PointState::Confirming: return "等待玩法加载";
    case PointState::Clearing: return "开启前清理周围怪物";
    case PointState::Interacting: return "暂停寻路，交互中";
    case PointState::Activating: return "等待祭祀状态确认";
    case PointState::Chasing: return "寻路接近存活怪物";
    case PointState::Fighting: return "战斗／完成确认";
    case PointState::Completed: return "已完成";
    case PointState::Unreachable: return "不可达";
    default: return "待处理";
    }
}
Automation& MainAutomation() { static Automation automation; return automation; }
Status Automation::GetStatus() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
bool Automation::GetCombatCircle(const poe::WorldAreaSnapshot& area, ritualcombat::Circle& circle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.running || !status_.showCombatCircle || !area.SameMap(status_.area)) return false;
    circle = status_.combatCircle; return true;
}
bool Automation::Prepare(const Options& options, std::string& error) {
    if (!Validate(options, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.running) { error = "祭祀任务仍在运行"; return false; }
    pause_ = false; status_.running = true; status_.paused = false; status_.showCombatCircle = false;
    status_.planOnly = options.planOnly; status_.text = "正在登记祭祀点并读取地图…"; ++status_.revision;
    return true;
}
void Automation::StartFailed(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.running = false; status_.paused = false; status_.text = error; ++status_.revision;
}
void Automation::SetPaused(bool paused) { pause_ = paused; }
bool Automation::MarkCompleted(int32_t x, int32_t y, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_); error.clear();
    if (status_.running) { error = "请先停止巡回再手动标记"; return false; }
    for (auto& point : status_.points) if (point.kind == PointKind::Ritual && point.object.x == x && point.object.y == y) {
        point.state = PointState::Completed; point.detail = "用户手动标记完成";
        status_.text = point.detail + " " + Coordinates({x, y}); ++status_.revision; return true;
    }
    error = "请先规划路线，再选择一个 RitualRuneObject"; return false;
}
bool Automation::ResetInteraction(int32_t x, int32_t y, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_); error.clear();
    if (status_.running) { error = "请先停止巡回再重试交互"; return false; }
    for (auto& point : status_.points) if (point.kind == PointKind::Ritual && point.object.x == x && point.object.y == y) {
        point.activationSent = point.activationConfirmed = point.blocker = false;
        point.activationAttempts = 0; point.activationPosition = {}; point.state = PointState::Pending;
        point.detail = "用户要求重新交互；再次开始巡回时会重新校验并发包";
        status_.text = point.detail; ++status_.revision; return true;
    }
    error = "请先规划路线，再选择一个 RitualRuneObject"; return false;
}

int Automation::Run(const Options& options, const std::vector<combo::Row>& rows,
                    std::atomic_bool& stop, const std::atomic<usecase::NavStopReason>* reason) {
    Status state = GetStatus();
    const auto previous = state;
    state.active = -1;
    state.showCombatCircle = false;
    bool ownCombo = false, combatExpected = false;
    usecase::StationaryRecovery stationaryRecovery;
    uint64_t pauseStarted = 0, pausedMs = 0;
    int result = 1;
    auto diagnostics = std::make_shared<poelog::RunDiagnostics>(app::Config().RuntimeFileW(L"ritual_logs"), L"ritual");
    state.diagnosticPath = diagnostics->Path();
    if (diagnostics->Good()) LOGI("[Ritual] diagnostics=%s", poelog::RunDiagnostics::Utf8(state.diagnosticPath).c_str());
    else LOGE("[Ritual] 诊断日志创建失败，请检查角色目录是否可写");
    diagnostics->Write("run_begin", "plan_only=" + std::to_string(options.planOnly) +
        " fight_elites=" + std::to_string(options.fightElites) + " rows=" + std::to_string(rows.size()) +
        " arena_radius=" + std::to_string(options.arenaRadius) + " preclear_radius=" + std::to_string(options.preclearRadius) +
        " scan_ms=" + std::to_string(options.scanMs) + " load_timeout_sec=" + std::to_string(options.loadTimeoutSec) +
        " battle_timeout_sec=" + std::to_string(options.battleTimeoutSec));
    for (const auto& row : rows) diagnostics->Write("skill_config", "key=" + std::to_string(row.key) +
        " name=" + row.name + " internal_id=" + row.internalId + " environment=" + std::to_string(unsigned(row.environment)) +
        " usage=" + std::to_string(unsigned(row.usage)) + " distance=" + std::to_string(row.distance) +
        " interval_ms=" + std::to_string(row.intervalMs) + " charge_ms=" + std::to_string(row.chargeMs));
    auto publish = [&](const std::string& text) {
        const bool changed = state.text != text;
        state.text = text;
        { std::lock_guard<std::mutex> lock(mutex_); state.revision = status_.revision + 1; status_ = state; }
        if (changed) LOGI("[Ritual] %s", text.c_str());
        diagnostics->Changed("status", text);
    };
    auto stopCombo = [&]() {
        if (!ownCombo) return;
        combo::MainRunner().Stop(); combo::MainRunner().Join();
        ownCombo = false; combatExpected = false;
        const auto status = combo::MainRunner().Status();
        diagnostics->Write("combo_stopped", "casts=" + std::to_string(status.casts) +
            " stop_confirmed=" + std::to_string(status.stopConfirmed) + " round_completed=" +
            std::to_string(status.roundCompleted) + " text=" + status.text);
        if (!combo::MainRunner().Status().stopConfirmed)
            throw std::runtime_error("技能停止未确认，祭祀交互未继续；请检查通信状态");
    };
    try {
        std::string error;
        if (!Validate(options, error)) throw std::runtime_error(error);
        if (!options.planOnly && std::none_of(rows.begin(), rows.end(), [](const combo::Row& row) {
                return row.environment == combo::Environment::Encounter && row.usage == combo::Usage::EveryLoop && row.distance;
            })) throw std::runtime_error("精英和终极 Boss 战斗需要至少一个有效的遇怪循环技能，请在技能连招中配置并保存");
        const auto connection = pipe::GetConnectionSnapshot();
        if (!connection.ready) throw std::runtime_error("请先连接当前角色的新版 Monitor.dll");
        GameSession session;
        if (!InitializeSession(session, &error) || session.game.pid != connection.gamePid)
            throw std::runtime_error("祭祀游戏会话无效：" + error);
        poe::WorldAreaSnapshot area;
        if (!poe::ReadWorldArea(session.memory, session.moduleBase, area, error)) throw std::runtime_error(error);
        if (options.expectedArea.Available() && !options.expectedArea.SameMap(area))
            throw std::runtime_error("祭祀列表来自旧地图实例，请重新扫描");
        if (!previous.area.SameMap(area)) state.returnHome = {};
        if (!options.planOnly && state.returnHome.enterSubmitted)
            throw std::runtime_error("本地图已提交过进门请求，未自动重发；请先确认游戏切图结果");
        auto localEntity = [&]() {
            auto address = session.memory.Follow(session.moduleBase + BaseOffset);
            for (int i = 0; address && i < kLocalPlayerChainDepth; ++i)
                address = session.memory.Follow(address + kMapObjectChain[i]);
            return address;
        };
        const auto player = localEntity();
        if (!player) throw std::runtime_error("无法定位当前角色");
        diagnostics->Write("session", "pid=" + std::to_string(connection.gamePid) +
            " session=" + std::to_string(connection.sessionId) + " player=" + std::to_string(player) +
            " map_owner=" + std::to_string(area.mapOwner) + " world=" + std::to_string(area.world) +
            " area=" + area.id + " name=" + area.name);
        poe::Entity self(session.memory, player);
        const auto positioned = self.GetAs<poe::PositionComponent>("Positioned");
        const auto life = self.GetAs<poe::LifeComponent>("Life");
        if (!positioned || !life) throw std::runtime_error("无法读取当前角色坐标或生命组件");
        auto health = [&]() {
            if (stop.load()) throw std::runtime_error("祭祀任务已停止");
            if (!app::Config().TargetAlive() || !connection.Same(pipe::GetConnectionSnapshot()))
                throw std::runtime_error("游戏或通信会话已变化，祭祀任务停止");
            poe::WorldAreaSnapshot current;
            if (!poe::ReadWorldArea(session.memory, session.moduleBase, current, error) || !area.SameMap(current) ||
                localEntity() != player) throw std::runtime_error("地图实例或角色已变化，祭祀任务停止");
            uint32_t hp = 0;
            if (!session.memory.ReadInto(life->Ptr() + kLife_HpOff, &hp, sizeof(hp)) || !hp)
                throw std::runtime_error("角色已死亡或生命状态不可读，祭祀任务停止");
            if (combatExpected && !combo::MainRunner().Status().running)
                throw std::runtime_error("技能循环已结束：" + combo::MainRunner().Status().text);
        };
        auto playerPosition = [&]() {
            nav::PixelPoint position;
            if (!session.memory.ReadInto(positioned->Ptr() + kPositioned_EntXOff, &position.x, sizeof(position.x)) ||
                !session.memory.ReadInto(positioned->Ptr() + kPositioned_EntYOff, &position.y, sizeof(position.y)))
                throw std::runtime_error("角色坐标暂时不可读");
            return position;
        };
        auto wait = [&](uint32_t duration) {
            const auto end = GetTickCount64() + duration;
            while (GetTickCount64() < end) {
                health(); const auto now = GetTickCount64();
                if (now < end) Sleep(DWORD((std::min)(uint64_t(50), end - now)));
            }
        };
        auto scan = [&]() {
            std::vector<poe::EntitySnapshot> entities;
            const auto started = GetTickCount64();
            while (true) {
                health(); poe::EntityScanInfo info;
                const bool scanned = poe::SurroundingsTraverser(session.memory).ListRitualCandidates(session.moduleBase, entities, info, &stop);
                diagnostics->Changed("ritual_scan", poe::ScanSummary(info, entities.size()));
                if (scanned) {
                    health(); return entities;
                }
                stopCombo();
                publish("祭祀扫描暂时不可读，保留当前巡回并重试：" + info.error);
                if (GetTickCount64() - started >= uint64_t(options.loadTimeoutSec) * 1000)
                    throw std::runtime_error("祭祀实体树持续不可读：" + info.error);
                wait(options.scanMs);
            }
        };
        auto collectObjects = [&](const std::vector<poe::EntitySnapshot>& entities) {
            std::vector<poe::EntitySnapshot> objects;
            for (const auto& entity : entities) if (entity.metadataPath == kObject) objects.push_back(entity);
            std::sort(objects.begin(), objects.end(), [](const auto& a, const auto& b) {
                return a.x != b.x ? a.x < b.x : a.y < b.y;
            });
            objects.erase(std::unique(objects.begin(), objects.end(), [](const auto& a, const auto& b) {
                return a.x == b.x && a.y == b.y;
            }), objects.end());
            return objects;
        };
        auto objects = collectObjects(scan());
        bool stable = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            wait(options.scanMs); auto next = collectObjects(scan());
            stable = !objects.empty() && next.size() == objects.size();
            if (stable) for (size_t i = 0; i < objects.size(); ++i)
                if (objects[i].x != next[i].x || objects[i].y != next[i].y) stable = false;
            objects = std::move(next); if (stable) break;
        }
        if (!stable) throw std::runtime_error(objects.empty() ? "没有扫描到 RitualRuneObject，请确认已进入祭祀地图" :
                                             "祭祀点尚未稳定，请等待地图加载后重新开始");
        if (objects.size() > 64) throw std::runtime_error("祭祀位置数量异常（超过 64），请检查扫描结果");
        state.area = area; state.points.clear(); state.route.clear(); state.active = -1;
        // Keep the whole instance ledger even when an earlier anchor is absent
        // from this snapshot. Later discovery must not forget a submitted click.
        if (previous.area.SameMap(area)) {
            for (const auto& point : previous.points)
                if (point.kind != PointKind::Elite || options.fightElites) state.points.push_back(point);
            for (auto& point : state.points) {
                point.order = 0; point.pathDistance = -1;
                point.interactable = point.light = point.blocker = false;
                point.ritual = {};
                point.activationConfirmed = false;
                if (point.state != PointState::Completed) {
                    point.state = PointState::Pending; point.detail.clear();
                }
            }
        }
        auto updateObject = [&](Point& point, const poe::EntitySnapshot& object) {
            point.object = object; point.ritual = object.ritual;
            if (object.ritual.Completed()) {
                point.state = PointState::Completed;
                point.detail = "current_state=3，本节点已完成";
            }
        };
        for (const auto& object : objects) {
            const auto known = std::find_if(state.points.begin(), state.points.end(), [&](const Point& point) {
                return point.kind == PointKind::Ritual && point.object.x == object.x && point.object.y == object.y;
            });
            if (known != state.points.end()) { updateObject(*known, object); continue; }
            if (std::count_if(state.points.begin(), state.points.end(), [](const Point& p) { return p.kind == PointKind::Ritual; }) >= 64) throw std::runtime_error("祭祀位置数量异常（超过 64）");
            Point point; updateObject(point, object);
            state.points.push_back(std::move(point));
        }
        publish("已登记 " + std::to_string(objects.size()) + " 个祭祀点，正在读取 A* 地图");
        mapdata::CostBuffer cost;
        if (usecase::FetchCostBuffer(cost, "RitualAuto", &stop) != 0 || cost.Empty())
            throw std::runtime_error("无法读取当前地图代价表");
        health();
        nav::PixelMap map;
        if (!map.BuildFrom(cost)) throw std::runtime_error("当前地图没有可走区域");
        auto startPosition = playerPosition();
        auto whitePosition = [&](nav::PixelPoint point) {
            if (map.Passable(point.x, point.y)) return point;
            nav::PixelPoint best = point; double distance = kInfinity;
            for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
                nav::PixelPoint next{point.x + dx, point.y + dy}; const auto d = nav::PixelDistance(point, next);
                if (d <= 8 && d < distance && map.Passable(next.x, next.y)) { distance = d; best = next; }
            }
            if (!std::isfinite(distance)) throw std::runtime_error("角色附近没有可走区域");
            return best;
        };
        const auto cancelPlanning = [&]() { return stop.load() || !connection.Same(pipe::GetConnectionSnapshot()); };
        auto pathLength = [&](nav::PixelPoint from, nav::PixelPoint to) {
            nav::PixelPath path;
            if (!nav::FindPixelPath(map, from, to, path, cancelPlanning)) {
                health(); return kInfinity;
            }
            return path.length;
        };
        auto whiteStart = whitePosition(startPosition);
        // One connectivity pass avoids falsely rejecting a point because its
        // geometrically closest white pixels happen to lie across a wall.
        std::vector<uint8_t> connected;
        auto rebuildConnectivity = [&]() {
            whiteStart = whitePosition(playerPosition());
            connected.assign(map.PassableCount(), 0);
            std::vector<uint32_t> pendingWhite{uint32_t(map.Id(whiteStart.x, whiteStart.y))};
            connected[pendingWhite.front()] = 1;
            for (size_t cursor = 0; cursor < pendingWhite.size(); ++cursor) {
                if ((cursor & 4095) == 0) health();
                const auto from = map.Point(pendingWhite[cursor]);
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const nav::PixelPoint next{from.x + dx, from.y + dy}; const auto id = map.Id(next.x, next.y);
                    if (id < 0 || connected[size_t(id)] || !nav::PixelLineClear(map, from, next)) continue;
                    connected[size_t(id)] = 1; pendingWhite.push_back(uint32_t(id));
                }
            }
            // Keep only the compact component bitset after the breadth-first pass.
            std::vector<uint32_t>().swap(pendingWhite);
        };
        rebuildConnectivity();
        auto approachFor = [&](nav::PixelPoint center, nav::PixelPoint from, nav::PixelPoint& out) {
            double best = kInfinity;
            const int radius = options.interactDistance - 2;
            for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                nav::PixelPoint candidate{center.x + dx, center.y + dy};
                const auto distance = nav::PixelDistance(center, candidate);
                const auto id = map.Id(candidate.x, candidate.y);
                if (distance > radius || id < 0 || !connected[size_t(id)]) continue;
                // Stand near the object without aiming movement at its colliding center.
                const auto score = std::abs(distance - (std::min)(6, radius)) + nav::PixelDistance(from, candidate) * 0.001;
                if (score < best) { best = score; out = candidate; }
            }
            return std::isfinite(best);
        };
        auto preparePoint = [&](Point& point) {
            if (point.state == PointState::Completed) return;
            if (!cost.Geo().Contains(point.object.x, point.object.y)) {
                point.state = PointState::Unreachable; point.detail = "目标坐标超出地图或尚未加载"; return;
            }
            const auto center = point.activationSent ? point.activationPosition : Position(point.object);
            if (!approachFor(center, whiteStart, point.approach)) {
                point.state = PointState::Unreachable; point.detail = "目标附近没有可达位置";
            }
        };
        for (auto& point : state.points) preparePoint(point);
        auto discover = [&]() {
            for (const auto& object : collectObjects(scan())) {
                const auto known = std::find_if(state.points.begin(), state.points.end(), [&](const Point& point) {
                    return point.kind == PointKind::Ritual && point.object.x == object.x && point.object.y == object.y;
                });
                if (known != state.points.end()) { updateObject(*known, object); continue; }
                if (std::count_if(state.points.begin(), state.points.end(), [](const Point& p) { return p.kind == PointKind::Ritual; }) >= 64) throw std::runtime_error("祭祀位置数量异常（超过 64）");
                Point point; updateObject(point, object); preparePoint(point); state.points.push_back(std::move(point));
                publish("补充发现祭祀点 " + Coordinates(Position(object)));
            }
        };
        bool finalPhase = false;
        auto encounterScan = [&]() {
            std::vector<poe::EntitySnapshot> entities;
            const auto started = GetTickCount64();
            while (true) {
                health(); poe::EntityScanInfo info;
                const bool scanned = poe::SurroundingsTraverser(session.memory).ListEncounterTargets(session.moduleBase, area, entities, info, &stop);
                diagnostics->Changed("encounter_scan", poe::ScanSummary(info, entities.size()));
                if (scanned) {
                    health(); return entities;
                }
                stopCombo(); publish("精英／Boss 扫描暂不可读，保留目标重试：" + info.error);
                if (GetTickCount64() - started >= uint64_t(options.loadTimeoutSec) * 1000)
                    throw std::runtime_error("精英／Boss 扫描持续失败，未判定击杀：" + info.error);
                wait(options.scanMs);
            }
        };
        auto recordTargets = [&](const std::vector<poe::EntitySnapshot>& entities, bool allowNew = true) {
            for (const auto& entity : entities) {
                const bool entrance = monster::BossEntrance(entity.metadataPath);
                const bool boss = entity.combat.monster && entity.combat.isCombatOwner && entity.combat.configuredForArea;
                const bool elite = monster::OptionalElite(entity.combat);
                if (!entrance && !boss && !elite) continue;
                const auto kind = entrance ? PointKind::BossEntrance : boss ? PointKind::MapBoss : PointKind::Elite;
                if (kind == PointKind::Elite && !options.fightElites) continue;
                auto known = std::find_if(state.points.begin(), state.points.end(), [&](const Point& point) {
                    return point.kind == kind && point.object.key == entity.key && point.object.entity == entity.entity &&
                        monster::Canonical(point.object.metadataPath) == monster::Canonical(entity.metadataPath);
                });
                if (known == state.points.end()) {
                    if (!allowNew) continue;
                    if ((kind == PointKind::Elite && (finalPhase || !entity.hp || !entity.combat.positionValid)) ||
                        (entrance && !entity.combat.positionValid)) continue;
                    if (state.points.size() >= 512) throw std::runtime_error("路线目标超过 512，请检查扫描结果");
                    Point point; point.kind = kind; point.object = entity;
                    state.points.push_back(std::move(point)); known = state.points.end() - 1;
                }
                const bool moved = known->object.x != entity.x || known->object.y != entity.y;
                known->object = entity;
                if (!entrance && monster::ConfirmedDead(entity.combat) &&
                    (kind != PointKind::MapBoss || monster::MapBoss(entity.combat))) {
                    known->state = PointState::Completed; known->detail = "已读取主体 HP=0，确认击杀";
                } else if (!entrance && entity.hp && known->state == PointState::Completed) {
                    known->state = PointState::Pending; known->detail = "目标重新存活，继续处理";
                }
                if (kind == PointKind::MapBoss && !monster::MapBoss(entity.combat) && known->state != PointState::Completed) {
                    known->state = PointState::Confirming; known->detail = "地图配置目标的运行时 Boss 证据待确认";
                }
                if (moved || known->state != PointState::Completed) {
                    if (known->state == PointState::Unreachable) known->state = PointState::Pending;
                    preparePoint(*known);
                }
            }
        };
        auto discoverTargets = [&]() { recordTargets(encounterScan()); };
        auto finalTarget = [&]() -> int {
            for (size_t i = 0; i < state.points.size(); ++i) {
                const auto& point = state.points[i];
                if (point.kind == PointKind::MapBoss && point.state != PointState::Completed &&
                    point.state != PointState::Unreachable && monster::Destination(point.object.combat)) return int(i);
            }
            const bool allBossesDead = std::any_of(state.points.begin(), state.points.end(), [](const Point& p) {
                return p.kind == PointKind::MapBoss;
            }) && std::none_of(state.points.begin(), state.points.end(), [](const Point& p) {
                return p.kind == PointKind::MapBoss && p.state != PointState::Completed;
            });
            if (allBossesDead) return -1;
            for (size_t i = 0; i < state.points.size(); ++i) {
                const auto& point = state.points[i];
                if (point.kind == PointKind::BossEntrance && point.state != PointState::Unreachable) return int(i);
            }
            return -1;
        };
        auto plan = [&]() {
            discover(); discoverTargets();
            health(); publish("正在规划祭祀／精英混合路线，终极 Boss 固定最后…");
            const auto from = whitePosition(playerPosition());
            std::vector<size_t> pending;
            for (size_t i = 0; i < state.points.size(); ++i) {
                auto& point = state.points[i]; point.order = 0;
                if (encounterroute::Mixed(point.kind, options.fightElites) &&
                    point.state != PointState::Completed && point.state != PointState::Unreachable) pending.push_back(i);
            }
            const int terminal = finalTarget();
            // Full pairwise A* is bounded to small itineraries. A map can have
            // hundreds of blue/yellow mobs: order those geometrically with
            // bounded 2-opt, then connect every selected leg using actual A*.
            const bool exact = pending.size() <= 12;
            auto measure = [&](nav::PixelPoint a, nav::PixelPoint b) {
                return exact ? pathLength(a, b) : nav::PixelDistance(a, b);
            };
            std::vector<double> start(pending.size()), tail(pending.size(), 0);
            std::vector<std::vector<double>> distance(pending.size(), std::vector<double>(pending.size(), 0));
            for (size_t i = 0; i < pending.size(); ++i) {
                health(); start[i] = measure(from, state.points[pending[i]].approach);
                if (terminal >= 0) tail[i] = measure(state.points[pending[i]].approach, state.points[size_t(terminal)].approach);
                for (size_t j = 0; j < i; ++j)
                    distance[i][j] = distance[j][i] = measure(state.points[pending[i]].approach, state.points[pending[j]].approach);
            }
            const auto order = encounterroute::VisitOrder(distance, start, tail);
            if (order.size() != pending.size()) throw std::runtime_error("祭祀／精英之间没有完整可达路线");
            state.route.clear(); state.route.push_back(from);
            auto appendRoute = [&](Point& point) {
                nav::PixelPath path; nav::PixelRoute compact;
                if (!nav::FindPixelPath(map, state.route.back(), point.approach, path, cancelPlanning) ||
                    !compact.Build(map, path.points, {}, cancelPlanning))
                    throw std::runtime_error("目标没有完整 A* 路线：" + point.object.name);
                point.pathDistance = path.length;
                const auto& points = compact.Points();
                if (points.size() > 1) state.route.insert(state.route.end(), points.begin() + 1, points.end());
            };
            std::vector<size_t> result;
            for (size_t i = 0; i < order.size(); ++i) {
                const auto index = pending[order[i]]; auto& point = state.points[index];
                point.order = uint32_t(i + 1);
                point.pathDistance = i ? distance[order[i - 1]][order[i]] : start[order[i]];
                result.push_back(index); appendRoute(point);
            }
            if (terminal >= 0) {
                auto& point = state.points[size_t(terminal)]; point.order = uint32_t(result.size() + 1);
                point.pathDistance = pathLength(state.route.back(), point.approach); appendRoute(point);
            }
            publish(std::string(exact ? "路线已规划：" : "路线已规划（多目标近似排序＋逐段 A*）：") + "祭祀／精英剩余 " + std::to_string(result.size()) + " 站；" +
                (terminal < 0 ? "终极 Boss 位置待加载，完成前段后继续确认" :
                 std::string("最后前往") + encounterroute::Text(state.points[size_t(terminal)].kind) + " " +
                 state.points[size_t(terminal)].object.name));
            return result;
        };
        auto order = plan();
        if (options.planOnly) { result = 0; publish(state.text + "；仅规划，尚未开始巡回"); }
        else {
            auto startCombo = [&](bool singleRound = false) {
                if (ownCombo) return;
                health(); combo::StartOptions start; start.cancel = &stop;
                start.singleRound = singleRound;
                start.diagnostics = diagnostics;
                if (!combo::MainRunner().Start(rows, error, start)) throw std::runtime_error("技能循环启动失败：" + error);
                ownCombo = true; combatExpected = !singleRound;
            };
            auto pauseGate = [&]() {
                health();
                if (pause_.load()) {
                    if (!state.paused) {
                        stationaryRecovery.timer.Reset();
                        pauseStarted = GetTickCount64(); stopCombo(); state.paused = true;
                        publish("祭祀巡回已暂停；保留当前目标和完成进度");
                    }
                    return true;
                }
                if (state.paused) {
                    pausedMs += GetTickCount64() - pauseStarted; pauseStarted = 0; state.paused = false;
                    publish("继续祭祀巡回");
                }
                return false;
            };
            auto waitPause = [&]() { while (pauseGate()) wait(50); };
            auto logicalNow = [&]() { return GetTickCount64() - pausedMs - (pauseStarted ? GetTickCount64() - pauseStarted : 0); };
            auto setPoint = [&](size_t index, PointState phase, const std::string& detail) {
                if (phase != PointState::Approaching && phase != PointState::Chasing && phase != PointState::Clearing)
                    stationaryRecovery.timer.Reset();
                state.active = int(index); state.points[index].state = phase; state.points[index].detail = detail;
                publish(std::string(encounterroute::Text(state.points[index].kind)) + " " + Coordinates(Position(state.points[index].object)) + "：" + detail);
            };
            struct Observed {
                bool object = false, light = false, blocker = false;
                poe::EntitySnapshot interactable;
                ritualstate::Snapshot ritual;
                bool CanStart() const { return object && ritual.CanStart() && interactable.entity; }
            };
            auto observe = [&](size_t index) {
                const auto entities = scan(); Observed observed;
                for (const auto& entity : entities) {
                    if (entity.metadataPath == kObject && nav::PixelDistance(Position(entity), Position(state.points[index].object)) <= 2) {
                        if (observed.object) throw std::runtime_error("同一祭祀点有多个锚点，已停止交互");
                        observed.object = true; observed.ritual = entity.ritual;
                        state.points[index].object = entity;
                    }
                    const bool blocker = IsBlocker(entity.metadataPath);
                    if (entity.metadataPath != kInteractable && entity.metadataPath != kLight && !blocker) continue;
                    size_t nearest = state.points.size(); double best = kInfinity, second = kInfinity;
                    for (size_t i = 0; i < state.points.size(); ++i) {
                        if (state.points[i].kind != PointKind::Ritual) continue;
                        const auto d = nav::PixelDistance(Position(entity), Position(state.points[i].object));
                        if (d < best) { second = best; best = d; nearest = i; }
                        else if (d < second) second = d;
                    }
                    if (nearest != index || best > (blocker ? options.arenaRadius : options.associationDistance)) continue;
                    if (second - best < 1) throw std::runtime_error("祭祀对象坐标归属不唯一，已停止交互");
                    if (blocker) observed.blocker = true;
                    else if (entity.metadataPath == kLight) observed.light = true;
                    else {
                        if (observed.interactable.entity) throw std::runtime_error("同一祭祀点有多个交互目标，已停止交互");
                        observed.interactable = entity;
                    }
                }
                // The anchor remains available at long distance. If a local
                // interactable is loaded, both state samples must agree.
                if (observed.interactable.entity) {
                    const auto& current = observed.interactable.ritual;
                    if (!observed.ritual.valid || !current.valid || observed.ritual.current != current.current)
                        observed.ritual = {};
                    else observed.ritual = current;
                }
                auto& point = state.points[index]; point.light = observed.light; point.blocker = observed.blocker;
                point.interactable = observed.interactable.entity != 0;
                point.ritual = observed.ritual;
                diagnostics->Changed("ritual_state_" + std::to_string(index), "object=" + std::to_string(observed.object) +
                    " interactable=" + std::to_string(observed.interactable.entity) + " position=" + Coordinates(Position(point.object)) +
                    " anchor_state=" + ritualstate::Text(point.object.ritual) +
                    " interactable_state=" + ritualstate::Text(observed.interactable.ritual) +
                    " resolved_state=" + ritualstate::Text(observed.ritual) +
                    " light=" + std::to_string(observed.light) + " blocker=" + std::to_string(observed.blocker));
                return observed;
            };
            if (!order.empty() || finalTarget() >= 0) { waitPause(); startCombo(); }
            usecase::NavigateOptions navigation; usecase::LoadNavOptionsFromIni(navigation);
            navigation.dryRun = false; navigation.singleShot = false; navigation.follow = nullptr;
            navigation.presetCost = &cost; navigation.presetPixelMap = &map; navigation.presetSessionId = connection.sessionId;
            navigation.arriveTolerance = int16_t((std::min)(3, options.interactDistance / 3));
            navigation.stationaryRecovery = &stationaryRecovery;
            stationaryRecovery.perform = [&](pipe::NamedPipeClie& client, nav::PixelPoint from, nav::PixelPoint toward) {
                health(); stopCombo(); health();
                if (pause_.load()) return usecase::NavControl{usecase::NavDirective::Hold, "用户暂停，翻滚未发送"};
                const auto fresh = playerPosition();
                if (fresh.x != from.x || fresh.y != from.y)
                    return usecase::NavControl{usecase::NavDirective::Continue, "角色已移动，取消本次脱困翻滚"};
                // Target is the current A* steering point, rather than the final
                // ritual/monster position across a corner. Native ID call accepts
                // world coordinates and performs its own packet coordinate conversion.
                publish("寻路坐标连续 5 秒不变，朝寻路方向翻滚一次 " + Coordinates(toward));
                LOGI("[RitualRoll] stationary=(%d,%d) toward=(%d,%d)", from.x, from.y, toward.x, toward.y);
                if (!combo::DodgeRollOnce(client, player, area.world, toward.x, toward.y, error))
                    return usecase::NavControl{usecase::NavDirective::Fail, error};
                // Give the single roll its animation window before another move
                // or the normal travel combo is allowed to replace the action.
                const auto settleUntil = GetTickCount64() + 600;
                while (GetTickCount64() < settleUntil) {
                    health();
                    if (pause_.load()) return usecase::NavControl{usecase::NavDirective::Hold, "翻滚后用户暂停"};
                    wait(40);
                }
                health(); return usecase::NavControl{};
            };
            navigation.control = [&](nav::PixelPoint) {
                if (pauseGate()) return usecase::NavControl{usecase::NavDirective::Hold, "用户暂停巡回"};
                startCombo();
                return usecase::NavControl{};
            };
            auto travelTo = [&](nav::PixelPoint target, const std::string& label) {
                while (true) {
                    waitPause(); startCombo(); navigation.goalX = target.x; navigation.goalY = target.y;
                    usecase::NavTelemetry telemetry;
                    const int rc = usecase::Navigate(navigation, &telemetry, &stop, reason);
                    health();
                    if (!rc) { stationaryRecovery.timer.Reset(); return; }
                    if (rc != 4) throw std::runtime_error(label + "失败：" + telemetry.note);
                    publish(label + "受到移动或障碍影响，从当前坐标重新规划：" + telemetry.note);
                    wait(options.scanMs);
                }
            };
            auto monsters = [&]() {
                std::vector<poe::EntitySnapshot> result;
                const auto started = GetTickCount64();
                while (true) {
                    health(); poe::EntityScanInfo info;
                    const bool scanned = poe::SurroundingsTraverser(session.memory).ListMonsters(session.moduleBase, result, info, &stop);
                    diagnostics->Changed("monster_scan", poe::ScanSummary(info, result.size()));
                    if (scanned) {
                        health();
                        // Capture incidental kills before corpses leave the tree.
                        // Do not append targets while a ritual holds a Point reference.
                        const bool possibleKill = std::any_of(state.points.begin(), state.points.end(), [&](const Point& point) {
                            return (point.kind == PointKind::Elite || point.kind == PointKind::MapBoss) &&
                                point.state != PointState::Completed && std::any_of(result.begin(), result.end(), [&](const auto& item) {
                                    return !item.hp && item.key == point.object.key && item.entity == point.object.entity &&
                                        monster::Canonical(item.metadataPath) == monster::Canonical(point.object.metadataPath);
                                });
                        });
                        if (possibleKill) recordTargets(encounterScan(), false);
                        return result;
                    }
                    stopCombo();
                    publish("怪物扫描暂时不可读，保留巡回并重试；不据此判定清场：" + info.error);
                    if (GetTickCount64() - started >= uint64_t(options.loadTimeoutSec) * 1000)
                        throw std::runtime_error("怪物实体树持续不可读，未判定清场：" + info.error);
                    wait(options.scanMs);
                }
            };
            // The scheduler, rather than a guessed delay or cast count, determines
            // the end of one full round (including holds, gaps and skipped rows).
            auto oneRound = [&](const std::function<bool()>& keepRunning) {
                stationaryRecovery.timer.Reset();
                stopCombo();
                while (true) {
                    waitPause(); health();
                    if (!keepRunning()) {
                        diagnostics->Write("round_cancelled", "目标死亡、卸载、移出攻击距离或玩法状态变化，重新选怪");
                        stopCombo(); return;
                    }
                    if (!ownCombo) startCombo(true);
                    const auto status = combo::MainRunner().Status();
                    if (!status.running) {
                        const bool completed = status.roundCompleted && status.stopConfirmed;
                        stopCombo();
                        if (!completed) throw std::runtime_error("单轮技能未正常完成：" + status.text);
                        return;
                    }
                    wait(options.scanMs);
                }
            };
            auto refreshMap = [&]() {
                stopCombo(); health(); mapdata::CostBuffer latest;
                if (usecase::FetchCostBuffer(latest, "RitualBossRoute", &stop) != 0 || latest.Empty())
                    throw std::runtime_error("精英／Boss 区域地图刷新失败，保留目标");
                health();
                if (!(latest.Geo() == cost.Geo())) throw std::runtime_error("当前地图尺寸变化，请重新规划");
                cost = std::move(latest);
                if (!map.BuildFrom(cost)) throw std::runtime_error("当前地图没有可走区域");
                rebuildConnectivity();
                for (auto& point : state.points) {
                    if (point.state == PointState::Unreachable) point.state = PointState::Pending;
                    preparePoint(point);
                }
            };
            auto fightTarget = [&](size_t index) {
                stopCombo(); state.showCombatCircle = false;
                const auto started = logicalNow(); uint64_t missingSince = 0, refreshedAt = started;
                bool returnedToLast = false;
                const uint32_t attackRange = ritualcombat::AttackRange(rows);
                const double standOff = (std::max)(1.0, double(attackRange) * 0.75);
                auto currentTarget = [&](poe::EntitySnapshot& target) {
                    const auto entities = encounterScan(); recordTargets(entities);
                    const auto& wanted = state.points[index];
                    for (const auto& item : entities) if (item.key == wanted.object.key && item.entity == wanted.object.entity &&
                        monster::Canonical(item.metadataPath) == monster::Canonical(wanted.object.metadataPath) &&
                        (wanted.kind == PointKind::MapBoss ? monster::MapBoss(item.combat) : monster::OptionalElite(item.combat))) {
                        target = item; return true;
                    }
                    return false;
                };
                while (true) {
                    stopCombo(); waitPause(); health();
                    poe::EntitySnapshot target;
                    const bool found = currentTarget(target);
                    if (state.points[index].state == PointState::Completed) {
                        setPoint(index, PointState::Completed, "已读取主体 HP=0，确认击杀"); return;
                    }
                    if (logicalNow() - started >= uint64_t(options.battleTimeoutSec) * 1000)
                        throw std::runtime_error("精英／Boss 战斗超时，尚未确认目标死亡");
                    if (!found || !target.combat.hpValid || !target.combat.positionValid) {
                        if (!missingSince) missingSince = logicalNow();
                        setPoint(index, PointState::Confirming, "目标暂未加载，返回最后位置重扫；未判定击杀");
                        if (!returnedToLast && state.points[index].object.combat.positionValid) {
                            refreshMap(); nav::PixelPoint approach;
                            if (approachFor(Position(state.points[index].object), playerPosition(), approach)) {
                                travelTo(approach, "重新加载精英／Boss"); stopCombo();
                            }
                            returnedToLast = true; missingSince = logicalNow();
                        }
                        if (logicalNow() - missingSince >= uint64_t(options.loadTimeoutSec) * 1000)
                            throw std::runtime_error("目标持续未加载，未获得 HP=0 证据；请重新规划或检查目标");
                        wait(options.scanMs); continue;
                    }
                    missingSince = 0; returnedToLast = false;
                    if (logicalNow() - refreshedAt >= uint64_t(options.loadTimeoutSec) * 1000) {
                        refreshMap(); refreshedAt = logicalNow();
                    }
                    const auto position = playerPosition();
                    poe::EntitySnapshot selected; nav::PixelPoint goal;
                    if (!ritualcombat::ChooseTarget(std::vector<poe::EntitySnapshot>{target},
                            {Position(target), options.arenaRadius}, map, position, standOff, selected, goal, cancelPlanning)) {
                        setPoint(index, PointState::Confirming, "目标附近暂不可达，刷新地图后重新规划");
                        refreshMap(); wait(options.scanMs); continue;
                    }
                    auto stillAlive = [&](nav::PixelPoint current, bool casting) {
                        if (logicalNow() - started >= uint64_t(options.battleTimeoutSec) * 1000) return false;
                        poe::EntitySnapshot fresh;
                        if (!currentTarget(fresh) || !fresh.hp || !fresh.combat.hpValid || !fresh.combat.positionValid) return false;
                        return casting ? nav::PixelDistance(current, Position(fresh)) <= attackRange + 4 :
                            nav::PixelDistance(Position(fresh), Position(target)) <= 6;
                    };
                    if (ritualcombat::InAttackPosition(map, position, Position(target), goal, attackRange)) {
                        setPoint(index, PointState::Fighting, target.name + "，主体 HP=" + std::to_string(target.hp) + "，释放一轮技能");
                        oneRound([&]() { return stillAlive(playerPosition(), true); }); continue;
                    }
                    setPoint(index, PointState::Chasing, "A* 接近 " + target.name + " " + Coordinates(Position(target)));
                    auto chase = navigation; chase.goalX = goal.x; chase.goalY = goal.y; chase.arriveTolerance = 1;
                    chase.stepDistance = (std::min)(navigation.stepDistance, int32_t(30));
                    const auto chaseStarted = logicalNow(); auto checkedAt = chaseStarted - options.scanMs;
                    chase.control = [&](nav::PixelPoint current) {
                        if (pauseGate()) return usecase::NavControl{usecase::NavDirective::Hold, "用户暂停巡回"};
                        if (logicalNow() - chaseStarted >= 1500)
                            return usecase::NavControl{usecase::NavDirective::Arrived, "重新定位移动目标"};
                        if (logicalNow() - checkedAt < options.scanMs) return usecase::NavControl{};
                        checkedAt = logicalNow();
                        if (!stillAlive(current, false)) {
                            stationaryRecovery.timer.Reset();
                            return usecase::NavControl{usecase::NavDirective::Arrived, "目标状态或位置变化"};
                        }
                        return usecase::NavControl{};
                    };
                    usecase::NavTelemetry telemetry;
                    const int rc = usecase::Navigate(chase, &telemetry, &stop, reason); health();
                    if (rc && rc != 4) throw std::runtime_error("精英／Boss 追怪失败：" + telemetry.note);
                    if (rc == 4) { refreshMap(); wait(options.scanMs); }
                }
            };
            while (!order.empty()) {
                waitPause(); const size_t index = order.front(); auto& point = state.points[index];
                state.showCombatCircle = false;
                if (point.kind == PointKind::Elite) {
                    fightTarget(index); order = plan(); continue;
                }
                setPoint(index, PointState::Approaching, "沿已规划的 A* 路线接近祭祀点");
                travelTo(point.approach, "祭祀接近");
                const auto activationWaitMs = (std::max)(uint64_t(1500), uint64_t(options.scanMs) * 3);
                auto resolveActivation = [&](const Observed& observed) {
                    if (observed.object && observed.ritual.Completed()) {
                        setPoint(index, PointState::Completed, "current_state=3，本节点已完成");
                        return true;
                    }
                    if (!observed.object || !observed.ritual.Fighting()) return false;
                    if (!point.activationSent && !point.activationConfirmed)
                        point.activationPosition = observed.interactable.entity ? Position(observed.interactable) : Position(point.object);
                    point.activationConfirmed = true;
                    setPoint(index, PointState::Fighting, "current_state=2，确认祭祀进行中");
                    return true;
                };
                mapdata::CostBuffer arenaCost;
                nav::PixelMap arenaMap;
                ritualcombat::Circle arena;
                bool haveArena = false, arenaPreclear = false;
                uint64_t refreshedAt = 0;
                const uint32_t attackRange = ritualcombat::AttackRange(rows);
                const double standOff = (std::max)(1.0, double(attackRange) * 0.75);
                auto ensureArena = [&](ritualcombat::Circle circle, bool preclear) {
                    const uint64_t refreshMs = uint64_t(preclear ? options.loadTimeoutSec : options.battleTimeoutSec) * 1000;
                    if (haveArena && arenaPreclear == preclear && arena.center.x == circle.center.x && arena.center.y == circle.center.y &&
                        arena.radius == circle.radius && logicalNow() - refreshedAt < refreshMs) return;
                    health(); mapdata::CostBuffer latest;
                    const int captured = usecase::FetchCostBuffer(latest, preclear ? "RitualPreclear" : "RitualCombat", &stop);
                    health(); const auto& source = captured == 0 && !latest.Empty() ? latest : cost;
                    if (!(source.Geo() == cost.Geo())) throw std::runtime_error("祭祀地图尺寸发生变化，请重新规划");
                    ritualcombat::ClipCost(source, circle, arenaCost);
                    if (!arenaMap.BuildFrom(arenaCost)) throw std::runtime_error("当前祭祀范围内没有可走区域，请检查半径");
                    arena = circle; haveArena = true; arenaPreclear = preclear; refreshedAt = logicalNow();
                };
                // Opening preclear retains its target/one-round behavior. Active
                // rituals use independent roaming below, regardless of skill range.
                auto preclearStep = [&]() {
                    stopCombo(); waitPause(); health();
                    const ritualcombat::Circle circle{Position(point.object), options.preclearRadius};
                    ensureArena(circle, true);
                    state.combatCircle = circle; state.showCombatCircle = true;
                    auto combatContinues = [&]() {
                        const auto latest = observe(index);
                        return ritualcombat::Next(latest.object, latest.ritual, latest.interactable.entity != 0) ==
                            ritualcombat::Phase::Clearing;
                    };
                    if (!combatContinues()) return;
                    const auto position = playerPosition();
                    if (!circle.Contains(position)) {
                        setPoint(index, PointState::Clearing,
                            "角色偏离祭祀范围，返回本点重新检查状态");
                        travelTo(point.approach, "返回祭祀范围"); stopCombo(); return;
                    }
                    const auto alive = monsters();
                    if (!combatContinues()) return;
                    poe::EntitySnapshot target; nav::PixelPoint goal;
                    const auto count = std::count_if(alive.begin(), alive.end(), [&](const auto& monster) {
                        return ritualcombat::AliveInside(monster, circle, arenaMap);
                    });
                    const auto prefix = std::string("开启前清怪，圆心 ") +
                        Coordinates(circle.center) + "，半径 " + std::to_string(circle.radius) +
                        "，存活怪物 " + std::to_string(count) + "；";
                    const bool found = ritualcombat::ChooseTarget(alive, circle, arenaMap, position, standOff,
                        target, goal, cancelPlanning);
                    diagnostics->Changed("target_selection", "preclear=1"
                        " player=" + Coordinates(position) + " center=" + Coordinates(circle.center) +
                        " radius=" + std::to_string(circle.radius) + " scanned=" + std::to_string(alive.size()) +
                        " alive_inside=" + std::to_string(count) + " found=" + std::to_string(found) +
                        " key=" + std::to_string(target.key) + " hp=" + std::to_string(target.hp) +
                        " target=" + Coordinates(Position(target)) + " goal=" + Coordinates(goal) +
                        " attack_range=" + std::to_string(attackRange));
                    health();
                    if (!combatContinues()) return;
                    if (!found) {
                        setPoint(index, PointState::Clearing,
                            prefix + "没有可达目标，等待状态变化并继续扫描");
                        wait(options.scanMs); return;
                    }
                    // Recheck the chosen entity after planning before starting any action.
                    auto targetStillValid = [&](nav::PixelPoint current, bool casting) {
                        if (!circle.Contains(current) || !combatContinues()) return false;
                        for (const auto& monster : monsters())
                            if (monster.key == target.key && monster.entity == target.entity &&
                                ritualcombat::AliveInside(monster, circle, arenaMap)) {
                                return casting ? nav::PixelDistance(current, Position(monster)) <= attackRange + 4 :
                                    nav::PixelDistance(Position(monster), Position(target)) <= 6;
                            }
                        return false;
                    };
                    if (ritualcombat::InAttackPosition(arenaMap, position, Position(target), goal, attackRange)) {
                        setPoint(index, PointState::Clearing,
                            prefix + "接近怪物 " + std::to_string(target.key) + "，释放一轮技能");
                        oneRound([&]() { return targetStillValid(playerPosition(), true); });
                        return;
                    }
                    setPoint(index, PointState::Clearing,
                        prefix + "A* 接近怪物 " + std::to_string(target.key) + " " + Coordinates(Position(target)));
                    auto chase = navigation;
                    chase.presetCost = &arenaCost; chase.presetPixelMap = &arenaMap;
                    chase.goalX = goal.x; chase.goalY = goal.y; chase.arriveTolerance = 1;
                    chase.stepDistance = (std::min)(navigation.stepDistance, int32_t(30));
                    const auto chaseStarted = logicalNow(); auto checkedAt = chaseStarted - options.scanMs;
                    chase.control = [&](nav::PixelPoint current) {
                        if (pauseGate()) return usecase::NavControl{usecase::NavDirective::Hold, "用户暂停巡回"};
                        if (logicalNow() - chaseStarted >= 1500)
                            return usecase::NavControl{usecase::NavDirective::Arrived, "重新选择最近存活怪物"};
                        if (logicalNow() - checkedAt < options.scanMs) return usecase::NavControl{};
                        checkedAt = logicalNow();
                        if (!targetStillValid(current, false)) {
                            stationaryRecovery.timer.Reset();
                            return usecase::NavControl{usecase::NavDirective::Arrived, "祭祀状态或目标已变化，重新检查"};
                        }
                        return usecase::NavControl{};
                    };
                    usecase::NavTelemetry telemetry;
                    const int rc = usecase::Navigate(chase, &telemetry, &stop, reason); health();
                    if (rc && rc != 4) throw std::runtime_error("祭祀追怪失败：" + telemetry.note);
                    if (rc == 4) {
                        publish("追怪暂时受阻，保留当前祭祀并重新选路：" + telemetry.note);
                        wait(options.scanMs);
                    }
                };
                Observed observed;
                while (!point.activationConfirmed && point.state != PointState::Completed) {
                    waitPause(); observed = observe(index);
                    if (resolveActivation(observed)) break;
                    if (!point.activationSent && ritualcombat::Next(observed.object, observed.ritual,
                            observed.interactable.entity != 0) == ritualcombat::Phase::Clearing) {
                        preclearStep(); continue;
                    }
                    state.showCombatCircle = false;
                    const auto center = point.activationSent ? point.activationPosition : Position(point.object);
                    if (nav::PixelDistance(playerPosition(), center) > options.interactDistance) {
                        nav::PixelPoint approach;
                        if (!approachFor(center, playerPosition(), approach)) {
                            publish("当前祭祀附近暂不可达，保留目标并等待重新规划"); wait(options.scanMs); continue;
                        }
                        setPoint(index, PointState::Approaching, "角色偏离交互范围，返回当前祭祀点");
                        travelTo(approach, "返回祭祀交互范围"); continue;
                    }
                    if (point.activationSent) {
                        stopCombo();
                        setPoint(index, PointState::Activating, "等待祭祀进入进行中状态；发包回执不代表玩法开启");
                        const auto confirmStarted = logicalNow();
                        do {
                            waitPause(); observed = observe(index);
                            if (resolveActivation(observed)) break;
                            wait(options.scanMs);
                        } while (logicalNow() - confirmStarted < activationWaitMs);
                        if (point.activationConfirmed || point.state == PointState::Completed) break;
                        observed = observe(index);
                        if (resolveActivation(observed)) break;
                        point.activationSent = false;
                        publish("尚未确认祭祀开启，重新检查可交互状态和周围怪物");
                        continue;
                    }
                    if (!observed.CanStart()) {
                        stopCombo();
                        setPoint(index, PointState::Confirming, std::string("等待本点可开始：") + ritualstate::Text(observed.ritual));
                        wait(options.scanMs); continue;
                    }
                    if (nav::PixelDistance(playerPosition(), Position(observed.interactable)) > options.interactDistance) {
                        nav::PixelPoint approach;
                        if (!approachFor(Position(observed.interactable), playerPosition(), approach)) {
                            publish("实际祭祀交互对象附近暂不可达，等待后重试"); wait(options.scanMs); continue;
                        }
                        setPoint(index, PointState::Approaching, "继续靠近实际祭祀交互对象");
                        travelTo(approach, "祭祀交互接近"); continue;
                    }
                    stopCombo(); health();
                    setPoint(index, PointState::Interacting, "暂停寻路并松开技能，准备祭祀交互");
                    auto last = playerPosition(); uint32_t still = 0; const auto settleAt = logicalNow();
                    while (still < 3 && logicalNow() - settleAt <= 3000) {
                        wait(80); const auto current = playerPosition();
                        if (pause_.load()) break;
                        still = nav::PixelDistance(current, last) <= 1 ? still + 1 : 0; last = current;
                    }
                    if (pause_.load()) { waitPause(); continue; }
                    if (still < 3) { publish("角色仍在移动，保留祭祀目标，等待稳定后交互"); continue; }
                    observed = observe(index);
                    if (resolveActivation(observed)) break;
                    if (!observed.CanStart()) continue;
                    packet::Reply reply{}; bool submitted = false;
                    point.activationPosition = Position(observed.interactable);
                    struct RecordSubmission {
                        Point& point; bool& submitted;
                        ~RecordSubmission() { if (submitted) point.activationSent = true; }
                    } record{point, submitted};
                    const bool received = usecase::InteractRitual(observed.interactable, point.object, area,
                        connection.gamePid, connection.sessionId, connection.controllerId, true, reply, error, &stop, &submitted,
                        [&]() {
                            health(); const auto fresh = observe(index);
                            if (resolveActivation(fresh)) return false;
                            return !pause_.load() && !combatExpected && fresh.CanStart() &&
                                fresh.interactable.key == observed.interactable.key &&
                                fresh.interactable.entity == observed.interactable.entity &&
                                nav::PixelDistance(playerPosition(), Position(fresh.interactable)) <= options.interactDistance;
                        });
                    point.activationSent = submitted;
                    if (submitted) {
                        ++point.activationAttempts;
                    }
                    health();
                    publish("祭祀交互尝试 " + std::to_string(point.activationAttempts) + " 次；" +
                        (!received ? error : reply.result != packet::Result::Ok ? std::string(reply.detail) :
                         "请求已返回，等待祭祀状态确认"));
                    if (!submitted && !pause_.load()) wait(options.scanMs);
                }
                stopCombo();
                if (point.state == PointState::Completed) {
                    order = plan();
                    continue;
                }
                const auto battleStarted = logicalNow();
                const ritualcombat::Circle combatCircle{Position(point.object), options.arenaRadius};
                std::mt19937_64 roamRandom(GetTickCount64() ^ player ^ point.object.key);
                std::vector<nav::PixelPoint> recentGoals;
                auto battleExpired = [&]() {
                    if (logicalNow() - battleStarted >= uint64_t(options.battleTimeoutSec) * 1000)
                        throw std::runtime_error("祭祀战斗超时，未读取到 current_state=3；已保留当前节点，请查看诊断日志");
                };
                while (true) {
                    waitPause(); health(); observed = observe(index);
                    if (observed.object && observed.ritual.Completed()) break;
                    battleExpired();
                    if (!observed.object || !observed.ritual.Fighting()) {
                        stopCombo();
                        setPoint(index, PointState::Confirming, std::string("等待祭祀战斗状态：") + ritualstate::Text(observed.ritual));
                        wait(options.scanMs); continue;
                    }
                    ensureArena(combatCircle, false);
                    state.combatCircle = combatCircle; state.showCombatCircle = true;
                    const auto position = playerPosition();
                    const bool returning = !combatCircle.Contains(position);
                    nav::PixelPoint goal = point.approach;
                    startCombo();
                    if (!returning && !ritualcombat::ChooseRoamGoal(arenaMap, combatCircle, position,
                            roamRandom(), goal, recentGoals, cancelPlanning)) {
                        health(); stationaryRecovery.timer.Reset();
                        setPoint(index, PointState::Fighting, "圆内暂时没有可达游走坐标，保持技能循环并等待重试");
                        wait(options.scanMs); continue;
                    }
                    if (returning && (!combatCircle.Contains(goal) || !arenaMap.Passable(goal.x, goal.y))) {
                        double nearest = kInfinity;
                        for (size_t i = 0; i < arenaMap.PassableCount(); ++i) {
                            const auto candidate = arenaMap.Point(uint32_t(i));
                            const auto distance = nav::PixelDistance(position, candidate);
                            if (distance < nearest) { nearest = distance; goal = candidate; }
                        }
                    }
                    if (!returning) {
                        recentGoals.push_back(goal);
                        if (recentGoals.size() > 4) recentGoals.erase(recentGoals.begin());
                    }
                    setPoint(index, PointState::Fighting, returning ? "角色偏离祭祀圆，持续施法并返回圆内" :
                        "圆心 " + Coordinates(combatCircle.center) + "，半径 " + std::to_string(combatCircle.radius) +
                        "，持续施法并游走至 " + Coordinates(goal));
                    diagnostics->Write("ritual_roam", "player=" + Coordinates(position) +
                        " center=" + Coordinates(combatCircle.center) + " radius=" + std::to_string(combatCircle.radius) +
                        " goal=" + Coordinates(goal) + " returning=" + std::to_string(returning));
                    auto roaming = navigation;
                    if (!returning) { roaming.presetCost = &arenaCost; roaming.presetPixelMap = &arenaMap; }
                    roaming.goalX = goal.x; roaming.goalY = goal.y; roaming.arriveTolerance = 1;
                    roaming.stepDistance = (std::min)(navigation.stepDistance, int32_t(30));
                    auto checkedAt = logicalNow() - options.scanMs;
                    roaming.control = [&](nav::PixelPoint current) {
                        if (pauseGate()) return usecase::NavControl{usecase::NavDirective::Hold, "用户暂停巡回"};
                        battleExpired();
                        if (logicalNow() - checkedAt >= options.scanMs) {
                            checkedAt = logicalNow();
                            const auto latest = observe(index);
                            if (!latest.object || !latest.ritual.Fighting()) {
                                stopCombo();
                                return usecase::NavControl{usecase::NavDirective::Arrived, "祭祀状态变化，停止游走并重新检查"};
                            }
                        }
                        if (!returning && !combatCircle.Contains(current))
                            return usecase::NavControl{usecase::NavDirective::Arrived, "角色偏离祭祀圆，重新选择返回路线"};
                        startCombo(); // Also resumes after pause, a scan retry or recovery roll.
                        return usecase::NavControl{};
                    };
                    usecase::NavTelemetry telemetry;
                    const int rc = usecase::Navigate(roaming, &telemetry, &stop, reason); health();
                    if (rc && rc != 4) throw std::runtime_error("祭祀圆内游走失败：" + telemetry.note);
                    if (rc == 4) {
                        publish("圆内游走暂时受阻，保持当前祭祀并重新选择可达坐标：" + telemetry.note);
                        wait(options.scanMs);
                    }
                }
                stopCombo();
                setPoint(index, PointState::Completed, "current_state=3，本节点已完成");
                order = plan();
            }
            const auto unresolved = std::count_if(state.points.begin(), state.points.end(), [&](const Point& point) {
                return encounterroute::Mixed(point.kind, options.fightElites) && point.state != PointState::Completed;
            });
            if (unresolved) throw std::runtime_error("仍有 " + std::to_string(unresolved) + " 个祭祀／精英目标不可达或待处理，请查看表格");
            finalPhase = true;
            state.active = -1; state.showCombatCircle = false;
            publish("祭祀和已规划精英全部完成，进入终极 Boss 阶段");
            uint64_t waitingSince = logicalNow(), quietSince = 0;
            bool visitedEntrance = false;
            while (true) {
                stopCombo(); waitPause(); health(); discoverTargets();
                int boss = -1; size_t confirmed = 0;
                for (size_t i = 0; i < state.points.size(); ++i) if (state.points[i].kind == PointKind::MapBoss) {
                    if (state.points[i].state == PointState::Completed) ++confirmed;
                    else { boss = int(i); break; }
                }
                const int terminal = finalTarget();
                if (!visitedEntrance && terminal >= 0 && state.points[size_t(terminal)].kind == PointKind::BossEntrance) {
                    refreshMap(); auto& entrance = state.points[size_t(terminal)];
                    if (entrance.state == PointState::Unreachable) throw std::runtime_error("橙色 Boss 入口暂不可达，请检查地图门或障碍");
                    setPoint(size_t(terminal), PointState::Approaching, "先前往橙色记录点，加载终极 Boss");
                    travelTo(entrance.approach, "Boss 入口接近"); stopCombo();
                    setPoint(size_t(terminal), PointState::Completed, "已到达入口；仍需确认并击杀终极 Boss");
                    visitedEntrance = true; refreshMap(); waitingSince = logicalNow(); continue;
                }
                if (boss >= 0) {
                    if (!state.points[size_t(boss)].order) {
                        uint32_t last = 0;
                        for (const auto& p : state.points) last = (std::max)(last, p.order);
                        state.points[size_t(boss)].order = last + 1;
                    }
                    quietSince = 0; fightTarget(size_t(boss)); waitingSince = logicalNow(); continue;
                }
                if (confirmed) {
                    if (!quietSince) quietSince = logicalNow();
                    publish("终极 Boss 主体已确认 HP=0，继续观察是否出现后续阶段");
                    if (logicalNow() - quietSince >= 5000) break;
                    wait(options.scanMs); continue;
                }
                publish(visitedEntrance ? "已到橙色记录点，等待地图配置中的终极 Boss 加载" :
                    "终极 Boss 和橙色入口尚未加载，保留任务继续扫描");
                if (logicalNow() - waitingSince >= uint64_t(options.loadTimeoutSec) * 1000)
                    throw std::runtime_error("未找到可确认的终极 Boss；未判定通关，请接近 Boss 区域后重新规划");
                wait(options.scanMs);
            }
            state.active = -1; state.showCombatCircle = false;
            stopCombo(); waitPause(); health();
            publish("终极 Boss 已确认击杀，准备召唤藏身處传送门");
            auto readPortals = [&]() {
                std::vector<poe::EntitySnapshot> portals; poe::EntityScanInfo info;
                const auto started = logicalNow();
                while (true) {
                    waitPause(); health();
                    if (poe::SurroundingsTraverser(session.memory).ListPortals(session.moduleBase, portals, info, &stop)) {
                        health(); return portals;
                    }
                    if (logicalNow() - started >= uint64_t(options.loadTimeoutSec) * 1000)
                        throw std::runtime_error("传送门扫描持续失败：" + info.error);
                    publish("传送门扫描暂不可读，等待重扫：" + info.error); wait(options.scanMs);
                }
            };
            auto settle = [&]() {
                auto last = playerPosition(); uint32_t still = 0; const auto started = logicalNow();
                while (still < 3 && logicalNow() - started < 3000) {
                    waitPause(); wait(80); const auto current = playerPosition();
                    still = nav::PixelDistance(current, last) <= 1 ? still + 1 : 0; last = current;
                }
                if (still < 3) throw std::runtime_error("角色持续移动，传送门操作未继续");
            };
            while (state.returnHome.CanSummon()) {
                waitPause(); settle(); health();
                packet::Reply reply{};
                const bool received = usecase::SummonPortal(area, player, connection.gamePid, connection.sessionId,
                    connection.controllerId, reply, error, &stop, &state.returnHome.summonSubmitted, [&]() {
                        health();
                        if (pause_.load() || combatExpected || combo::MainRunner().Status().running) return false;
                        const auto portals = readPortals(); const auto origin = playerPosition();
                        state.returnHome.before.clear();
                        for (const auto& p : portals) state.returnHome.before.push_back(portalreturn::Id(p));
                        state.returnHome.originX = origin.x; state.returnHome.originY = origin.y;
                        return !pause_.load();
                    });
                health();
                if (!state.returnHome.summonSubmitted) {
                    if (pause_.load()) continue;
                    throw std::runtime_error("召唤传送门未提交：" + error);
                }
                publish("召唤已提交一次，等待附近新藏身處门；" +
                    (!received ? error : std::string(reply.detail)));
            }
            const auto portalStarted = logicalNow();
            auto findNewPortal = [&](poe::EntitySnapshot& out) {
                auto portals = readPortals(); const auto position = playerPosition();
                if (state.returnHome.target) portals.erase(std::remove_if(portals.begin(), portals.end(), [&](const auto& p) {
                    return !(portalreturn::Id(p) == state.returnHome.target);
                }), portals.end());
                const auto selected = portalreturn::SelectNew(portals, state.returnHome, position.x, position.y);
                if (!selected) return false;
                out = *selected; return true;
            };
            while (!state.returnHome.enterSubmitted) {
                waitPause(); health();
                if (logicalNow() - portalStarted >= uint64_t(options.loadTimeoutSec) * 1000)
                    throw std::runtime_error("未能确认并进入附近的新藏身處门；召唤未自动重发，请检查游戏");
                poe::EntitySnapshot portal;
                if (!findNewPortal(portal)) { wait(options.scanMs); continue; }
                state.returnHome.target = portalreturn::Id(portal);
                publish("已确认新藏身處门 " + portal.name + " key=" + std::to_string(portal.key) + " " + Coordinates(Position(portal)));
                if (nav::PixelDistance(playerPosition(), Position(portal)) > portalreturn::kEnterDistance) {
                    refreshMap(); nav::PixelPoint approach;
                    if (!approachFor(Position(portal), playerPosition(), approach) ||
                        nav::PixelDistance(approach, Position(portal)) > portalreturn::kEnterDistance - 2)
                        throw std::runtime_error("新藏身處门附近没有可达交互位置");
                    auto enter = navigation; enter.goalX = approach.x; enter.goalY = approach.y; enter.arriveTolerance = 1;
                    enter.control = [&](nav::PixelPoint) {
                        health();
                        if (pauseGate()) return usecase::NavControl{usecase::NavDirective::Hold, "用户暂停回城"};
                        return usecase::NavControl{};
                    };
                    publish("A* 接近新藏身處门");
                    usecase::NavTelemetry telemetry;
                    const int rc = usecase::Navigate(enter, &telemetry, &stop, reason); health();
                    if (rc) throw std::runtime_error("接近藏身處门失败：" + telemetry.note);
                }
                settle(); waitPause();
                if (!findNewPortal(portal)) throw std::runtime_error("已确认的新藏身處门消失，未发送进门");
                if (!state.returnHome.CanEnter()) throw std::runtime_error("本地图的进门提交状态不允许再次操作");
                packet::Reply reply{};
                const bool received = usecase::EnterHideoutPortal(portal, area, player, connection.gamePid,
                    connection.sessionId, connection.controllerId, reply, error, &stop, &state.returnHome.enterSubmitted, [&]() {
                        health(); poe::EntitySnapshot fresh;
                        return !pause_.load() && !combatExpected && !combo::MainRunner().Status().running &&
                            findNewPortal(fresh) && portalreturn::Id(fresh) == state.returnHome.target &&
                            nav::PixelDistance(playerPosition(), Position(fresh)) <= portalreturn::kEnterDistance;
                    });
                // A valid enter can change map before its pipe reply arrives.
                // From this point, use fresh destination reads rather than health().
                if (!state.returnHome.enterSubmitted) {
                    health();
                    if (pause_.load()) continue;
                    throw std::runtime_error("进门未提交：" + error);
                }
                publish("进门已提交一次，等待实际到达藏身處；" + (!received ? error : std::string(reply.detail)));
            }
            const packet::PortalScene source{area.world, area.mapOwner, area.record, player};
            portalreturn::Arrival arrival;
            const auto arrivalStarted = GetTickCount64();
            const auto arrivalTimeout = (std::max)(uint64_t(30000), uint64_t(options.loadTimeoutSec) * 1000);
            while (!state.returnHome.arrived) {
                if (stop.load()) throw std::runtime_error("回城观察已停止；进门未自动重发");
                if (!app::Config().TargetAlive() || !connection.Same(pipe::GetConnectionSnapshot()))
                    throw std::runtime_error("等待回城时游戏或通信会话变化，未确认到达藏身處");
                poe::WorldAreaSnapshot destination; packet::PortalScene scene;
                recovery::Vitals vitals;
                const auto read = [&](uint64_t a, void* p, size_t n) { return session.memory.ReadInto(a, p, n); };
                const bool valid = poe::ReadWorldArea(session.memory, session.moduleBase, destination, error) &&
                    portalreturn::ReadScene(session.moduleBase, read, scene) && scene.world == destination.world &&
                    scene.mapOwner == destination.mapOwner && scene.record == destination.record &&
                    recovery::ReadVitals(session.moduleBase, read, vitals) && vitals.entity == scene.player && vitals.world == scene.world;
                if (valid && !vitals.current[0]) throw std::runtime_error("回城期间角色死亡，未确认成功");
                if (valid && portalreturn::SameMap(source, scene) && scene.player != player)
                    throw std::runtime_error("回城期间角色身份变化，未确认成功");
                if (arrival.Observe(source, valid ? scene : packet::PortalScene{}, valid ? destination.name : "",
                                    valid && vitals.current[0], GetTickCount64())) {
                    state.returnHome.arrived = true;
                    publish("祭祀／所选精英与终极 Boss 已完成，已进入藏身處：" + destination.name);
                    break;
                }
                if (GetTickCount64() - arrivalStarted >= arrivalTimeout)
                    throw std::runtime_error("进门后未确认到达藏身處；未自动重发，请检查游戏切图结果");
                Sleep(100);
            }
            result = 0;
        }
    } catch (const std::exception& exception) {
        result = stop.load() ? 3 : 1;
        if (state.active >= 0 && size_t(state.active) < state.points.size() &&
            state.points[size_t(state.active)].state != PointState::Completed) {
            auto& point = state.points[size_t(state.active)]; point.state = PointState::Attention; point.detail = exception.what();
        }
        publish(exception.what());
    } catch (...) { result = 1; publish("祭祀任务出现未知异常，已停止"); }
    try { stopCombo(); }
    catch (const std::exception& exception) { result = 1; publish(state.text + "；" + exception.what()); }
    state.running = false; state.paused = false; state.showCombatCircle = false; pause_ = false; publish(state.text);
    diagnostics->Write("run_end", "result=" + std::to_string(result) + " active=" + std::to_string(state.active) + " text=" + state.text);
    return result;
}
} // namespace ritual
