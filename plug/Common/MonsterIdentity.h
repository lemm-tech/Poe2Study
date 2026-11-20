#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace monster {
struct Evidence {
    bool monster = true, valid = true, configuredForArea = false, isCombatOwner = true;
    uint32_t rarity = 0;
    int32_t bossTier = 0;
    bool hpValid = false;
    uint32_t hp = 0;
    bool positionValid = false;
};
inline std::string Canonical(std::string path) {
    const auto at = path.find_last_of('@');
    if (at != std::string::npos && at + 1 < path.size() &&
        std::all_of(path.begin() + at + 1, path.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        path.resize(at);
    return path;
}
inline std::string Role(const Evidence& e) {
    if (!e.monster) return "monster_path_object";
    if (!e.valid) return "unknown";
    if (!e.isCombatOwner) return "shared_monster_proxy";
    if (e.configuredForArea) return e.rarity == 3 && e.bossTier > 0 ? "map_boss" : "map_boss_unconfirmed";
    if (e.bossTier > 0) return "other_boss";
    if (e.rarity == 3) return "unique_unconfirmed";
    if (e.rarity == 1 || e.rarity == 2) return "elite";
    return "ordinary";
}
inline bool MapBoss(const Evidence& e) { return Role(e) == "map_boss"; }
inline bool OptionalElite(const Evidence& e) {
    // Include additional unique/boss encounters (e.g. an extra content boss).
    // An unconfirmed configured map boss must never enter the optional phase.
    return e.monster && e.valid && e.isCombatOwner && !e.configuredForArea && e.rarity > 0 && e.rarity <= 3;
}
inline bool Destination(const Evidence& e) {
    return MapBoss(e) && e.hpValid && e.hp > 0 && e.positionValid;
}
inline bool ConfirmedDead(const Evidence& e) {
    return e.monster && e.valid && e.isCombatOwner && e.hpValid && e.hp == 0;
}
inline bool BossEntrance(const std::string& path) {
    return path == "Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame_Boss";
}
} // namespace monster
