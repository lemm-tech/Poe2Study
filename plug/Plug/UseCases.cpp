#include "UseCases.h"
#include <mutex>
#include <algorithm>
#include "MyOLAFunction.h"

#include <windows.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../Common/AppConfig.h"
#include "../Common/Logger.h"
#include "../Common/MapFrame.h"
#include "BmpWriter.h"
#include "MapShowcase.h"
#include "MapSink.h"
#include "NamedPipeClie.h"
#include "../Common/GameOffsets.h"
#include "../Common/PortalReturn.h"
#include <cmath>

namespace usecase {

bool ScanInventory(std::vector<poe::InventoryItem>& out, std::string& err)
{
    out.clear();
    err.clear();

    GameSession session;
    if (!InitializeSession(session, &err)) {
        return false;
    }

    poe::InventoryTraverser traverser(session.memory);
    const bool ok = traverser.Traverse(out, session.moduleBase);
    if (!ok) {
        err = "背包链解析失败（游戏可能已更新，需要重新校准 BaseOffset）";
    }
    session.driver.Detach();
    return ok;
}

bool ScanSkillSlots(poe::SkillSlotsSnapshot& out, std::string& err)
{
    out = {};
    err.clear();
    GameSession session;
    if (!InitializeSession(session, &err)) return false;
    const bool ok = poe::ReadSkillSlots(session.memory, session.moduleBase, out, err);
    session.driver.Detach();
    return ok;
}

bool ScanParty(poe::PartySnapshot& out, std::string& err, const std::atomic_bool* stop)
{
    out = {}; err.clear();
    GameSession session;
    if (!InitializeSession(session, &err)) return false;
    if (!poe::ReadParty(session.memory, session.moduleBase, out, err, stop)) return false;
    if (!app::Config().TargetAlive()) {
        out = {}; err = "目标游戏已退出，请重新打开对应角色面板"; return false;
    }
    return true;
}

bool TeleportToPartyMember(const poe::PartyMember& member, uint32_t expectedPid,
    uint64_t expectedSession, uint64_t expectedController, packet::Reply& reply,
    std::string& err, const std::atomic_bool* stop, bool* submitted, bool requireSameMap,
    const std::function<bool()>& validateBeforeSend)
{
    reply = {}; err.clear();
    if (submitted) *submitted = false;
    const auto current = [&] {
        const auto connection = pipe::GetConnectionSnapshot();
        return expectedPid && expectedSession && expectedController && app::Config().TargetAlive() &&
            !(stop && stop->load()) && connection.ready && connection.gamePid == expectedPid &&
            connection.sessionId == expectedSession && connection.controllerId == expectedController;
    };
    if (!current()) { err = "游戏或通信会话已变化，请连接新版 DLL 后重试"; return false; }
    poe::PartySnapshot latest;
    if (!ScanParty(latest, err, stop)) return false;
    if (latest.pid != expectedPid || member.self || member.name == latest.selfName) {
        err = "所选队友不属于当前角色面板，或选择了自身"; return false;
    }
    size_t matches = 0;
    for (const auto& candidate : latest.members)
        if (!candidate.self && candidate.name == member.name && candidate.account == member.account) {
            if (requireSameMap && (candidate.mapStatus != poe::PartyMapStatus::Resolved ||
                member.mapId.empty() || candidate.mapId != member.mapId)) {
                err = "队长所在地图已变化，等待重新判断"; return false;
            }
            ++matches;
        }
    if (matches != 1) { err = "该队友已离队或身份已变化，请重新获取队友列表"; return false; }

    packet::PartyTeleportTarget target{};
    const auto encode = [](const std::string& text, char16_t* out, uint32_t limit, uint32_t& length) {
        if (text.empty() || text.size() > size_t(limit) * 4) return false;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
        if (count <= 0 || uint32_t(count) > limit) return false;
        std::wstring wide(size_t(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), wide.data(), count) != count) return false;
        static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows UTF-16");
        std::memcpy(out, wide.data(), size_t(count) * sizeof(char16_t)); length = uint32_t(count); return true;
    };
    if (!encode(member.name, target.name, packet::kPartyNameLimit, target.nameLength) ||
        !encode(member.account, target.account, packet::kPartyAccountLimit, target.accountLength) || !packet::ValidTarget(target)) {
        err = "队友名称或账号格式无效或过长，未发送"; return false;
    }
    pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
    if (!current() || !client.Initialize() || client.SessionId() != expectedSession || client.ClientId() != expectedController) {
        err = "通信会话已变化，未发送，请重新连接后重试"; return false;
    }
    packet::Request request{}; request.operation = packet::Operation::TeleportToPartyMember;
    request.length = sizeof(target); std::memcpy(request.bytes, &target, sizeof(target));
    packet::Message message(request); std::vector<uint8_t> body;
    if (!current()) { err = "操作已取消或会话已变化，未发送"; return false; }
    if (validateBeforeSend && !validateBeforeSend()) { err = "周围队长或场景状态变化，等待重新判断"; return false; }
    if (submitted) *submitted = true; // Ambiguous/lost replies still consume one attempt.
    if (!client.Request(&message, MT_PacketReply, body, 8000) || body.size() != sizeof(reply)) {
        err = "未收到队友传送回执；未自动重发，请先检查游戏是否已经切图"; return false;
    }
    std::memcpy(&reply, body.data(), sizeof(reply));
    reply.detail[sizeof(reply.detail) - 1] = 0;
    if (reply.version != packet::kVersion || reply.objectLength > sizeof(reply.object) ||
        (reply.result == packet::Result::Ok && reply.invoked != 1)) {
        err = "队友传送回执不匹配；未自动重发，请先检查游戏状态"; return false;
    }
    return true;
}

bool ScanSurroundings(std::vector<poe::EntitySnapshot>& out, std::string& err)
{
    out.clear();
    err.clear();

    GameSession session;
    if (!InitializeSession(session, &err)) {
        return false;
    }

    poe::SurroundingsTraverser traverser(session.memory);
    const bool ok = traverser.Traverse(out, session.moduleBase);
    if (!ok) {
        err = "周围实体链解析失败（游戏可能已更新，需要重新校准 BaseOffset）";
    }
    session.driver.Detach();
    return ok;
}

