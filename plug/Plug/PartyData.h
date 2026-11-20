#pragma once
#include "../Common/GameMemory.h"
#include <atomic>
#include <string>
#include <vector>

namespace poe {
enum class PartyMapStatus { Resolved, ProfileMissing, ProfileAmbiguous, AreaUnavailable };
struct PartyMember {
    std::string name, account, mapName, mapId;
    uint16_t areaKey = 0;
    PartyMapStatus mapStatus = PartyMapStatus::ProfileMissing;
    bool self = false;
    bool operator==(const PartyMember& other) const;
};
struct PartySnapshot {
    uint32_t pid = 0;
    uint64_t sampledAt = 0;
    std::string selfName;
    std::vector<PartyMember> members; // Roster only; profiles can contain other contacts.
};
// Requires a verified game session. Two consistent bounded reads, no Monitor needed.
// Clears out on failure; stop is optional and never writes to the target process.
bool ReadParty(const mem::GameMemory& memory, uint64_t moduleBase,
               PartySnapshot& out, std::string& error, const std::atomic_bool* stop = nullptr);
} // namespace poe
