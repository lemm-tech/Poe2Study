#pragma once
#include <cstdint>
#include <string>

namespace bossprobe {
// Only a match to this area's configured variety AND runtime boss evidence
// qualifies as an automatic destination. Metadata keywords never decide roles.
struct Evidence {
    bool monster = true;
    bool valid = true;
    bool configuredForArea = false;
    bool isCombatOwner = true;
    uint32_t rarity = 0;
    int32_t bossTier = 0;
    bool hpValid = false;
    uint32_t hp = 0;
    bool positionValid = false;
};

inline std::string Role(const Evidence& e) {
    if (!e.monster) return "monster_path_object";
    if (!e.valid) return "unknown";
    if (!e.isCombatOwner) return "shared_monster_proxy";
    if (e.configuredForArea)
        return e.rarity == 3 && e.bossTier > 0 ? "map_boss" : "map_boss_unconfirmed";
    if (e.bossTier > 0) return "other_boss";
    if (e.rarity == 3) return "unique_unconfirmed";
    if (e.rarity == 1 || e.rarity == 2) return "elite";
    return "ordinary";
}

inline bool Destination(const Evidence& e) {
    return Role(e) == "map_boss" && e.hpValid && e.hp > 0 && e.positionValid;
}

inline bool BossEntrance(const std::string& metadata) {
    return metadata == "Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame_Boss";
}
} // namespace bossprobe