// ------------------------------------------------------------
// 祭祀候选：只读扫描，并校验扫描前后属于同一地图实例。
// ------------------------------------------------------------
bool ScanRitualCandidates(RitualScanResult& out, std::string& err, const std::atomic_bool* stop)
{
    out = {}; err.clear();
    GameSession session;
    if (!InitializeSession(session, &err)) return false;
    poe::SurroundingsTraverser traverser(session.memory);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (stop && stop->load()) { err = "祭祀扫描已取消"; return false; }
        RitualScanResult fresh;
        poe::WorldAreaSnapshot after;
        poe::EntityScanInfo info;
        if (!poe::ReadWorldArea(session.memory, session.moduleBase, fresh.area, err)) continue;
        if (!traverser.ListRitualCandidates(session.moduleBase, fresh.candidates, info, stop)) {
            err = info.error; continue;
        }
        if (!poe::ReadWorldArea(session.memory, session.moduleBase, after, err)) continue;
        if (!fresh.area.SameMap(after)) { err = "扫描期间已切换地图，请在场景稳定后重新扫描"; continue; }
        if (!app::Config().TargetAlive()) { err = "目标游戏已退出"; return false; }
        fresh.visited = info.visited;
        std::sort(fresh.candidates.begin(), fresh.candidates.end(), [](const auto& a, const auto& b) {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.key < b.key;
        });
        out = std::move(fresh); err.clear(); return true;
    }
    if (err.empty()) err = "祭祀扫描未完成，请重试";
    return false;
}

bool InteractRitual(const poe::EntitySnapshot& interactable,
    const poe::EntitySnapshot& expectedObject, const poe::WorldAreaSnapshot& expectedArea,
    uint32_t expectedPid, uint64_t expectedSession, uint64_t expectedController,
    bool appendEnd, packet::Reply& reply, std::string& err,
    const std::atomic_bool* stop, bool* submitted,
    const std::function<bool()>& validateBeforeSend)
{
    constexpr double kMaxAssociationDistance = 46.0;
    constexpr auto kRitualObjectMetadata = "Metadata/Terrain/Leagues/Ritual/RitualRuneObject";
    constexpr auto kRitualInteractableMetadata = "Metadata/Terrain/Leagues/Ritual/RitualRuneInteractable";
    reply = {}; err.clear();
    if (submitted) *submitted = false;
    const auto validNow = [&] {
        const auto connection = pipe::GetConnectionSnapshot();
        return expectedPid && expectedSession && expectedController && app::Config().TargetAlive() &&
            !(stop && stop->load()) && connection.ready && connection.gamePid == expectedPid &&
            connection.sessionId == expectedSession && connection.controllerId == expectedController;
    };
    if (!validNow()) { err = "游戏或通信会话已变化，未发送祭祀交互"; return false; }
    if (!expectedArea.Available() || expectedObject.metadataPath != kRitualObjectMetadata ||
        !interactable.key || !interactable.entity || interactable.metadataPath != kRitualInteractableMetadata) {
        err = "祭祀对象身份或地图信息无效，请重新扫描"; return false;
    }
    GameSession session;
    if (!InitializeSession(session, &err)) return false;
    if (session.game.pid != expectedPid) { err = "游戏进程已变化，未发送祭祀交互"; return false; }
    poe::WorldAreaSnapshot before, after;
    if (!poe::ReadWorldArea(session.memory, session.moduleBase, before, err) || !expectedArea.SameMap(before)) {
        err = "当前地图实例已变化，请重新规划祭祀路线"; return false;
    }
    std::vector<poe::EntitySnapshot> candidates;
    poe::EntityScanInfo info;
    if (!poe::SurroundingsTraverser(session.memory).ListRitualCandidates(
            session.moduleBase, candidates, info, stop)) {
        err = info.error.empty() ? "祭祀对象扫描失败，未发送" : info.error;
        return false;
    }
    if (!poe::ReadWorldArea(session.memory, session.moduleBase, after, err) ||
        !before.SameMap(after) || !expectedArea.SameMap(after)) {
        err = "扫描期间地图实例已变化，请重新规划祭祀路线"; return false;
    }

    const poe::EntitySnapshot* currentInteractable = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.metadataPath == kRitualInteractableMetadata && candidate.key == interactable.key &&
            candidate.entity == interactable.entity) {
            if (currentInteractable) { err = "祭祀交互对象存在重复记录，未发送"; return false; }
            currentInteractable = &candidate;
        }
    }
    if (!currentInteractable) { err = "祭祀交互对象已变化或离开周围范围，请重新扫描"; return false; }

    const auto distanceSquared = [](int32_t ax, int32_t ay, int32_t bx, int32_t by) {
        const double dx = double(ax) - double(bx), dy = double(ay) - double(by);
        return dx * dx + dy * dy;
    };
    const auto nearestObject = [&](int32_t x, int32_t y, const poe::EntitySnapshot*& nearest) {
        nearest = nullptr;
        double nearestDistance = 0;
        bool tied = false;
        for (const auto& candidate : candidates) {
            if (candidate.metadataPath != kRitualObjectMetadata) continue;
            const double distance = distanceSquared(x, y, candidate.x, candidate.y);
            if (!nearest || distance < nearestDistance) {
                nearest = &candidate; nearestDistance = distance; tied = false;
            } else if (distance == nearestDistance) tied = true;
        }
        return nearest && !tied && nearestDistance <= kMaxAssociationDistance * kMaxAssociationDistance;
    };
    const poe::EntitySnapshot* associatedObject = nullptr;
    const poe::EntitySnapshot* currentExpectedObject = nullptr;
    if (!nearestObject(currentInteractable->x, currentInteractable->y, associatedObject) ||
        !nearestObject(expectedObject.x, expectedObject.y, currentExpectedObject) ||
        distanceSquared(currentExpectedObject->x, currentExpectedObject->y, expectedObject.x, expectedObject.y) > 4.0 ||
        distanceSquared(associatedObject->x, associatedObject->y,
                        currentExpectedObject->x, currentExpectedObject->y) != 0) {
        err = "祭祀交互点与路线目标无法唯一关联，请重新扫描或等待靠近"; return false;
    }

    auto playerEntity = session.memory.Follow(session.moduleBase + BaseOffset);
    for (int i = 0; playerEntity && i < kLocalPlayerChainDepth; ++i)
        playerEntity = session.memory.Follow(playerEntity + kMapObjectChain[i]);
    if (!playerEntity) { err = "无法读取当前角色位置，未发送祭祀交互"; return false; }
    poe::Entity player(session.memory, playerEntity);
    const auto positioned = player.GetAs<poe::PositionComponent>("Positioned");
    if (!positioned) { err = "无法读取当前角色位置，未发送祭祀交互"; return false; }
    const auto playerPosition = positioned->GetPosition();
    if (playerPosition.x < 0 || playerPosition.y < 0) {
        err = "当前角色坐标无效，未发送祭祀交互"; return false;
    }
    if (distanceSquared(playerPosition.x, playerPosition.y, currentInteractable->x,
                        currentInteractable->y) > kMaxAssociationDistance * kMaxAssociationDistance) {
        err = "当前角色距离祭祀交互点超过 46，未发送"; return false;
    }
    if (!validNow()) { err = "游戏或通信会话已变化，未发送祭祀交互"; return false; }

    pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
    if (!client.Initialize() || client.SessionId() != expectedSession || client.ClientId() != expectedController ||
        !validNow()) {
        err = "祭祀交互管道会话已变化，未发送"; return false;
    }
    poe::WorldAreaSnapshot finalArea;
    if (!poe::ReadWorldArea(session.memory, session.moduleBase, finalArea, err) || !expectedArea.SameMap(finalArea)) {
        err = "发包前地图实例已变化，未发送祭祀交互"; return false;
    }
    if ((stop && stop->load()) || (validateBeforeSend && !validateBeforeSend()) || !validNow()) {
        err = "祭祀交互已取消或导航状态变化，未发送"; return false;
    }

    ritualstate::Snapshot ritual;
    if (!ritualstate::ReadEntity(currentInteractable->entity, [&](uint64_t address, void* out, size_t size) {
            return session.memory.ReadInto(address, out, size);
        }, ritual) || !ritual.CanStart()) {
        err = std::string("祭祀当前不可开始，未发送：") + ritualstate::Text(ritual); return false;
    }

    packet::Request request{};
    request.operation = packet::Operation::InteractRitual;
    request.itemKey = currentInteractable->key;
    request.expectedEntity = currentInteractable->entity;
    request.actionFlags = packet::kInteractFlags;
    if (appendEnd) request.options |= packet::AppendEnd;
    packet::Message message(request);
    std::vector<uint8_t> body;
    if (submitted) *submitted = true;
    if (!client.Request(&message, MT_PacketReply, body, 8000) || body.size() != sizeof(reply)) {
        err = "未收到祭祀交互回执；请求可能已发送，等待 current_state 确认开启";
        return false;
    }
    std::memcpy(&reply, body.data(), sizeof(reply));
    reply.detail[sizeof(reply.detail) - 1] = 0;
    if (reply.version != packet::kVersion || reply.objectLength > sizeof(reply.object) ||
        (reply.result == packet::Result::Ok && reply.invoked != (appendEnd ? 2u : 1u))) {
        err = "祭祀交互回执不匹配；等待 current_state 确认开启"; return false;
    }
    return true;
}

