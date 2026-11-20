#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../Common/GameMemory.h"

namespace poe {
struct SkillSlot {
    uint32_t slotId = 0; // Zero-based vector index; repeated bindings remain separate.
    bool occupied = false;
    uint32_t skillId = 0;
    std::string internalId;
    std::string localizedName;
    bool movement = false; // Built-in Move: display only, excluded from skill combos.
};
struct SkillSlotsSnapshot {
    uint32_t pid = 0;
    uint64_t playerEntity = 0;
    std::string characterName;
    std::vector<SkillSlot> slots;
};
// Requires a version-verified session. Two consistent reads, no calls or writes.
bool ReadCharacterName(const mem::GameMemory& memory, uint64_t moduleBase,
                       std::string& name, std::string& err);
bool ReadSkillSlots(const mem::GameMemory& memory, uint64_t moduleBase,
                   SkillSlotsSnapshot& out, std::string& err);
std::string FormatSkillId(uint32_t skillId);
std::string SkillSlotsToTsv(const SkillSlotsSnapshot& snapshot);
}
