#pragma once
#include "../Common/GameMemory.h"
#include <string>

namespace poe {
struct WorldAreaSnapshot {
    uint64_t world = 0, mapOwner = 0, record = 0;
    std::string id, name;
    bool Available() const { return !id.empty() && !name.empty(); }
    bool SameMap(const WorldAreaSnapshot& other) const {
        return Available() && other.Available() && world == other.world &&
            mapOwner == other.mapOwner && record == other.record && id == other.id;
    }
};
// Call with a verified game module/session. Reads only, and clears out on failure.
bool ReadWorldArea(const mem::GameMemory& memory, uint64_t moduleBase,
                   WorldAreaSnapshot& out, std::string& error);
} // namespace poe