namespace {
bool PortalAction(const poe::EntitySnapshot* portal, const poe::WorldAreaSnapshot& source,
    uint64_t player, uint32_t expectedPid, uint64_t expectedSession, uint64_t expectedController,
    packet::Reply& reply, std::string& err, const std::atomic_bool* stop, bool* submitted,
    const std::function<bool()>& validateBeforeSend)
{
    reply = {}; err.clear();
    if (submitted) *submitted = false;
    const auto current = [&]() {
        const auto c = pipe::GetConnectionSnapshot();
        return expectedPid && expectedSession && expectedController && app::Config().TargetAlive() &&
            !(stop && stop->load()) && c.ready && c.gamePid == expectedPid &&
            c.sessionId == expectedSession && c.controllerId == expectedController;
    };
    const packet::PortalScene expected{source.world, source.mapOwner, source.record, player};
    if (!current() || !source.Available() || !portalreturn::ValidScene(expected)) {
        err = "传送门源地图、角色或通信会话无效，未发送"; return false;
    }
    GameSession session;
    if (!InitializeSession(session, &err) || session.game.pid != expectedPid) {
        err = "传送门游戏进程已变化，未发送"; return false;
    }
    const auto sameScene = [&]() {
        packet::PortalScene scene; poe::WorldAreaSnapshot area;
        return current() && portalreturn::ReadScene(session.moduleBase, [&](uint64_t a, void* p, size_t n) {
            return session.memory.ReadInto(a, p, n);
        }, scene) && scene == expected && poe::ReadWorldArea(session.memory, session.moduleBase, area, err) && source.SameMap(area);
    };
    if (!sameScene()) { err = "传送门操作的地图实例或角色已变化，未发送"; return false; }
    if (portal) {
        std::vector<poe::EntitySnapshot> portals; poe::EntityScanInfo info;
        if (!portalreturn::IsHideoutPortal(*portal) ||
            !poe::SurroundingsTraverser(session.memory).ListPortals(session.moduleBase, portals, info, stop)) {
            err = "进门前目标或周围传送门暂不可读：" + info.error; return false;
        }
        const auto found = std::find_if(portals.begin(), portals.end(), [&](const auto& item) {
            return portalreturn::Id(item) == portalreturn::Id(*portal) && portalreturn::IsHideoutPortal(item) && item.name == portal->name;
        });
        poe::Entity self(session.memory, player);
        const auto position = self.GetAs<poe::PositionComponent>("Positioned");
        int32_t xy[2]{};
        if (found == portals.end() || !position ||
            !session.memory.ReadInto(position->Ptr() + kPositioned_EntXOff, xy, sizeof(xy)) || xy[0] < 0 || xy[1] < 0 ||
            portalreturn::Distance(xy[0], xy[1], found->x, found->y) > portalreturn::kEnterDistance) {
            err = "新召唤的藏身處门已变化或距离超过 18，未发送进门"; return false;
        }
    }
    pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
    if (!client.Initialize() || client.SessionId() != expectedSession || client.ClientId() != expectedController ||
        (validateBeforeSend && !validateBeforeSend()) || !sameScene()) {
        err = "传送门操作已取消，或地图／通信会话变化，未发送"; return false;
    }
    packet::Request request{};
    request.operation = portal ? packet::Operation::EnterPortal : packet::Operation::SummonPortal;
    request.length = sizeof(expected); std::memcpy(request.bytes, &expected, sizeof(expected));
    if (portal) { request.itemKey = portal->key; request.expectedEntity = portal->entity; }
    packet::Message message(request); std::vector<uint8_t> body;
    if (submitted) *submitted = true;
    if (!client.Request(&message, MT_PacketReply, body, 8000) || body.size() != sizeof(reply)) {
        err = "未收到传送门回执；请求可能已提交，继续观察游戏，不自动重发"; return false;
    }
    std::memcpy(&reply, body.data(), sizeof(reply)); reply.detail[sizeof(reply.detail) - 1] = 0;
    if (reply.version != packet::kVersion || reply.objectLength > sizeof(reply.object) ||
        (reply.result == packet::Result::Ok && reply.invoked != 1)) {
        err = "传送门回执不匹配；继续观察游戏，不自动重发"; return false;
    }
    return true;
}
} // namespace

