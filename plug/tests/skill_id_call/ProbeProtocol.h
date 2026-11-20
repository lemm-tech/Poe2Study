#pragma once
#include <cstdint>

namespace idprobe {
constexpr uint32_t kMagic = 0x44494B53;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxSamples = 192;
constexpr uint64_t kIdCallRva = 0x211ECD0;
constexpr uint64_t kPendingActionRva = 0x1DE1920;
constexpr uint64_t kManagerGetterRva = 0x16DF650;
constexpr uint64_t kActorSkillManager = 0x9C0;
constexpr uint64_t kManagerBegin = 0x148, kManagerEnd = 0x150, kManagerCapacity = 0x158;
constexpr uint32_t kMaxManagedSkills = 512;
constexpr uint8_t kManagerGetterSignature[] = { 0x48,0x8D,0x81,0xC0,0x09,0,0,0xC3 };
// Experimental offsets stay outside Common and all production projects.
constexpr uint64_t kContextActor = 0x198, kContextPlayer = 0x188;
constexpr uint64_t kPendingId = 0x214, kActiveAction = 0x248;
constexpr uint64_t kActionSkill = 0xF8;
constexpr uint64_t kLifeMana = 0x238, kLifeEs = 0x278;
constexpr uint8_t kIdCallSignature[] = {
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
    0x55,0x57,0x41,0x56,0x48,0x8B,0xEC,0x48,0x81,0xEC,0x80,0,0,0
};
#pragma pack(push, 1)
struct Sample {
    uint64_t tick;
    uint32_t hp, mana, es;
    int32_t x, y;
    uint32_t pendingId, activeId, phase;
};
struct Block {
    uint32_t magic, version, bytes, cast;
    uint32_t skillId;
    int32_t x, y;
    uint32_t flags, flagA, flagB, holdMs, observeMs;
    uint64_t expectedEntity, expectedContext;
    uint32_t status, exceptionCode, castReturned, stopReturned;
    uint64_t context, entity, actor, player, life, positioned;
    uint32_t sampleCount;
    char error[256];
    Sample samples[kMaxSamples];
};
#pragma pack(pop)
static_assert(sizeof(Sample) == 40, "Python wire layout must match");
static_assert(sizeof(Block) == 8068, "Python wire layout must match");
}
