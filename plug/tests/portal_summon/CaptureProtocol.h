#pragma once
#include <cstdint>
namespace portalprobe {
constexpr uint32_t kMagic = 0x50525443, kVersion = 1, kBatch = 32;
#pragma pack(push, 1)
struct Event {
    uint64_t sequence, tick, buildSequence, caller, rcx, rdx, r8, r9;
    uint64_t vtable, serializer, world, entity;
    uint32_t kind, threadId, opcode, objectLength, dataLength, originalLength, stackCount, flags;
    int32_t x, y;
    uint64_t stack[24];
    uint8_t object[256], data[1024];
};
struct Block {
    uint32_t magic, version, bytes, operation, status, exceptionCode, eventCount, hooksActive;
    uint64_t state, protocol, stream, world, entity, positioned;
    uint64_t totalEvents, dropped;
    Event events[kBatch];
};
#pragma pack(pop)
static_assert(sizeof(Event) == 1608 && sizeof(Block) == 51552, "portal capture wire layout");
} // namespace portalprobe