bool SummonPortal(const poe::WorldAreaSnapshot& source, uint64_t player, uint32_t expectedPid,
    uint64_t expectedSession, uint64_t expectedController, packet::Reply& reply, std::string& err,
    const std::atomic_bool* stop, bool* submitted, const std::function<bool()>& validateBeforeSend)
{
    return PortalAction(nullptr, source, player, expectedPid, expectedSession, expectedController,
        reply, err, stop, submitted, validateBeforeSend);
}
bool EnterHideoutPortal(const poe::EntitySnapshot& portal, const poe::WorldAreaSnapshot& source,
    uint64_t player, uint32_t expectedPid, uint64_t expectedSession, uint64_t expectedController,
    packet::Reply& reply, std::string& err, const std::atomic_bool* stop, bool* submitted,
    const std::function<bool()>& validateBeforeSend)
{
    return PortalAction(&portal, source, player, expectedPid, expectedSession, expectedController,
        reply, err, stop, submitted, validateBeforeSend);
}

// 一次采集背包与周围。
bool ScanSnapshot(ScanResult& out, std::string& err)
{
    out = ScanResult{};
    err.clear();

    GameSession session;
    if (!InitializeSession(session, &err)) {
        return false;
    }

    bool ok = true;

    poe::InventoryTraverser invTraverser(session.memory);
    if (!invTraverser.Traverse(out.inventory, session.moduleBase)) {
        err = "背包链解析失败（游戏可能已更新，需要重新校准 BaseOffset）";
        ok = false;
    }

    poe::SurroundingsTraverser surTraverser(session.memory);
    if (!surTraverser.Traverse(out.surroundings, session.moduleBase)) {
        if (!err.empty()) {
            err += "；";
        }
        err += "周围实体链解析失败（游戏可能已更新，需要重新校准 BaseOffset）";
        ok = false;
    }

    session.driver.Detach();
    return ok;
}

// ------------------------------------------------------------
// 地图客户端：从 Monitor.dll 收代价表 -> 生成 BMP
//
// 重构成三层：
//   PipeSink      收包 + 解包 + O(1) 判完成
//   MapShowcase   代价 -> 灰度
//   BmpWriter     灰度 -> BMP 文件
// MapRenderer 那 98 行「五合一」被彻底拆开。
// ------------------------------------------------------------
namespace {

std::atomic_bool g_running{ true };
std::atomic_bool g_saved{ false };

mapsink::PipeSink          g_sink;
const mapshow::IMapShowcase* g_showcase = nullptr;
std::string                g_outPath;
int                        g_savedWidth = 0;
int                        g_savedHeight = 0;

// 扫图落盘的完成回执（Monitor 写完 BMP 后回一个 MT_Ack）
std::atomic_bool g_dumpDone{ false };

// 走路槽位扫描的回执（Monitor 扫完回 MT_WalkScanAck）
std::atomic_bool g_walkScanDone{ false };
int32_t          g_walkScanSlot = -1;

void OnSignal(int)
{
    g_running = false;
}

// 把当前 CostBuffer 存成 BMP
bool SaveCurrent(const char* tag)
{
    const mapdata::CostBuffer& cost = g_sink.Result();
    if (cost.Empty()) {
        LOGE("[%s] 代价表为空，无法存盘", tag);
        return false;
    }

    const mapdata::Geometry& geo = cost.Geo();
    const uint32_t W = geo.WorldW();
    const uint32_t H = geo.WorldH();

    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            gray[(size_t)y * W + x] = g_showcase->ToGray(cost.At(x, y));
        }
    }

    if (!bmp::WriteGray8(g_outPath, gray.data(), W, H)) {
        LOGE("[%s] BMP 写入失败: %s", tag, g_outPath.c_str());
        return false;
    }

    g_savedWidth = (int)W;
    g_savedHeight = (int)H;
    LOGI("[%s] 已生成 %s  (%u x %u, 着色=%s)",
         tag, g_outPath.c_str(), W, H, g_showcase->Name());
    return true;
}

void OnMessage(MessageType type, const void* data, size_t size)
{
    switch (type) {
    case MT_MapMeta: {
        if (size < sizeof(MapMeta)) {
            LOGW("[Client] MapMeta 长度不足: %zu", size);
            break;
        }
        MapMeta raw{};
        memcpy(&raw, data, sizeof(MapMeta));

        mapdata::MapFrame::Meta meta{};
        meta.width     = raw.width;
        meta.height    = raw.height;
        meta.rowStride = raw.rowStride;
        meta.layer     = raw.layer;

        std::string err;
        if (!g_sink.Begin(meta, err)) {
            LOGE("[Client] MapMeta 被拒绝: %s", err.c_str());
            break;
        }

        g_saved = false;
        LOGI("[Client] 元信息: %u x %u  rowStride=%u  layer=%u",
             meta.width, meta.height, meta.rowStride, (unsigned)meta.layer);
        break;
    }
    case MT_MapRow: {
        if (size < sizeof(MapRowHeader)) break;

        MapRowHeader hdr{};
        memcpy(&hdr, data, sizeof(MapRowHeader));

        mapdata::MapFrame::RowSlice slice;
        slice.y    = hdr.rowIndex;
        slice.off  = hdr.offset;
        slice.data = static_cast<const uint8_t*>(data) + sizeof(MapRowHeader);
        slice.len  = (uint32_t)(size - sizeof(MapRowHeader));

        if (!g_sink.Feed(slice)) {
            LOGW("[Client] Feed 失败 y=%u", hdr.rowIndex);
            break;
        }

        // ★ O(1) 判定（原来是每包全扫 1656 行）
        if (g_sink.Complete() && !g_saved.exchange(true)) {
            LOGI("[Client] 全部收齐：%u / %u 行",
                 g_sink.RowsDone(), g_sink.RowsTotal());
            SaveCurrent("Client");
        }
        break;
    }
    case MT_Ack:
        LOGI("[Client] 收到 Ack");
        g_dumpDone = true;
        break;
    case MT_WalkScanAck: {
        if (size < sizeof(WalkScanAck)) break;
        WalkScanAck ack{};
        memcpy(&ack, data, sizeof(ack));
        g_walkScanSlot = ack.slot;
        g_walkScanDone = true;
        LOGI("[Scan] 回执：slot=0x%X objMode=%u argMode=%u 试了 %d 个，(%d,%d)->(%d,%d)",
             (unsigned)ack.slot, (unsigned)ack.objMode, (unsigned)ack.argMode,
             (int)ack.tested, (int)ack.fromX, (int)ack.fromY, (int)ack.toX, (int)ack.toY);
        break;
    }
    case MT_Exit:
        LOGI("[Client] 收到退出指令");
        g_running = false;
        break;
    default:
        LOGW("[Client] 未知消息类型: %d 长度: %zu", (int)type, size);
        break;
    }
}

} // namespace

