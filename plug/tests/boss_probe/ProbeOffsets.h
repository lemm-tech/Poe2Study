#pragma once
#include <cstdint>

// Experimental, calibrated by read-only sampling on 2026-11-19.
// Deliberately kept out of Common/GameOffsets.h and all production projects.
namespace bossprobe {
constexpr uint64_t kWorldBossCount = 0x9C;
constexpr uint64_t kWorldBossData = 0xA4;
constexpr uint64_t kMonsterVtable = 0x34DF1E8;
constexpr uint64_t kMagicVtable = 0x34E7BB8;
constexpr uint64_t kMonsterData = 0x18;
constexpr uint64_t kVarietyRow = 0x10;
constexpr uint64_t kVarietyTable = 0x18;
constexpr uint64_t kVarietyName = 0x110;
constexpr uint64_t kMagicRarity = 0x144;
constexpr uint64_t kRuntimeMods = 0x150;
constexpr uint64_t kRuntimeModStride = 0x40;
constexpr uint64_t kRuntimeModRow = 0x28;
constexpr uint64_t kRuntimeModTable = 0x30;
constexpr uint64_t kModStatRefs = 0x1E;
constexpr uint64_t kReferenceStride = 0x10;
constexpr unsigned kModStatSlots = 4;
} // namespace bossprobe
