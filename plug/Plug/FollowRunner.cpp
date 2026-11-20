#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "FollowRunner.h"
#include "MyOLAFunction.h"
#include "NamedPipeClie.h"
#include "Navigator.h"
#include "Surroundings.h"
#include "WorldArea.h"
#include "UseCases.h"
#include "../Common/Logger.h"
#include "../Common/PacketProtocol.h"
#include <chrono>
#include <cwchar>
#include <stdexcept>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <unordered_set>
#include <unordered_map>
#include <cstring>

namespace follow {
namespace {
class DiagnosticLog {
public:
    std::wstring path;
    std::mutex mutex;
    FILE* file = nullptr;
    DiagnosticLog() {
        auto base = ConfigPath();
        if (base.empty()) return;
        base.resize(base.find_last_of(L"\\/") + 1);
        base += L"follow_logs";
        if (!CreateDirectoryW(base.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
        SYSTEMTIME time{}; GetLocalTime(&time);
        wchar_t name[128]{};
        swprintf_s(name, L"\\follow_%04u%02u%02u_%02u%02u%02u_%lu.log", time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId());
        path = base + name;
        file = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
        if (!file) { path.clear(); return; }
        if (_ftelli64(file) == 0) fwrite("\xEF\xBB\xBF", 1, 3, file);
    }
    ~DiagnosticLog() { if (file) fclose(file); }
};
DiagnosticLog& Diagnostics() { static DiagnosticLog log; return log; }
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n && !s.empty()) throw std::runtime_error("名称不是有效的 UTF-8 文本");
    std::wstring out(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}
std::string Utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n && !s.empty()) throw std::runtime_error("名称不是有效的 Unicode 文本");
    std::string out(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
struct World {
    uint64_t map = 0, data = 0, player = 0;
    uint32_t width = 0, height = 0;
    bool Same(const World& b) const {
        return map == b.map && data == b.data && player == b.player && width == b.width && height == b.height;
    }
    bool SameScene(const World& b) const {
        return map == b.map && data == b.data && width == b.width && height == b.height;
    }
};
bool ReadWorld(const GameSession& session, World& out) {
    const auto& m = session.memory;
    auto entity = m.Follow(session.moduleBase + BaseOffset);
    for (int i = 0; entity && i < kLocalPlayerChainDepth; ++i) entity = m.Follow(entity + kMapObjectChain[i]);
    const auto map = mem::PointerChain(kMapObjectChain, kMapObjectChainDepth, mem::TailOp::Add)
        .Resolve(m, m.Follow(session.moduleBase + BaseOffset));
    uint32_t w = 0, h = 0;
    if (!entity || !map || !m.ReadInto(map + MAP_OFF_GRID_W, &w, sizeof(w)) ||
        !m.ReadInto(map + MAP_OFF_GRID_H, &h, sizeof(h)) || !w || !h ||
        w > kMaxGridDimension || h > kMaxGridDimension) return false;
    const auto data = m.Follow(map + MAP_OFF_ARY_BEGIN);
    if (!data) return false;
    out = {map, data, entity, w * CELL_SIZE, h * CELL_SIZE};
    return true;
}
void LogPlayer(const char* event, const poe::PlayerCandidate& player, uint64_t self) {
    LogDiagnostic("[%s] self=%d name=%s entity=0x%llX node=0x%llX Player=0x%llX Positioned=0x%llX xy_addr=0x%llX xy_ok=%d xy=(%d,%d) metadata=%s",
        event, player.entity == self, player.name.c_str(), player.entity, player.node,
        player.playerComponent, player.positioned, player.positioned + kPositioned_EntXOff,
        player.positionValid, player.x, player.y, player.metadataPath.c_str());
}
bool ListPlayers(const GameSession& session, uint64_t self, std::vector<poe::PlayerCandidate>& players,
                 const std::atomic_bool* stop, std::string& error) {
    poe::PlayerScanInfo info;
    const auto begin = GetTickCount64();
    const bool complete = poe::SurroundingsTraverser(session.memory).ListPlayers(session.moduleBase, players, info, stop);
    LogDiagnostic("[SCAN] complete=%d pid=%lu module=0x%llX self=0x%llX sentinel=0x%llX root=0x%llX visited=%zu players=%zu elapsed_ms=%llu error=%s",
        complete, session.game.pid, session.moduleBase, self, info.sentinel, info.root, info.visited,
        players.size(), GetTickCount64() - begin, info.error.c_str());
    for (const auto& player : players) LogPlayer("CANDIDATE", player, self);
    error = info.error; return complete;
}
// Owned by the navigation thread. Sampling the leader never issues actions.
class LootController {
public:
    LootController(const GameSession& session, const World& world, pipe::ConnectionSnapshot connection,
                   const std::atomic_bool& stop, const Options& options)
        : session_(session), world_(world), connection_(connection), stop_(stop), distance_(options.lootDistance) {
        std::vector<std::string> parsed; std::string error;
        if (!ParseLootNames(options.lootNames, parsed, error)) throw std::runtime_error(error);
        names_.insert(parsed.begin(), parsed.end());
        LogDiagnostic("[LOOT_OPTIONS] names=%s scan_ms=250 distance=%d interact_range=12 attempts=%u confirm_ms=%llu",
            JoinLootNames(parsed).c_str(), distance_, kMaxAttempts, kConfirmMs);
    }
    LootSnapshot Poll(nav::PixelPoint player) {
        if (!connection_.Same(pipe::GetConnectionSnapshot())) return Fail("拾取期间连接或游戏会话变化");
        if (stop_) { state_.waiting = true; return state_; }
        if (state_.terminal || GetTickCount64() < nextScan_) return state_;
        World before, after;
        if (!ReadWorld(session_, before)) return ScanFailure("地图读取暂时失败");
        if (!world_.Same(before)) return Fail("拾取期间地图或角色变化，请重新启动跟随");
        std::vector<poe::EntitySnapshot> items; poe::EntityScanInfo info;
        const bool complete = poe::SurroundingsTraverser(session_.memory)
            .ListWorldItems(session_.moduleBase, items, info, &stop_);
        if (!complete) return ScanFailure(info.error);
        if (!ReadWorld(session_, after)) return ScanFailure("扫描后地图校验暂时失败");
        if (!world_.Same(after)) return Fail("扫描期间地图或角色变化，请重新启动跟随");
        const auto now = GetTickCount64(); nextScan_ = now + 250;
        for (const auto& item : items) {
            if (!names_.count(item.name)) continue;
            if (item.x < 0 || item.y < 0 || uint32_t(item.x) >= world_.width || uint32_t(item.y) >= world_.height)
                return ScanFailure("名单物品坐标不在当前地图内");
        }
        scanFailureAt_ = 0;
        state_.waiting = false; state_.note.clear();
        if (state_.active) {
            // Confirm disappearance from the full scan, not the distance-filtered candidates.
            const auto found = std::find_if(items.begin(), items.end(), [&](const poe::EntitySnapshot& item) {
                return item.key == state_.key && item.entity == state_.entity && item.name == state_.name;
            });
            if (found == items.end()) {
                // Two complete scans, separated by 250ms, confirm disappearance.
                if (++missingScans_ < 2) {
                    state_.waiting = true; state_.note = "正在确认地面物品消失：" + state_.name;
                } else {
                    LogDiagnostic("[LOOT_RESOLVED] name=%s key=0x%X entity=0x%llX sends=%u", state_.name.c_str(), state_.key, state_.entity, attempts_);
                    ++state_.resolved; ReleaseTarget();
                }
            } else {
                missingScans_ = 0; state_.position = {found->x, found->y};
                if (nav::PixelDistance(player, state_.position) > distance_) {
                    LogDiagnostic("[LOOT_OUT_OF_RANGE] name=%s key=0x%X distance=%.1f limit=%d",
                        state_.name.c_str(), state_.key, nav::PixelDistance(player, state_.position), distance_);
                    ReleaseTarget();
                } else if (attempts_ >= kMaxAttempts && now - lastSentAt_ >= kConfirmMs) {
                    LogDiagnostic("[LOOT_SKIP] name=%s key=0x%X entity=0x%llX sends=%u reason=still_on_ground",
                        state_.name.c_str(), state_.key, state_.entity, attempts_);
                    history_[state_.key] = {state_.entity, state_.name, attempts_, lastSentAt_, true};
                    ++state_.skipped; ReleaseTarget();
                } else {
                    if (now - lockedAt_ >= 90000) return Fail("接近物品超时：" + state_.name);
                    state_.waiting = lastSentAt_ && now - lastSentAt_ < kConfirmMs;
                    if (state_.waiting) state_.note = "等待地面物品消失（" + std::to_string(attempts_) + "/2）：" + state_.name;
                }
            }
        }
        std::vector<const poe::EntitySnapshot*> matches;
        for (const auto& item : items) {
            if (!names_.count(item.name) || nav::PixelDistance(player, {item.x, item.y}) > distance_) continue;
            const auto history = history_.find(item.key);
            if (history != history_.end() && history->second.entity == item.entity &&
                history->second.name == item.name && history->second.skipped) continue;
            matches.push_back(&item);
        }
        state_.remaining = uint32_t(matches.size());
        if (state_.active || matches.empty()) return state_;
        const auto nearest = *std::min_element(matches.begin(), matches.end(), [&](const poe::EntitySnapshot* a, const poe::EntitySnapshot* b) {
            const double da = nav::PixelDistance(player, {a->x, a->y}), db = nav::PixelDistance(player, {b->x, b->y});
            return da < db || (da == db && a->key < b->key);
        });
        state_.active = true; state_.key = nearest->key; state_.entity = nearest->entity;
        state_.position = {nearest->x, nearest->y}; state_.name = nearest->name;
        lockedAt_ = now; lastSentAt_ = 0; attempts_ = missingScans_ = 0;
        const auto history = history_.find(state_.key);
        if (history != history_.end() && history->second.entity == state_.entity && history->second.name == state_.name) {
            attempts_ = history->second.attempts; lastSentAt_ = history->second.lastSentAt;
            state_.waiting = attempts_ >= kMaxAttempts || (lastSentAt_ && now - lastSentAt_ < kConfirmMs);
            if (state_.waiting) state_.note = "等待地面物品消失（" + std::to_string(attempts_) + "/2）：" + state_.name;
        }
        LogDiagnostic("[LOOT_LOCK] name=%s key=0x%X entity=0x%llX item=(%d,%d) self=(%d,%d) distance=%.1f remaining=%u",
            state_.name.c_str(), state_.key, state_.entity, state_.position.x, state_.position.y,
            player.x, player.y, nav::PixelDistance(player, state_.position), state_.remaining);
        return state_;
    }
    bool Pick(uint32_t key, uint64_t entity, std::string& error) {
        error.clear();
        if (stop_) return true;
        if (!state_.active || state_.key != key || state_.entity != entity || state_.waiting || attempts_ >= kMaxAttempts) return true;
        World current;
        if (!connection_.Same(pipe::GetConnectionSnapshot()) || !ReadWorld(session_, current) || !world_.Same(current)) {
            error = "发出拾取指令前地图、角色或连接校验失败"; return false;
        }
        pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
        if (!client.Initialize() || client.SessionId() != connection_.sessionId) { error = "拾取管道连接失败"; return false; }
        packet::Request request; request.operation = packet::Operation::Pick;
        request.itemKey = key; request.expectedEntity = entity;
        request.actionFlags = 0xFF000400;
        packet::Message message(request); std::vector<uint8_t> body;
        if (stop_) return true;
        if (!connection_.Same(pipe::GetConnectionSnapshot())) { error = "发包前连接已变化"; return false; }
        if (!client.Request(&message, MT_PacketReply, body, 8000) || body.size() != sizeof(packet::Reply)) {
            error = "拾取回执丢失，已停止且未自动重发，请检查游戏状态"; return false;
        }
        packet::Reply reply; std::memcpy(&reply, body.data(), sizeof(reply)); reply.detail[sizeof(reply.detail) - 1] = 0;
        LogDiagnostic("[LOOT_SEND] name=%s key=0x%X entity=0x%llX attempt=%u result=%u invoked=%u detail=%s",
            state_.name.c_str(), key, entity, attempts_ + 1, unsigned(reply.result), reply.invoked, reply.detail);
        if (!connection_.Same(pipe::GetConnectionSnapshot()) || reply.version != packet::kVersion) {
            error = "拾取回执会话或版本不匹配"; return false;
        }
        if (reply.result != packet::Result::Ok && reply.result != packet::Result::TargetMissing) {
            error = "拾取失败：" + state_.name + "；" + reply.detail; return false;
        }
        ++attempts_; lastSentAt_ = GetTickCount64();
        history_[state_.key] = {state_.entity, state_.name, attempts_, lastSentAt_, false};
        state_.waiting = true; nextScan_ = 0;
        return true;
    }
private:
    static constexpr uint32_t kMaxAttempts = 2;
    static constexpr uint64_t kConfirmMs = 1500;
    void ReleaseTarget() {
        state_.active = state_.waiting = false; state_.key = 0; state_.entity = 0;
        state_.position = {}; state_.name.clear(); state_.note.clear();
        lockedAt_ = lastSentAt_ = 0; attempts_ = missingScans_ = 0;
    }
    LootSnapshot Fail(const std::string& error) {
        state_.terminal = true; state_.waiting = true; state_.note = error;
        LogDiagnostic("[LOOT_FAIL] %s", error.c_str()); return state_;
    }
    LootSnapshot ScanFailure(const std::string& error) {
        const auto now = GetTickCount64(); nextScan_ = now + 250;
        if (!scanFailureAt_) { scanFailureAt_ = now; LogDiagnostic("[LOOT_SCAN_WAIT] %s", error.c_str()); }
        if (now - scanFailureAt_ >= 5000) return Fail("掉落物扫描持续失败：" + error);
        state_.waiting = true; state_.note = "暂停移动，等待完整掉落物扫描：" + error; return state_;
    }
    const GameSession& session_;
    World world_;
    pipe::ConnectionSnapshot connection_;
    const std::atomic_bool& stop_;
    int32_t distance_;
    std::unordered_set<std::string> names_;
    struct PickHistory {
        uint64_t entity;
        std::string name;
        uint32_t attempts;
        uint64_t lastSentAt;
        bool skipped;
    };
    std::unordered_map<uint32_t, PickHistory> history_;
    LootSnapshot state_;
    uint64_t nextScan_ = 0, scanFailureAt_ = 0, lockedAt_ = 0, lastSentAt_ = 0;
    uint32_t attempts_ = 0, missingScans_ = 0;
};

// Lives for the entire follow run, including scene reloads. Only the navigation
// thread calls this controller, so transfer/pick/movement requests are serialized.
class TravelController {
public:
    TravelController(const GameSession& session, pipe::ConnectionSnapshot connection,
                     const Options& options, const std::atomic_bool& stop,
                     std::function<Snapshot()> sample,
                     std::function<void(const std::string&, const std::string&, uint32_t)> publish)
        : session_(session), connection_(connection), options_(options), stop_(stop),
          sample_(std::move(sample)), publish_(std::move(publish)) {}

    bool WaitingForScene(const World& world) {
        if (!pendingUntil_) return false;
        const auto now = GetTickCount64();
        if (now < settleUntil_ || (pendingWorld_.Same(world) && now < pendingUntil_)) return true;
        if (!pendingWorld_.SameScene(world)) {
            poe::WorldAreaSnapshot area; std::string error;
            if (pendingParty_ && poe::ReadWorldArea(session_.memory, session_.moduleBase, area, error) && Hideout(area.name)) {
                homeWorld_ = world; haveHome_ = true;
            }
        }
        LogDiagnostic("[TRAVEL_SETTLED] changed=%d attempts=%u", !pendingWorld_.Same(world), attempts_);
        pendingUntil_ = settleUntil_ = 0; nextRead_ = 0; cached_ = {};
        return false;
    }
    void BeginScene(const World& world) { world_ = world; nextRead_ = 0; cached_ = {}; }

    TravelSnapshot Poll(const Snapshot& sample, bool allowAction) {
        if (stop_) return Hold("正在停止跟随");
        if (!connection_.Same(pipe::GetConnectionSnapshot())) return Failed("连接或游戏会话已变化，请重新启动跟随");
        World current;
        if (!ReadWorld(session_, current)) return Hold("暂停移动，等待地图加载");
        if (!world_.Same(current)) return Reload("场景已变化，正在重新取图");
        if (sample.sceneChanged) return Reload("正在重新定位新场景");
        const auto now = GetTickCount64();
        if (Fresh(sample, now, options_.StaleMs())) {
            // A nearby entity proves co-presence. Local scene changes alone do not.
            if (haveSeenLeader_ && !seenLeaderWorld_.SameScene(world_)) Reset("在新场景重新看到队长");
            seenLeaderWorld_ = world_; haveSeenLeader_ = true;
            if (now >= nextVisibleArea_) {
                poe::WorldAreaSnapshot area; std::string error;
                if (poe::ReadWorldArea(session_.memory, session_.moduleBase, area, error)) {
                    SetLeaderArea(area.id, area.name); selfMap_ = area.name;
                    if (Hideout(area.name)) { homeWorld_ = world_; haveHome_ = true; }
                    Publish();
                }
                nextVisibleArea_ = now + 2000;
            }
            cached_ = {}; nextRead_ = 0;
            return {};
        }
        if (!allowAction) return {};
        if (!sample.absent || !sample.searchedAt || now < sample.searchedAt ||
            now - sample.searchedAt > options_.searchMs + options_.StaleMs())
            return Hold("等待完整的周围玩家扫描，暂停跨图操作");
        if (now < nextRead_) return cached_;
        nextRead_ = now + 1500;
        poe::WorldAreaSnapshot area;
        poe::PartyMember leader;
        std::string error;
        if (!ReadLeader(area, leader, error)) return Cache(Hold("等待有效队伍和地图信息：" + error));
        selfMap_ = area.name;
        SetLeaderArea(leader.mapId, leader.mapName); Publish();
        const bool sameType = area.id == leader.mapId;
        if (options_.mode == Mode::Story) {
            if (sameType) return Cache(Hold("地图类型一致，原地等待队长来接"));
            return Party(leader);
        }
        const bool selfHome = Hideout(area.name), leaderHome = Hideout(leader.mapName);
        const bool seenHere = haveSeenLeader_ && seenLeaderWorld_.SameScene(world_);
        if (sameType && (seenHere || (!selfHome && !leaderHome)))
            return Cache(Hold(seenHere ? "已确认同场景，原地等待队长来接" : "地图类型一致，原地等待队长来接（实例未确认）"));
        if (leaderHome) {
            if (sameType && haveHome_ && homeWorld_.SameScene(world_))
                return Cache(Hold("已在队长藏身处，等待队长来接"));
            return Party(leader);
        }
        if (!selfHome) return Party(leader); // Different non-hideout maps: first go to the leader's home.

        std::vector<poe::EntitySnapshot> portals; poe::EntityScanInfo info;
        if (!poe::SurroundingsTraverser(session_.memory).ListPortals(session_.moduleBase, portals, info, &stop_))
            return Cache(Hold("传送门扫描暂未完成：" + info.error));
        World after;
        if (!ReadWorld(session_, after) || !world_.Same(after)) return Reload("扫描传送门期间场景变化");
        for (const auto& portal : portals) if (portal.name == leader.mapName) {
            if (portal.x < 0 || portal.y < 0 || uint32_t(portal.x) >= world_.width || uint32_t(portal.y) >= world_.height)
                return Cache(Hold("匹配传送门坐标无效，等待重新扫描"));
            TravelSnapshot value; value.action = TravelAction::Portal;
            value.key = portal.key; value.entity = portal.entity; value.position = {portal.x, portal.y};
            value.note = "前往传送门：" + portal.name;
            portalMapId_ = leader.mapId; portalMapName_ = leader.mapName;
            return Cache(std::move(value)); // Any one of multiple matching portals is valid.
        }
        if (haveHome_ && homeWorld_.SameScene(world_))
            return Cache(Hold("已到队长藏身处，等待目的地匹配的传送门：" + leader.mapName));
        return Party(leader);
    }

    TravelSnapshot Enter(uint32_t key, uint64_t entity) {
        poe::WorldAreaSnapshot area; poe::PartyMember leader; std::string error;
        if (stop_) return Hold("正在停止跟随");
        if (!ReadLeader(area, leader, error)) return Hold("进门前重新核对队伍：" + error);
        if (leader.mapId != portalMapId_ || leader.mapName != portalMapName_ || Hideout(leader.mapName)) {
            nextRead_ = 0; return Hold("队长地图已变化，正在重新选择传送方式");
        }
        std::vector<poe::EntitySnapshot> portals; poe::EntityScanInfo info;
        if (!poe::SurroundingsTraverser(session_.memory).ListPortals(session_.moduleBase, portals, info, &stop_))
            return Hold("进门前传送门扫描失败：" + info.error);
        const auto found = std::find_if(portals.begin(), portals.end(), [&](const poe::EntitySnapshot& p) {
            return p.key == key && p.entity == entity && p.name == leader.mapName;
        });
        World current;
        if (found == portals.end()) { nextRead_ = 0; return Hold("传送门已变化，正在重新扫描"); }
        if (!ReadWorld(session_, current) || !world_.Same(current)) return Reload("进门前场景已变化");
        if (!connection_.Same(pipe::GetConnectionSnapshot())) return Failed("进门前通信会话已变化");
        pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
        if (!client.Initialize() || client.SessionId() != connection_.sessionId || client.ClientId() != connection_.controllerId)
            return Failed("进门管道会话校验失败");
        if (stop_) return Hold("正在停止跟随");
        if (!AbsentNow()) return Hold("队长或当前场景状态变化，正在重新判断");
        packet::Request request; request.operation = packet::Operation::EnterPortal;
        request.itemKey = key; request.expectedEntity = entity;
        packet::Message message(request); std::vector<uint8_t> body;
        BeginPending(false);
        const bool received = client.Request(&message, MT_PacketReply, body, 8000);
        packet::Reply reply;
        if (received && body.size() == sizeof(reply)) {
            std::memcpy(&reply, body.data(), sizeof(reply));
            reply.detail[sizeof(reply.detail) - 1] = 0;
            LogDiagnostic("[FOLLOW_PORTAL] key=0x%X entity=0x%llX map=%s result=%u invoked=%u detail=%s",
                key, entity, portalMapId_.c_str(), unsigned(reply.result), reply.invoked, reply.detail);
            if (reply.version != packet::kVersion) return Failed("传送门回执版本不匹配");
        }
        return Reload("进门请求已发起，等待实际切图");
    }
private:
    static bool Hideout(const std::string& name) {
        return name.find("藏身處") != std::string::npos || name.find("藏身处") != std::string::npos;
    }
    static TravelSnapshot Hold(const std::string& text) { TravelSnapshot s; s.action = TravelAction::Wait; s.note = text; return s; }
    static TravelSnapshot Reload(const std::string& text) { TravelSnapshot s; s.action = TravelAction::ReloadScene; s.note = text; return s; }
    static TravelSnapshot Failed(const std::string& text) { TravelSnapshot s; s.action = TravelAction::Failed; s.note = text; return s; }
    TravelSnapshot Cache(TravelSnapshot value) {
        nextRead_ = GetTickCount64() + 1500; cached_ = std::move(value); return cached_;
    }
    void Publish() { publish_(selfMap_, leaderMap_, attempts_); }
    void Reset(const char* reason) {
        LogDiagnostic("[TRAVEL_RESET] old_attempts=%u reason=%s", attempts_, reason);
        attempts_ = 0; Publish();
    }
    void SetLeaderArea(const std::string& id, const std::string& name) {
        if (!leaderMapId_.empty() && leaderMapId_ != id) Reset("队长地图类型已变化，确认进入另一场景");
        leaderMapId_ = id; leaderMap_ = name;
    }
    bool ReadLeader(poe::WorldAreaSnapshot& area, poe::PartyMember& leader, std::string& error) {
        World before, after; poe::PartySnapshot party;
        if (!connection_.Same(pipe::GetConnectionSnapshot()) || !ReadWorld(session_, before) || !world_.Same(before)) {
            error = "场景或通信会话变化"; return false;
        }
        if (!poe::ReadWorldArea(session_.memory, session_.moduleBase, area, error) ||
            !poe::ReadParty(session_.memory, session_.moduleBase, party, error, &stop_)) return false;
        size_t count = 0;
        for (const auto& member : party.members) if (!member.self && member.name == options_.leaderName) { leader = member; ++count; }
        if (count != 1 || leader.mapStatus != poe::PartyMapStatus::Resolved || leader.mapId.empty() || leader.mapName.empty()) {
            error = count != 1 ? "队伍中未找到唯一的队长" : "队长地图尚未就绪"; return false;
        }
        if (!leaderAccount_.empty() && leaderAccount_ != leader.account) { error = "队长账号发生变化，请重启跟随"; return false; }
        leaderAccount_ = leader.account;
        if (!ReadWorld(session_, after) || !world_.Same(after) || !connection_.Same(pipe::GetConnectionSnapshot())) {
            error = "读取期间场景或会话变化"; return false;
        }
        return true;
    }
    void BeginPending(bool party) {
        pendingWorld_ = world_; pendingParty_ = party;
        const auto now = GetTickCount64(); settleUntil_ = now + 2500; pendingUntil_ = now + 15000;
    }
    bool AbsentNow() const {
        const auto latest = sample_(); const auto now = GetTickCount64(); World current;
        return !stop_ && connection_.Same(pipe::GetConnectionSnapshot()) &&
            ReadWorld(session_, current) && world_.Same(current) &&
            !latest.valid && !latest.terminal && !latest.sceneChanged && latest.absent && latest.searchedAt &&
            now >= latest.searchedAt && now - latest.searchedAt <= options_.searchMs + options_.StaleMs();
    }
    TravelSnapshot Party(const poe::PartyMember& leader) {
        if (attempts_ >= 5) return Cache(Hold("队友传送已达 5 次，等待队长换图、出现匹配门或手动重启跟随"));
        packet::Reply reply; std::string error; bool submitted = false;
        const bool received = usecase::TeleportToPartyMember(leader, connection_.gamePid, connection_.sessionId,
            connection_.controllerId, reply, error, &stop_, &submitted, true, [&] { return AbsentNow(); });
        if (!submitted) return Cache(Hold("队友传送未发起：" + error));
        ++attempts_; Publish(); BeginPending(true);
        LogDiagnostic("[FOLLOW_PARTY_TELEPORT] mode=%s name=%s map=%s attempt=%u received=%d result=%u invoked=%u error=%s",
            ModeText(options_.mode), leader.name.c_str(), leader.mapId.c_str(), attempts_, received,
            unsigned(reply.result), reply.invoked, error.c_str());
        return Reload("队友传送已发起（" + std::to_string(attempts_) + "/5），等待实际切图");
    }
    const GameSession& session_;
    pipe::ConnectionSnapshot connection_;
    const Options& options_;
    const std::atomic_bool& stop_;
    std::function<Snapshot()> sample_;
    std::function<void(const std::string&, const std::string&, uint32_t)> publish_;
    World world_, pendingWorld_, seenLeaderWorld_, homeWorld_;
    bool pendingParty_ = false, haveSeenLeader_ = false, haveHome_ = false;
    uint64_t pendingUntil_ = 0, settleUntil_ = 0, nextRead_ = 0, nextVisibleArea_ = 0;
    uint32_t attempts_ = 0;
    std::string selfMap_, leaderMap_, leaderMapId_, leaderAccount_, portalMapId_, portalMapName_;
    TravelSnapshot cached_;
};
} // namespace

std::wstring DiagnosticLogPath() { return Diagnostics().path; }
void LogDiagnostic(const char* format, ...) {
    char text[4096]{};
    va_list args; va_start(args, format); _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args); va_end(args);
    auto& log = Diagnostics(); std::lock_guard<std::mutex> lock(log.mutex);
    if (!log.file) { LOGW("[FollowDiag] %s", text); return; }
    SYSTEMTIME time{}; GetLocalTime(&time);
    fprintf(log.file, "[%02u:%02u:%02u.%03u][tid=%lu] %s\n", time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, GetCurrentThreadId(), text);
    fflush(log.file);
}

std::wstring ConfigPath() {
    return app::Config().RuntimeFileW(L"follow.ini");
}
bool LoadConfig(Options& options, std::string& error) {
    error.clear();
    try {
        const auto path = ConfigPath();
        if (path.empty()) throw std::runtime_error("无法获取跟随配置路径");
        Options loaded;
        wchar_t text[512]{};
        GetPrivateProfileStringW(L"follow", L"leaderName", L"", text, 512, path.c_str());
        loaded.leaderName = Utf8(text);
        wchar_t lootText[4098]{};
        const auto lootLength = GetPrivateProfileStringW(L"follow", L"lootNames", L"", lootText, 4098, path.c_str());
        if (lootLength >= 4097) throw std::runtime_error("拾取名单配置过长");
        loaded.lootNames = Utf8(lootText);
        auto number = [&](const wchar_t* key, uint32_t fallback) {
            const auto def = std::to_wstring(fallback);
            GetPrivateProfileStringW(L"follow", key, def.c_str(), text, 512, path.c_str());
            const std::wstring value(text);
            if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                throw std::runtime_error("跟随配置含非法数值");
            const auto n = std::stoull(value);
            if (n > 1000000) throw std::runtime_error("跟随配置数值超出范围");
            return uint32_t(n);
        };
        loaded.distance = int32_t(number(L"distance", 60));
        loaded.lootDistance = int32_t(number(L"lootDistance", 200));
        loaded.sampleMs = number(L"sampleMs", 100);
        loaded.replanMs = number(L"replanMs", 250);
        loaded.searchMs = number(L"searchMs", 500);
        loaded.moveThreshold = int32_t(number(L"moveThreshold", 15));
        loaded.mode = Mode(number(L"mode", uint32_t(Mode::Story)));
        auto checked = loaded; if (checked.leaderName.empty()) checked.leaderName = "pending";
        if (!Validate(checked, error)) return false;
        options = std::move(loaded); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool SaveConfig(const Options& o, std::string& error) {
    if (!Validate(o, error)) return false;
    std::wstring temp;
    try {
        const auto path = ConfigPath();
        if (path.empty()) throw std::runtime_error("无法获取跟随配置路径");
        const std::wstring text = L"\xFEFF; POE2 follow settings\r\n[follow]\r\nleaderName=" + Wide(o.leaderName) +
            L"\r\ndistance=" + std::to_wstring(o.distance) + L"\r\nsampleMs=" + std::to_wstring(o.sampleMs) +
            L"\r\nreplanMs=" + std::to_wstring(o.replanMs) + L"\r\nsearchMs=" + std::to_wstring(o.searchMs) +
            L"\r\nmoveThreshold=" + std::to_wstring(o.moveThreshold) + L"\r\nlootNames=" + Wide(o.lootNames) +
            L"\r\nlootDistance=" + std::to_wstring(o.lootDistance) +
            L"\r\nmode=" + std::to_wstring(uint32_t(o.mode)) + L"\r\n";
        temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入跟随配置");
        DWORD written = 0; const auto bytes = DWORD(text.size() * sizeof(wchar_t));
        const bool ok = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("跟随配置保存失败");
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
        return true;
    } catch (const std::exception& e) {
        if (!temp.empty()) DeleteFileW(temp.c_str()); error = e.what(); return false;
    }
}

Runner& MainRunner() { (void)Diagnostics(); static Runner runner; return runner; }
Runner::~Runner() { Stop(); StopPlayerScan(); Join(); JoinPlayerScan(); }
bool Runner::Start(const Options& options, std::string& error) {
    if (!Validate(options, error)) return false;
    if (running_.load()) { error = "跟随已经运行"; return false; }
    if (scanning_.load()) { error = "正在刷新周围玩家，请稍候"; return false; }
    if (!pipe::GetConnectionSnapshot().ready) { error = "通信未就绪，请先在寻路页连接 Monitor"; return false; }
    Join(); stop_ = false; samplingStop_ = false; running_ = true;
    { std::lock_guard<std::mutex> lock(mutex_); status_ = {}; status_.running = true; status_.text = "正在获取当前地图…"; }
    try { thread_ = std::thread(&Runner::Run, this, options); }
    catch (const std::exception& e) { running_ = false; error = e.what(); return false; }
    return true;
}
void Runner::Stop() { if (running_) LogDiagnostic("[STOP_REQUEST] user requested stop"); stop_ = true; wake_.notify_all(); }
void Runner::Join() { if (thread_.joinable()) thread_.join(); }
Status Runner::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto value = status_; value.running = running_.load(); return value;
}
bool Runner::RefreshPlayers(std::string& error) {
    error.clear();
    if (Running()) { error = "请先停止跟随后再刷新玩家列表"; return false; }
    if (scanning_.exchange(true)) { error = "正在刷新周围玩家"; return false; }
    JoinPlayerScan(); scanStop_ = false;
    { std::lock_guard<std::mutex> lock(mutex_);
      players_.players.clear(); players_.selfEntity = players_.selfPositioned = 0;
      ++players_.revision; players_.text = "正在遍历周围玩家…"; }
    try { scanThread_ = std::thread(&Runner::ScanPlayers, this); }
    catch (const std::exception& e) { scanning_ = false; error = e.what(); return false; }
    return true;
}
PlayerListStatus Runner::GetPlayers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = players_; result.refreshing = scanning_.load(); return result;
}
void Runner::StopPlayerScan() { scanStop_ = true; }
void Runner::JoinPlayerScan() { if (scanThread_.joinable()) scanThread_.join(); }
void Runner::ScanPlayers() {
    PlayerListStatus result;
    try {
        LogDiagnostic("[LIST_BEGIN] refreshing nearby player dropdown");
        GameSession session; std::string error;
        if (!InitializeSession(session, &error)) throw std::runtime_error(error);
        World before, after; poe::PlayerCandidate self;
        if (!ReadWorld(session, before) || !poe::ReadPlayerCandidate(session.memory, before.player, self))
            throw std::runtime_error("本地玩家身份读取失败，请进入场景后刷新");
        LogPlayer("SELF", self, before.player);
        std::vector<poe::PlayerCandidate> players;
        if (!ListPlayers(session, before.player, players, &scanStop_, error)) throw std::runtime_error(error);
        if (scanStop_ || !ReadWorld(session, after) || !before.Same(after)) throw std::runtime_error("扫描已取消或地图已变化，请重新刷新");
        std::unordered_set<uint64_t> entities;
        for (auto& player : players) {
            if (player.entity == before.player || player.positioned == self.positioned ||
                player.playerComponent == self.playerComponent || player.name == self.name) {
                LogPlayer("LIST_EXCLUDE_SELF", player, before.player); continue;
            }
            if (entities.insert(player.entity).second) result.players.push_back(std::move(player));
        }
        result.selfEntity = before.player; result.selfPositioned = self.positioned;
        result.text = "周围其他玩家 " + std::to_string(result.players.size()) + " 个（已排除自身），可下拉选择或手动输入";
        LogDiagnostic("[LIST_DONE] self_name=%s self=0x%llX choices=%zu", self.name.c_str(), before.player, result.players.size());
    } catch (const std::exception& e) { result.text = e.what(); LogDiagnostic("[LIST_ERROR] %s", e.what()); }
    { std::lock_guard<std::mutex> lock(mutex_); result.revision = players_.revision + 1; players_ = std::move(result); }
    scanning_ = false;
}
void Runner::Run(Options options) {
    std::string endText = "已停止跟随";
    State endState = State::Stopped;
    try {
        const auto connection = pipe::GetConnectionSnapshot();
        GameSession session; std::string error;
        if (!InitializeSession(session, &error)) throw std::runtime_error(error);
        if (session.game.pid != connection.gamePid) throw std::runtime_error("游戏 PID 与通信端不一致");
        TravelController travel(session, connection, options, stop_,
            [&] { std::lock_guard<std::mutex> lock(mutex_); return status_.target; },
            [&](const std::string& own, const std::string& leader, uint32_t attempts) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.selfMap = own; status_.leaderMap = leader; status_.teleportAttempts = attempts;
            });
        std::string expectedSelf;
        while (!stop_) {
            World baseline;
            poe::PlayerCandidate self;
            World candidate; uint64_t stableAt = 0;
            { std::lock_guard<std::mutex> lock(mutex_);
              status_.target = {}; status_.selfPositionValid = false;
              status_.navigation.state = State::Travelling; status_.text = "等待场景加载和角色定位…"; }
            while (!stop_) {
                if (!connection.Same(pipe::GetConnectionSnapshot())) throw std::runtime_error("连接或游戏会话变化，请重新启动跟随");
                World current;
                if (ReadWorld(session, current) && poe::ReadPlayerCandidate(session.memory, current.player, self) && self.positionValid) {
                    if (!expectedSelf.empty() && self.name != expectedSelf) throw std::runtime_error("本地角色身份变化，请重新启动跟随");
                    if (!candidate.Same(current)) { candidate = current; stableAt = GetTickCount64(); }
                    if (GetTickCount64() - stableAt >= 750 && !travel.WaitingForScene(current)) { baseline = current; break; }
                } else stableAt = GetTickCount64();
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(150), [&] { return stop_.load(); });
            }
            if (stop_) break;
            expectedSelf = self.name;
            travel.BeginScene(baseline);
            LogDiagnostic("[SCENE_START] mode=%s requested=%s pid=%lu module=0x%llX session=%llu map=0x%llX map_data=0x%llX size=%ux%u distance=%d sample_ms=%u replan_ms=%u",
                ModeText(options.mode),
                options.leaderName.c_str(), session.game.pid, session.moduleBase, connection.sessionId, baseline.map,
                baseline.data, baseline.width, baseline.height, options.distance, options.sampleMs, options.replanMs);
            LogPlayer("SELF", self, baseline.player);
            if (self.name == options.leaderName) throw std::runtime_error("填写的是自身角色名称，请选择其他玩家");
            { std::lock_guard<std::mutex> lock(mutex_);
              status_.selfEntity = baseline.player; status_.selfPositioned = self.positioned; status_.selfName = self.name; }
            std::thread sampler;
            samplingStop_ = false;
            struct SamplingGuard {
                std::atomic_bool& stop; std::condition_variable& wake; std::thread& thread;
                ~SamplingGuard() { stop = true; wake.notify_all(); if (thread.joinable()) thread.join(); }
            } samplingGuard{samplingStop_, wake_, sampler};
            sampler = std::thread([&, baseline] {
                uint64_t leader = 0, positioned = 0, playerComponent = 0, nextSearch = 0, identityAt = 0, sequence = 0, lastSampleAt = 0;
                uint64_t lastLogAt = 0;
                std::string lastNote;
                bool seen = false;
                bool absent = false;
                uint64_t searchedAt = 0;
                auto publish = [&](Snapshot value) {
                    value.sequence = ++sequence;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (value.valid) {
                        status_.sampleGapMs = lastSampleAt ? value.sampledAt - lastSampleAt : 0;
                        lastSampleAt = value.sampledAt; ++status_.samples;
                    }
                    status_.target = std::move(value);
                };
                try {
                    while (!stop_ && !samplingStop_) {
                        const auto tick = GetTickCount64();
                        Snapshot value; value.mapToken = baseline.map;
                        value.width = baseline.width; value.height = baseline.height;
                        World current;
                        if (!connection.Same(pipe::GetConnectionSnapshot())) {
                            LogDiagnostic("[SESSION_CHANGED] connection no longer matches baseline");
                            value.terminal = true; value.note = "连接或游戏会话变化，请重新启动跟随"; publish(value); break;
                        }
                        if (!ReadWorld(session, current)) {
                            value.note = "地图读取暂时失败，暂停移动"; leader = positioned = 0;
                            absent = false;
                        } else if (!baseline.Same(current)) {
                            LogDiagnostic("[WORLD_CHANGED] old_self=0x%llX new_self=0x%llX old_map=0x%llX new_map=0x%llX",
                                baseline.player, current.player, baseline.map, current.map);
                            value.sceneChanged = true; value.note = "地图已变化，等待新场景并恢复跟随"; publish(value); break;
                        } else {
                            if (leader && tick >= identityAt) {
                                poe::PlayerCandidate checkedSelf;
                                if (!poe::ReadPlayerCandidate(session.memory, baseline.player, checkedSelf) || checkedSelf.name != self.name) {
                                    LogPlayer("SELF_IDENTITY_REJECT", checkedSelf, baseline.player);
                                    value.terminal = true; value.note = "自身身份校验失败，请重新启动跟随"; publish(value); break;
                                }
                                if (checkedSelf.positioned != self.positioned || checkedSelf.playerComponent != self.playerComponent)
                                    LogPlayer("SELF_COMPONENT_CHANGED", checkedSelf, baseline.player);
                                self = std::move(checkedSelf);
                                { std::lock_guard<std::mutex> lock(mutex_); status_.selfPositioned = self.positioned; }
                                poe::PlayerCandidate checked;
                                const bool ok = poe::ReadPlayerCandidate(session.memory, leader, checked);
                                LogPlayer("IDENTITY", checked, baseline.player);
                                const bool same = ok && checked.name == options.leaderName &&
                                    checked.entity != baseline.player && checked.positioned != self.positioned &&
                                    checked.playerComponent != self.playerComponent;
                                positioned = same ? checked.positioned : 0;
                                playerComponent = same ? checked.playerComponent : 0;
                                { std::lock_guard<std::mutex> lock(mutex_); status_.leaderPositioned = positioned; }
                                identityAt = tick + 1000;
                                if (!positioned) { LogDiagnostic("[IDENTITY_REJECT] requested=%s actual=%s entity=0x%llX", options.leaderName.c_str(), checked.name.c_str(), leader); leader = 0; }
                            }
                            if (tick >= nextSearch) {
                                std::vector<poe::PlayerCandidate> players;
                                std::string scanError;
                                const bool complete = ListPlayers(session, baseline.player, players, &stop_, scanError);
                                poe::PlayerCandidate match; uint32_t matches = 0;
                                std::unordered_set<uint64_t> entities;
                                for (const auto& player : players) {
                                    if (player.name != options.leaderName || !entities.insert(player.entity).second) continue;
                                    if (player.entity == baseline.player || player.positioned == self.positioned ||
                                        player.playerComponent == self.playerComponent || player.name == self.name) {
                                        LogPlayer("REJECT_SELF_ALIAS", player, baseline.player); continue;
                                    }
                                    ++matches; match = player;
                                }
                                LogDiagnostic("[MATCH] requested=%s complete=%d matches=%u error=%s", options.leaderName.c_str(), complete, matches, scanError.c_str());
                                nextSearch = GetTickCount64() + options.searchMs;
                                searchedAt = GetTickCount64(); absent = complete && matches == 0;
                                if (complete && matches == 1) {
                                    const bool changed = leader != match.entity;
                                    leader = match.entity; positioned = match.positioned; playerComponent = match.playerComponent;
                                    if (changed) identityAt = GetTickCount64() + 1000;
                                    seen = true;
                                    LogPlayer("LOCK", match, baseline.player);
                                    { std::lock_guard<std::mutex> lock(mutex_); status_.leaderName = match.name; status_.leaderPositioned = positioned; }
                                    LOGI("[Follow] locked leader=%s entity=0x%llX map=0x%llX", options.leaderName.c_str(), leader, baseline.map);
                                } else {
                                    leader = positioned = playerComponent = 0;
                                    value.note = !complete ? "周围玩家扫描未完成，等待重试：" + scanError :
                                        matches > 1 ? "发现多个同名玩家，暂停锁定" :
                                        (seen ? "队长已离开可见范围，正在重新寻找" : "周围未找到队长，正在寻找");
                                }
                            }
                            if (leader && positioned) {
                                static_assert(kPositioned_EntYOff == kPositioned_EntXOff + sizeof(int32_t));
                                nav::PixelPoint point{};
                                World after;
                                const bool separate = leader != baseline.player && positioned != self.positioned && playerComponent != self.playerComponent;
                                const bool read = separate && session.memory.ReadInto(positioned + kPositioned_EntXOff, &point, sizeof(point));
                                nav::PixelPoint selfPoint{};
                                const bool selfRead = session.memory.ReadInto(self.positioned + kPositioned_EntXOff, &selfPoint, sizeof(selfPoint));
                                { std::lock_guard<std::mutex> lock(mutex_); status_.selfPosition = selfPoint; status_.selfPositionValid = selfRead; }
                                if (tick - lastLogAt >= 1000 || !read) {
                                    lastLogAt = tick;
                                    LogDiagnostic("[SAMPLE] seq=%llu requested=%s self_name=%s self=0x%llX self_pos=0x%llX self_xy_addr=0x%llX self_ok=%d self_xy=(%d,%d) leader=0x%llX leader_Player=0x%llX leader_pos=0x%llX leader_xy_addr=0x%llX leader_ok=%d leader_xy=(%d,%d) alias=%d equal_xy=%d",
                                        sequence + 1, options.leaderName.c_str(), self.name.c_str(), baseline.player, self.positioned,
                                        self.positioned + kPositioned_EntXOff, selfRead, selfPoint.x, selfPoint.y, leader, playerComponent,
                                        positioned, positioned + kPositioned_EntXOff, read, point.x, point.y, !separate,
                                        read && selfRead && point.x == selfPoint.x && point.y == selfPoint.y);
                                }
                                if (read && point.x >= 0 && point.y >= 0 && uint32_t(point.x) < baseline.width &&
                                    uint32_t(point.y) < baseline.height && ReadWorld(session, after) && baseline.Same(after)) {
                                    value.valid = true; value.position = point; value.entity = leader; value.sampledAt = GetTickCount64();
                                } else { leader = positioned = playerComponent = 0; value.note = "队长坐标读取失败或实体与自身重合，正在重新锁定"; }
                            }
                            if (value.note.empty() && !value.valid) value.note = "正在重新寻找队长";
                        }
                        value.absent = absent; value.searchedAt = searchedAt;
                        if (lastNote != value.note) { lastNote = value.note; LogDiagnostic("[SAMPLE_STATE] valid=%d note=%s", value.valid, value.note.c_str()); }
                        publish(std::move(value));
                        std::unique_lock<std::mutex> lock(mutex_);
                        const auto elapsed = GetTickCount64() - tick;
                        const auto wait = elapsed < options.sampleMs ? options.sampleMs - elapsed : 1;
                        wake_.wait_for(lock, std::chrono::milliseconds(wait), [&] { return stop_ || samplingStop_; });
                    }
                } catch (const std::exception& e) {
                    Snapshot value; value.terminal = true; value.note = e.what(); publish(std::move(value));
                }
            });
            Channel channel; channel.options = options;
            channel.sample = [&] { std::lock_guard<std::mutex> lock(mutex_); return status_.target; };
            channel.travel = [&](const Snapshot& sample, bool allowAction) { return travel.Poll(sample, allowAction); };
            channel.enterPortal = [&](uint32_t key, uint64_t entity) { return travel.Enter(key, entity); };
            LootController loot(session, baseline, connection, stop_, options);
            std::vector<std::string> lootNames; ParseLootNames(options.lootNames, lootNames, error);
            if (!lootNames.empty()) {
                channel.loot = [&](nav::PixelPoint player) { return loot.Poll(player); };
                channel.pick = [&](uint32_t key, uint64_t entity, std::string& why) { return loot.Pick(key, entity, why); };
            }
            uint64_t navLogAt = 0; auto navState = State::Stopped;
            channel.publish = [&](const Feedback& feedback) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.navigation = feedback; status_.text = feedback.note.empty() ? StateText(feedback.state) : feedback.note;
                const auto now = GetTickCount64();
                if (feedback.state != navState || now - navLogAt >= 1000) {
                    navState = feedback.state; navLogAt = now;
                    LogDiagnostic("[NAV] state=%s pipe_self=(%d,%d) external_self_ok=%d external_self=(%d,%d) target=(%d,%d) distance=%.2f plans=%u calls=%u note=%s",
                        StateText(feedback.state), feedback.player.x, feedback.player.y, status_.selfPositionValid,
                        status_.selfPosition.x, status_.selfPosition.y, feedback.leader.x, feedback.leader.y,
                        feedback.distance, feedback.plans, feedback.calls, feedback.note.c_str());
                }
            };
            usecase::NavigateOptions navOptions; usecase::LoadNavOptionsFromIni(navOptions);
            navOptions.follow = &channel; navOptions.presetSessionId = connection.sessionId;
            navOptions.stepDistance = (std::min)(navOptions.stepDistance, (std::max)(10, options.distance));
            usecase::NavTelemetry telemetry;
            const int rc = usecase::Navigate(navOptions, &telemetry, &stop_);
            if (stop_) break;
            World after;
            if (rc == usecase::kFollowSceneReload || GetStatus().target.sceneChanged ||
                !ReadWorld(session, after) || !baseline.Same(after)) {
                LogDiagnostic("[SCENE_RELOAD] rc=%d note=%s", rc, telemetry.note.c_str());
                continue;
            }
            if (rc != 3 && rc != 0) {
                endText = telemetry.note; endState = State::Failed;
                const auto finalStatus = GetStatus();
                if (!finalStatus.navigation.lootName.empty())
                    endText = "拾取已停止（" + finalStatus.navigation.lootName + "）：" + endText;
            }
            break;
        }
    } catch (const std::exception& e) { endText = e.what(); endState = State::Failed; }
    samplingStop_ = true; wake_.notify_all();
    { std::lock_guard<std::mutex> lock(mutex_); status_.navigation.state = endState; status_.text = endText; status_.target.valid = false; }
    LOGI("[Follow] finished: %s", endText.c_str());
    LogDiagnostic("[FINISH] state=%s note=%s", StateText(endState), endText.c_str());
    running_ = false;
}
} // namespace follow