int FetchMap(const std::string& showcaseName)
{
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    const app::AppConfig& cfg = app::Config();

    g_showcase = &mapshow::GetShowcase(showcaseName);
    g_outPath  = cfg.BmpPath();
    g_saved    = false;

    LOGI("===== 地图客户端 =====");
    LOGI("管道: %ws", cfg.PipeFullName().c_str());
    LOGI("着色: %s", g_showcase->Name());
    LOGI("输出: %s", g_outPath.c_str());
    LOGI("等待 Monitor.dll ...");

    pipe::NamedPipeClie client(cfg.PipeFullName(), OnMessage, managed::Role::Data);
    if (!client.Initialize()) {
        LOGE("管道客户端初始化失败");
        return 1;
    }
    LOGI("已连接 Monitor，发起扫图请求...");

    MapReqMessage req;
    client.SendPackage(&req);

    // 超时保护：原来没有超时，Monitor 采集失败时这里会永远阻塞。
    // 30 秒足够一次正常采集（实测 array 策略 0 ms，
    // 即使回退到逐点 CostCall 也只需几秒）。
    const ULONGLONG t0 = GetTickCount64();
    constexpr ULONGLONG kTimeoutMs = 30000;
    bool timedOut = false;

    while (g_running) {
        if (g_saved.load()) {
            Sleep(300);
            break;
        }
        if (GetTickCount64() - t0 > kTimeoutMs) {
            timedOut = true;
            break;
        }
        Sleep(100);
    }

    client.Uninitialize();

    if (g_saved.load()) {
        LOGI("地图客户端结束（%d x %d）", g_savedWidth, g_savedHeight);
        return 0;
    }
    if (timedOut) {
        LOGE("地图客户端超时（%llu 秒）：Monitor 没有送来完整地图",
             (unsigned long long)(kTimeoutMs / 1000));
        LOGE("  排查：看 monitor_log.txt 里 [采集] 那几行，"
             "若是「代价数组为空」说明游戏正在切图，进图后再试");
        return 1;
    }
    LOGE("地图客户端结束：未收到完整地图");
    return 1;
}

// ------------------------------------------------------------
// 回放：从 .bin 文件还原代价表 -> 生成 BMP（不需要游戏/管道）
//
// 这是离线调试的主要入口：
//   改渲染/改着色/改 BMP 格式，不用开游戏，跑一条命令就能看结果。
// ------------------------------------------------------------
int ReplayMap(const std::string& binPath, uint32_t gridW, uint32_t gridH,
              const std::string& showcaseName, const std::string& outPath)
{
    LOGI("===== 地图回放 =====");
    LOGI("输入: %s", binPath.c_str());
    LOGI("网格: %u x %u", gridW, gridH);

    g_showcase = &mapshow::GetShowcase(showcaseName);
    g_outPath  = outPath.empty() ? app::Config().BmpPath() : outPath;
    LOGI("着色: %s   输出: %s", g_showcase->Name(), g_outPath.c_str());

    mapsink::ReplaySink sink;
    std::string err;
    if (!sink.LoadFile(binPath, gridW, gridH, 0, err)) {
        LOGE("加载失败: %s", err.c_str());
        return 1;
    }

    LOGI("识别格式: %s", sink.DetectedFormat());
    LOGI("世界尺寸: %u x %u", sink.Result().Geo().WorldW(), sink.Result().Geo().WorldH());
    LOGI("非零点数: %zu / %zu",
         sink.Result().CountNonZero(), sink.Result().Size());

    // 复用同一套存盘逻辑：临时把 sink 的数据接到 g_sink 不合适，
    // 这里直接内联一遍（只有 10 行，比为了复用引入间接层更清楚）
    const mapdata::CostBuffer& cost = sink.Result();
    if (cost.Empty()) {
        LOGE("代价表为空");
        return 1;
    }

    const mapdata::Geometry& geo = cost.Geo();
    const uint32_t W = geo.WorldW();
    const uint32_t H = geo.WorldH();

    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            gray[(size_t)y * W + x] = g_showcase->ToGray(cost.At(x, y));
        }
    }

    if (!bmp::WriteGray8(g_outPath, gray.data(), W, H)) {
        LOGE("BMP 写入失败: %s", g_outPath.c_str());
        return 1;
    }

    LOGI("已生成 %s  (%u x %u, 着色=%s)",
         g_outPath.c_str(), W, H, g_showcase->Name());
    return 0;
}

