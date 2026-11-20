#pragma once
#include "GameOffsets.h"
#include "PacketProtocol.h"
#include <cmath>
#include <string>
#include <vector>

namespace portalreturn {
constexpr const char* kMetadata = "Metadata/MiscellaneousObjects/MultiplexPortal";
constexpr double kNearby = 120.0;
constexpr double kEnterDistance = 18.0;
inline bool Hideout(const std::string& name) { return name.find("藏身處") != std::string::npos; }
struct Identity {
    uint32_t key = 0;
    uint64_t entity = 0;
    explicit operator bool() const { return key && entity; }
    bool operator==(const Identity& other) const { return key == other.key && entity == other.entity; }
};
template<class Entity> Identity Id(const Entity& entity) { return {entity.key, entity.entity}; }
struct Ledger {
    bool summonSubmitted = false, enterSubmitted = false, arrived = false;
    int32_t originX = 0, originY = 0;
    std::vector<Identity> before;
    Identity target;
    bool CanSummon() const { return !summonSubmitted && !enterSubmitted && !arrived; }
    bool CanEnter() const { return summonSubmitted && bool(target) && !enterSubmitted && !arrived; }
};
template<class Entity> bool IsHideoutPortal(const Entity& entity) {
    return bool(Id(entity)) && entity.metadataPath == kMetadata && Hideout(entity.name) && entity.x >= 0 && entity.y >= 0;
}
inline double Distance(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    return std::hypot(double(ax) - bx, double(ay) - by);
}
template<class Entity>
const Entity* SelectNew(const std::vector<Entity>& portals, const Ledger& ledger, int32_t x, int32_t y) {
    if (!ledger.summonSubmitted || x < 0 || y < 0) return nullptr;
    const Entity* best = nullptr;
    double distance = kNearby + 1;
    bool tied = false;
    for (const auto& portal : portals) {
        if (!IsHideoutPortal(portal)) continue;
        bool old = false;
        for (const auto& id : ledger.before) if (id == Id(portal)) { old = true; break; }
        if (old || Distance(portal.x, portal.y, ledger.originX, ledger.originY) > kNearby) continue;
        const auto d = Distance(x, y, portal.x, portal.y);
        if (d > kNearby) continue;
        if (!best || d < distance) { best = &portal; distance = d; tied = false; }
        else if (d == distance) tied = true;
    }
    return tied ? nullptr : best;
}

// Read the same source-map identity in GUI and Monitor, including the local
// player back-reference. Failed/partial reads cannot authorize an action.
template<class Read> bool ReadScene(uint64_t base, Read read, packet::PortalScene& out) {
    out = {};
    auto pointer = [&](uint64_t slot, uint64_t& value) {
        return IsPlausibleAddress(slot) && read(slot, &value, sizeof(value)) && IsPlausibleAddress(value);
    };
    auto once = [&](packet::PortalScene& scene) {
        uint64_t p = 0;
        if (!pointer(base + BaseOffset, p)) return false;
        for (int i = 0; i <= kLocalPlayerChainDepth; ++i) {
            if (!pointer(p + kMapObjectChain[i], p)) return false;
            if (i + 1 == kWorldAreaWorldChainDepth) scene.world = p;
            if (i + 1 == kLocalPlayerChainDepth) scene.player = p;
        }
        scene.mapOwner = p;
        return scene.world == scene.mapOwner && pointer(scene.world + kWorldAreaRecordOff, scene.record);
    };
    packet::PortalScene first, second;
    if (!once(first) || !once(second) || !(first == second)) return false;
    out = first; return true;
}
inline bool ValidScene(const packet::PortalScene& scene) {
    return IsPlausibleAddress(scene.world) && scene.world == scene.mapOwner &&
        IsPlausibleAddress(scene.record) && IsPlausibleAddress(scene.player);
}
inline bool SameMap(const packet::PortalScene& a, const packet::PortalScene& b) {
    return a.world == b.world && a.mapOwner == b.mapOwner && a.record == b.record;
}
inline bool ValidSummonRequest(const packet::Request& request) {
    if (request.operation != packet::Operation::SummonPortal || request.length != sizeof(packet::PortalScene) ||
        request.itemKey || request.expectedEntity || request.captureMask || request.actionId || request.x || request.y ||
        (request.options & ~packet::PreviewOnly)) return false;
    packet::PortalScene scene;
    std::memcpy(&scene, request.bytes, sizeof(scene));
    return ValidScene(scene);
}

// Arrival needs a different, readable map, a living local player, and a stable
// hideout sample. Neither a reply nor a loading-screen read failure is arrival.
struct Arrival {
    packet::PortalScene candidate;
    uint64_t since = 0;
    bool Observe(const packet::PortalScene& source, const packet::PortalScene& scene,
                 const std::string& name, bool alive, uint64_t now) {
        if (!ValidScene(scene) || SameMap(scene, source) || !alive || !Hideout(name)) { candidate = {}; since = 0; return false; }
        if (!(candidate == scene)) { candidate = scene; since = now; return false; }
        return now >= since && now - since >= 500;
    }
};
} // namespace portalreturn
