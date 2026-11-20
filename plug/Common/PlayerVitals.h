#pragma once
#include "GameOffsets.h"
#include "Recovery.h"
#include <cstring>

// The same bounded character component traversal works with GameMemory and RPM
// inside Monitor. Failed reads never become zero-valued resource samples.
namespace recovery {
struct Vitals {
    uint64_t entity = 0, world = 0, life = 0;
    uint32_t current[kResources]{}, maximum[kResources]{};
};
template<class Read>
bool ReadVitals(uint64_t base, Read read, Vitals& out) {
    out = {};
    auto pointer = [&](uint64_t slot, uint64_t& value) {
        return IsPlausibleAddress(slot) && read(slot, &value, sizeof(value)) && IsPlausibleAddress(value);
    };
    auto player = [&](uint64_t& entity, uint64_t& world) {
        if (!pointer(base + BaseOffset, entity)) return false;
        for (int i = 0; i < kLocalPlayerChainDepth; ++i) {
            if (!pointer(entity + kMapObjectChain[i], entity)) return false;
            if (i + 1 == kWorldAreaWorldChainDepth) world = entity;
        }
        return true;
    };
    Vitals result;
    if (!player(result.entity, result.world)) return false;
    uint64_t header = 0, pairs = 0, end = 0, array = 0, arrayEnd = 0;
    if (!pointer(result.entity + kEntityPairsOff0, header) || !pointer(header + kEntityPairsOff1, header) ||
        !pointer(header + kEntityPairsOff2, pairs) || !pointer(header + kEntityPairsEndOff, end) ||
        !pointer(result.entity + kEntityComponentsOff, array) ||
        !pointer(result.entity + kEntityComponentsEndOff, arrayEnd) || end <= pairs || end - pairs > 0x4000 ||
        (end - pairs) % kComponentPairSize || arrayEnd < array ||
        arrayEnd - array != (end - pairs) / kComponentPairSize * kComponentPtrSize) return false;
    const auto count = (end - pairs) / kComponentPairSize;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t name = 0; uint32_t index = 0; char text[5]{};
        if (!pointer(pairs + i * kComponentPairSize, name) || !read(name, text, sizeof(text)) ||
            !read(pairs + i * kComponentPairSize + kComponentPairIndexOff, &index, sizeof(index)) || index >= count) return false;
        if (std::memcmp(text, "Life", sizeof(text)) == 0) {
            if (result.life || !pointer(array + index * kComponentPtrSize, result.life)) return false;
        }
    }
    uint64_t owner = 0, vtable = 0;
    if (!result.life || !pointer(result.life + skilllayout::kComponentOwnerEntity, owner) || owner != result.entity ||
        !pointer(result.life, vtable) || vtable != base + kLifeVtableRva) return false;
    for (size_t i = 0; i < kResources; ++i) {
        const auto resource = result.life + kLifeResourceOff[i];
        if (!pointer(resource, vtable) || vtable != base + kLifeResourceVtableRva[i] ||
            !pointer(resource + kLifeResourceOwnerOff, owner) || owner != result.life) return false;
        uint32_t values[2]{}, maximumAgain = 0;
        if (!read(resource + kLifeResourceMaximumOff, values, sizeof(values)) ||
            !read(resource + kLifeResourceMaximumOff, &maximumAgain, sizeof(maximumAgain)) ||
            maximumAgain != values[0] || values[0] > 100000000 || values[1] > values[0]) return false;
        result.maximum[i] = values[0]; result.current[i] = values[1];
    }
    uint64_t entityAgain = 0, worldAgain = 0, lifeAgain = 0, pairsAgain = 0, arrayAgain = 0;
    if (!result.maximum[0] || !player(entityAgain, worldAgain) || entityAgain != result.entity || worldAgain != result.world ||
        !pointer(header + kEntityPairsOff2, pairsAgain) || pairsAgain != pairs ||
        !pointer(result.entity + kEntityComponentsOff, arrayAgain) || arrayAgain != array ||
        !pointer(result.life + skilllayout::kComponentOwnerEntity, lifeAgain) || lifeAgain != result.entity) return false;
    out = result;
    return true;
}
} // namespace recovery