// ------------------------------------------------------------
// 扫图落盘：请 Monitor 在注入端直接写 BMP（不经过管道传图）
//
// 用途：判断"是读错了还是传错了"。
//   Monitor 写的那张 = 游戏进程内看到的原始代价表
//   Plug 出的那张   = 经过 nibble 打包 -> 管道 -> 解包的产物
// 两张一比，问题出在哪一段一目了然。
// ------------------------------------------------------------
int DumpMapOnMonitor(uint8_t layer, uint8_t style, bool markPlayer)
{
    const app::AppConfig& cfg = app::Config();

    LOGI("===== 扫图落盘（Monitor 端）=====");
    g_dumpDone = false;

    pipe::NamedPipeClie client(cfg.PipeFullName(), OnMessage, managed::Role::Data);
    if (!client.Initialize()) {
        LOGE("管道客户端初始化失败（Monitor.dll 注入了吗？）");
        return 1;
    }

    ScanBmpReq req{};
    req.layer = layer;
    req.style = style;
    req.markStart = markPlayer ? 1 : 0;
    req.markX = -1;
    req.markY = -1;

    ScanBmpMessage msg(req);
    if (!client.SendPackage(&msg)) {
        LOGE("请求发送失败");
        client.Uninitialize();
        return 1;
    }

    // 采集 + 存盘在 Monitor 的 Data 工作线程执行。
    // 没有专门的回执消息（这个命令只给人用、频率极低，加回执不值得），
    // 所以这里按"等到它做完或者超时"处理：Monitor 完成后会回一个 MT_Ack。
    const ULONGLONG t0 = GetTickCount64();
    while (!g_dumpDone.load() && (GetTickCount64() - t0) < 8000) {
        Sleep(50);
    }
    if (!g_dumpDone.load()) {
        LOGW("8 秒内没收到 Monitor 的回执，但它可能已经写完了（看文件时间戳）");
    }

    client.Uninitialize();
    LOGI("看 Monitor.dll 同目录的 dump_layer%u_*.bmp 和 dump_walkmask.bmp",
         (unsigned)layer);
    return 0;
}

// ------------------------------------------------------------
// 走路 call 槽位扫描
//
// 这条路只走管道，不碰任何偏移：请求发给 Monitor，由它在游戏进程内
// 逐个 (对象, 槽位, 参数形态) 试调，用玩家坐标是否变化判胜负。
//
// 回执 MT_WalkScanAck 里带命中槽位（-1 = 没找到），日志在 monitor_log.txt。
// ------------------------------------------------------------
int ScanWalkSlot(uint8_t objMode, uint8_t argMode,
                 uint32_t slotStart, uint32_t slotEnd,
                 int32_t tx, int32_t ty, uint32_t settleMs, uint64_t objAddr)
{
    const app::AppConfig& cfg = app::Config();

    g_walkScanDone = false;
    g_walkScanSlot = -1;

    LOGI("===== 走路 call 槽位扫描 =====");
    LOGI("对象模式=%u 参数模式=%u 槽位 0x%X~0x%X 目标(%d,%d)",
         (unsigned)objMode, (unsigned)argMode, slotStart, slotEnd, tx, ty);
    LOGI("扫描期间游戏会短暂卡顿，结果看 monitor_log.txt 的 [Scan] 行");

    pipe::NamedPipeClie client(cfg.PipeFullName(), OnMessage);
    if (!client.Initialize()) {
        LOGE("管道客户端初始化失败（Monitor.dll 注入了吗？）");
        return 1;
    }

    WalkScanReq req{};
    req.slotStart = slotStart;
    req.slotEnd   = slotEnd;
    req.tx        = tx;
    req.ty        = ty;
    req.argMode   = argMode;
    req.objMode   = objMode;
    req.settleMs  = (uint16_t)(settleMs ? settleMs : 400);
    req.objAddr   = objAddr;

    WalkScanMessage msg(req);
    if (!client.SendPackage(&msg)) {
        LOGE("请求发送失败");
        client.Uninitialize();
        return 1;
    }

    // 每个槽位 400ms，留足余量后超时
    const uint32_t slots = (slotEnd > slotStart) ? ((slotEnd - slotStart) / 8) : 1;
    const ULONGLONG perSlot = (ULONGLONG)(settleMs ? settleMs : 400) + 150ULL;
    const ULONGLONG budget = (ULONGLONG)slots * perSlot + 8000ULL;
    const ULONGLONG t0 = GetTickCount64();

    while (!g_walkScanDone.load() && (GetTickCount64() - t0) < budget) {
        Sleep(100);
    }

    client.Uninitialize();

    if (g_walkScanDone.load()) {
        LOGI("扫描完成，命中槽位 = 0x%X（-1 表示没找到）", (unsigned)g_walkScanSlot);
    }
    else {
        LOGW("扫描超时（%llu ms），但 Monitor 可能仍在跑，看 monitor_log.txt",
             (unsigned long long)(GetTickCount64() - t0));
    }
    return 0;
}


// ------------------------------------------------------------
// 技能释放诊断（新战斗模块）
//
// 这条路只走管道：请求发给 Monitor，由它在游戏进程内按参数调用技能 call。
// Plug 侧不碰任何偏移，也不认识 shared_ptr —— 它只负责把"想怎么调"说清楚。
//
// 为什么允许逐字段覆盖：诊断阶段的每一次验证都是"改一个变量、看有没有效果"。
// 把参数全暴露出来，就不用每验证一个假设就重编一次 DLL。
// ------------------------------------------------------------
namespace {

std::atomic_bool g_skillAckDone{ false };
SkillAck         g_skillAck{};

// 停止施法的回执（与释放分开，互不干扰）
std::atomic_bool g_skillStopAckDone{ false };
SkillStopAck     g_skillStopAck{};

void OnSkillStopMessage(MessageType type, const void* data, size_t size)
{
    if (type != MT_SkillStopAck || !data || size < sizeof(SkillStopAck)) return;
    SkillStopAck ack{};
    memcpy(&ack, data, sizeof(ack));
    g_skillStopAck = ack;
    g_skillStopAckDone = true;
}

void OnSkillMessage(MessageType type, const void* data, size_t size)
{
    if (type != MT_SkillAck) return;
    if (!data || size < sizeof(SkillAck)) return;
    SkillAck ack{};
    memcpy(&ack, data, sizeof(ack));
    g_skillAck = ack;
    g_skillAckDone = true;
}

const char* SkillResultText(int32_t r)
{
    switch (r) {
    case 0: return "Ok";
    case 1: return "NoActionSystem";
    case 2: return "BadEntry";
    case 3: return "NoSkillPtr";
    case 4: return "OutOfBounds";
    case 5: return "Crashed";
    case 6: return "RateLimited";
    case 7: return "PreflightFailed";
    case 8: return "CastBudgetExhausted";
    default: return "Unknown";
    }
}

} // namespace

// ============================================================
// 只停止施法（不释放技能）
//
// 为什么不和 SkillProbe 合并：
//   释放 call 的状态依赖极强（多数时候空转，反复调会让游戏内存暴涨崩溃），
//   而停止 call 结构简单、每次都能生效。分开成两条通道，各自独立可用。
//
// 返回：0 = 成功，1 = 失败（管道/技能器问题）
// ============================================================
int SkillStop()
{
    static std::mutex skillStopMutex;
    std::lock_guard<std::mutex> stopLock(skillStopMutex);
    const app::AppConfig& cfg = app::Config();

    LOGI("===== 停止施法 =====");

    g_skillStopAckDone = false;
    g_skillStopAck = SkillStopAck{};

    SkillStopReq req{};
    SkillStopMessage msg(req);

    pipe::NamedPipeClie client(cfg.PipeFullName(), OnSkillStopMessage);
    if (!client.Initialize()) {
        LOGE("管道客户端初始化失败（Monitor.dll 注入了吗？）");
        return 1;
    }

    if (!client.SendPackage(&msg)) {
        LOGE("停止请求发送失败");
        client.Uninitialize();
        return 1;
    }

    // 等回执（最多 3 秒）
    const DWORD deadline = GetTickCount() + 3000;
    while (GetTickCount() < deadline) {
        if (g_skillStopAckDone.load()) break;
        Sleep(20);
    }

    client.Uninitialize();

    if (!g_skillStopAckDone.load()) {
        LOGE("等停止回执超时");
        return 1;
    }

    const SkillStopAck ack = g_skillStopAck;
    LOGI("停止结果: %s  rcx=0x%llX  gate 0x%02X -> 0x%02X",
         ack.result == 0 ? "成功" : "失败",
         (unsigned long long)ack.stopThis, ack.gateBefore, ack.gateAfter);
    return ack.result;
}

int SkillProbe(uint32_t slot, uint64_t r8, int32_t posX, int32_t posY,
               uint8_t rdxMode, uint8_t r9Mode,
               uint64_t stack28, uint64_t stack30, bool dryRun,
               uint32_t stopAfterMs, uint8_t posMode, int32_t aheadDistance,
               uint8_t hygiene)
{
    static std::mutex skillRequestMutex;
    std::unique_lock<std::mutex> requestLock(skillRequestMutex, std::try_to_lock);
    if (!requestLock.owns_lock()) return 3;
    const app::AppConfig& cfg = app::Config();

    g_skillAckDone = false;
    g_skillAck = SkillAck{};

    LOGI("===== 技能释放探针 =====");
    LOGI("槽位=%u r8=0x%llX 坐标=(%d,%d) rdxMode=%u r9Mode=%u dryRun=%d hygiene=0x%02X",
         slot, (unsigned long long)r8, posX, posY,
         (unsigned)rdxMode, (unsigned)r9Mode, (int)dryRun, (unsigned)hygiene);
    LOGI("栈参数 sp28=0x%llX sp30=0x%llX", (unsigned long long)stack28,
         (unsigned long long)stack30);

    // Logical request scope; the fixed Controller transport remains connected.
    pipe::NamedPipeClie client(cfg.PipeFullName(), OnSkillMessage);
    if (!client.Initialize()) {
        LOGE("管道客户端初始化失败（Monitor.dll 注入了吗？）");
        return 1;
    }

    SkillCastReq req{};
    req.slot    = (int32_t)slot;
    req.r8      = r8;
    req.stack28 = stack28;
    req.stack30 = stack30;
    req.posX    = posX;
    req.posY    = posY;
    req.rdxMode = rdxMode;
    req.r9Mode  = r9Mode;
    req.dryRun  = dryRun ? 1 : 0;
    req.stopAfterMs = stopAfterMs;
    req.posMode = posMode;
    req.aheadDistance = aheadDistance;
    req.hygiene = hygiene;

    SkillCastMessage msg(req);
    if (!client.SendPackage(&msg)) {
        LOGE("请求发送失败");
        client.Uninitialize();
        return 1;
    }

    // 等回执：Cast 内部有熔断 Sleep（最长 60ms）+ SEH 调用，给足 5 秒
    const ULONGLONG t0 = GetTickCount64();
    while (!g_skillAckDone.load() && (GetTickCount64() - t0) < 5000ULL) {
        Sleep(50);
    }

    client.Uninitialize();

    if (!g_skillAckDone.load()) {
        LOGW("5 秒内没收到回执（看 monitor_log.txt 的 [Skill] 行）");
        return 0;
    }

    const SkillAck& a = g_skillAck;
    LOGI("----- 回执 -----");
    LOGI("结果       : %s (%d)", SkillResultText(a.result), a.result);
    LOGI("返回值     : %d", a.retValue);
    LOGI("动作系统   : 0x%llX", (unsigned long long)a.actionSys);
    LOGI("地图对象   : 0x%llX  (候选A=0x%llX)",
         (unsigned long long)a.mapObject, (unsigned long long)a.mapObjectA);
    LOGI("shared_ptr : 0x%llX  _Ptr=0x%llX _Rep=0x%llX ref=%u",
         (unsigned long long)a.skillPtrAddr,
         (unsigned long long)a.skillPtr,
         (unsigned long long)a.skillRep, a.skillRefCnt);
    LOGI("释放坐标   : (%d,%d)", a.posX, a.posY);
    LOGI("累计调用   : %u   剩余额度: %u", a.callCount, a.budgetLeft);
    LOGI("会话卫生   : 位=0x%02X 状态位写回=%s",
         (unsigned)a.hygiene, a.stateRestored ? "成功" : "未做/失败");
    LOGI("引用计数   : _Uses %u->%u   _Weaks %u->%u  %s",
         a.usesBefore, a.usesAfter, a.weakBefore, a.weakAfter,
         (a.usesAfter == a.usesBefore && a.weakAfter == a.weakBefore)
             ? "(已配平，下次可安全调用)" : "(未配平，下次可能崩)");
    return 0;
}

int SkillDump(uint32_t slot)
{
    // 只读诊断：发一条 dryRun 请求，Monitor 侧只解链、不调用
    return SkillProbe(slot, kSkillArgR8RightMouse, 0, 0, 0, 0,
                      kSkillArgStack28, kSkillArgStack30Default, true, 0, 1, 60);
}

// ============================================================
// GUI 用：释放一次技能，并把 Monitor 的真实回执带回来
//
// 为什么不能直接用 SkillProbe：
//   SkillProbe 收完回执只往日志打，然后无条件 return 0。
//   GUI 拿到 0 就以为成功，于是出现"点了按钮显示已释放，其实Monitor
//   那边被熔断拒了"这种骗人的反馈。这里把 result 翻译成明确的返回码。
// ============================================================
int SkillCastOnce(uint32_t slot, uint64_t r8, SkillAck *out, bool allowDuringMotionTest,
                  int32_t aimX, int32_t aimY) {
  static std::mutex skillRequestMutex;
  std::unique_lock<std::mutex> requestLock(skillRequestMutex, std::try_to_lock);
  if (!requestLock.owns_lock())
    return 3;
  const app::AppConfig &cfg = app::Config();
  g_skillAckDone = false;
  g_skillAck = SkillAck{};
  LOGI("===== GUI 释放技能 slot=%u r8=0x%llX%s；回执后立即 StopSkill =====",
       slot, (unsigned long long)r8,
       allowDuringMotionTest ? " [边走边放诊断]" : "");

  pipe::NamedPipeClie client(cfg.PipeFullName(), OnSkillMessage);
  if (!client.Initialize()) {
    LOGE("[GUI技能] 管道连不上（Monitor.dll 注入了吗？）");
    return 1;
  }
  SkillCastReq req{};
  req.slot = (int32_t)slot;
  req.r8 = r8;
  req.stack28 = kSkillArgStack28;
  req.stack30 = kSkillArgStack30Default;
  if (aimX >= 0 && aimY >= 0) {
    req.posX = aimX;
    req.posY = aimY;
    req.posMode = 0;
  } else {
    req.posX = 0;
    req.posY = 0;
    req.posMode = 1;
  }
  req.aheadDistance = 60;
  req.rdxMode = 0;
  req.r9Mode = 0;
  req.dryRun = 0;
  req.stopAfterMs =
      0; // SkillCall 回执后立即发独立 StopSkill 包，不使用固定延时。
  req.hygiene = (uint8_t)(0x03 | (allowDuringMotionTest ? 0x08 : 0x00));
  SkillCastMessage msg(req);
  if (!client.SendPackage(&msg)) {
    LOGE("[GUI技能] 请求发送失败，仍尝试发送 StopSkill 以清理可能已开始的释放");
    client.Uninitialize();
    (void)SkillStop();
    return 1;
  }
  const ULONGLONG t0 = GetTickCount64();
  while (!g_skillAckDone.load() && (GetTickCount64() - t0) < 8000ULL)
    Sleep(30);
  client.Uninitialize();
  const bool gotAck = g_skillAckDone.load();
  if (out)
    *out = g_skillAck;

  // 已经调用 SkillCall：不等固定时长，立即发送独立 StopSkill 包。
  const int stopRc = SkillStop();
  if (!gotAck) {
    LOGW("[GUI技能] 8 秒内没收到 SkillCall 回执；StopSkill rc=%d，检查 %s",
         stopRc, app::Config().LogPath().c_str());
    return 2;
  }
  const SkillAck &a = g_skillAck;
  LOGI("[GUI技能] 回执：%s(%d) ret=%d 额度剩 %u  ref %u->%u %s",
       SkillResultText(a.result), a.result, a.retValue, a.budgetLeft,
       a.usesBefore, a.usesAfter,
       (a.usesAfter == a.usesBefore) ? "(配平)" : "(未配平)");
  if (a.result == 0 && stopRc != 0) {
    LOGE("[GUI技能] SkillCall 成功，但立即 StopSkill 失败 rc=%d", stopRc);
    return 7;
  }
  switch (a.result) {
  case 0:
    return 0;
  case 6:
    return 3;
  case 8:
    return 4;
  case 5:
    return 5;
  default:
    return 6;
  }
}
// 裸走路 call 标定
//
// 只发一条 MT_WalkCall，Monitor 在游戏进程内真实调用一次走路 call，
// 把调用前后的角色坐标和距离带回来。这里不做任何判定、不重试、不去重。
//
// 标定用（GUI 常驻按钮），所以：
//   · 复用固定 Controller 长连接，每次操作单独分配 RequestId
//   · 回执超时给 3 秒：WalkTo 内部有熔断 Sleep（最小间隔 60ms），
//     正常几十毫秒就有回执
// ============================================================
namespace {

std::atomic_bool g_walkCallAckDone{ false };
WalkCallAck      g_walkCallAck{};

void OnWalkCallMessage(MessageType type, const void* data, size_t size)
{
    if (type != MT_WalkCallAck || !data || size < sizeof(WalkCallAck)) return;
    WalkCallAck ack{};
    memcpy(&ack, data, sizeof(ack));
    g_walkCallAck = ack;
    g_walkCallAckDone = true;
}

} // namespace

int WalkCallOnce(int32_t x, int32_t y, WalkCallAck* ac)
{
    static std::mutex walkRequestMutex;
    std::lock_guard<std::mutex> operation(walkRequestMutex);
    const app::AppConfig& cfg = app::Config();

    g_walkCallAckDone = false;
    g_walkCallAck = WalkCallAck{};

    LOGI("===== 裸走路 call 标定：目标 (%d,%d) =====", x, y);

    pipe::NamedPipeClie client(cfg.PipeFullName(), OnWalkCallMessage);
    if (!client.Initialize()) {
        LOGE("[WalkCall] 管道连不上（Monitor.dll 注入了吗？）");
        return 1;
    }

    WalkCallReq req{};
    req.x = x;
    req.y = y;

    WalkCallMessage msg(req);
    if (!client.SendPackage(&msg)) {
        LOGE("[WalkCall] 请求发送失败");
        client.Uninitialize();
        return 1;
    }

    const ULONGLONG t0 = GetTickCount64();
    while (!g_walkCallAckDone.load() && (GetTickCount64() - t0) < 3000ULL) {
        Sleep(10);
    }

    client.Uninitialize();

    if (ac) *ac = g_walkCallAck;

    if (!g_walkCallAckDone.load()) {
        LOGE("[WalkCall] 3 秒内没收到回执");
        return 4;
    }

    const WalkCallAck& a = g_walkCallAck;
    LOGI("[WalkCall] 回执：ok=%d result=%d 耗时%ums 角色(%d,%d) 距离 %ld -> %ld",
         a.ok, a.result, (unsigned)a.elapsedMs,
         a.playerX, a.playerY, (long)a.distBefore, (long)a.distAfter);

    if (a.ok == 1) return 2;   // 未就绪
    if (a.ok != 0) return 3;   // call 本身失败
    return 0;
}

} // namespace usecase
