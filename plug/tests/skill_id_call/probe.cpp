#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include "../../Common/GameBuild.h"
#include "ProbeProtocol.h"

namespace {
using namespace idprobe;
bool ReadInto(uint64_t address, void* out, size_t size) {
    SIZE_T done = 0;
    return IsPlausibleAddress(address) && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address), out, size, &done) && done == size;
}
template<class T> T Read(uint64_t address) {
    T value{};
    if (!ReadInto(address, &value, sizeof(value))) throw std::runtime_error("Unreadable context/character field");
    return value;
}
uint64_t Pointer(uint64_t address) {
    auto value = Read<uint64_t>(address);
    if (!IsPlausibleAddress(value)) throw std::runtime_error("Invalid context/character pointer");
    return value;
}
uint32_t OptionalId(uint64_t actor) {
    uint64_t action = 0, skill = 0;
    uint32_t id = 0;
    if (ReadInto(actor + kActiveAction, &action, 8) && action &&
        ReadInto(action + kActionSkill, &skill, 8) && skill)
        ReadInto(skill + skilllayout::kRuntimeSkillId, &id, 4);
    return id;
}
uint64_t Component(uint64_t entity, const char* wanted) {
    auto header = Pointer(Pointer(entity + kEntityPairsOff0) + kEntityPairsOff1);
    auto begin = Pointer(header + kEntityPairsOff2);
    auto end = Read<uint64_t>(header + kEntityPairsEndOff);
    auto array = Pointer(entity + kEntityComponentsOff);
    auto arrayEnd = Read<uint64_t>(entity + kEntityComponentsEndOff);
    if (end <= begin || end - begin > 128 * kComponentPairSize ||
        (end - begin) % kComponentPairSize || arrayEnd < array ||
        arrayEnd - array != (end - begin) / kComponentPairSize * kComponentPtrSize)
        throw std::runtime_error("Invalid component table");
    for (uint64_t pair = begin; pair < end; pair += kComponentPairSize) {
        auto name = Pointer(pair);
        char text[96]{};
        bool terminated = false;
        for (size_t i = 0; i + 1 < sizeof(text); ++i) {
            text[i] = Read<char>(name + i);
            if (!text[i]) { terminated = true; break; }
        }
        if (!terminated) throw std::runtime_error("Invalid component name");
        auto index = Read<uint32_t>(pair + kComponentPairIndexOff);
        if (index >= (end - begin) / kComponentPairSize)
            throw std::runtime_error("Invalid component index");
        if (std::strcmp(text, wanted) == 0) return Pointer(array + index * kComponentPtrSize);
    }
    throw std::runtime_error(std::string("Missing component: ") + wanted);
}
uint64_t Context(uint64_t base) {
    auto value = Pointer(base + BaseOffset);
    for (auto off : {0x348ULL, 0x48ULL, 0ULL}) value = Pointer(value + off);
    return value;
}
void ValidateIdentity(Block& b, uint64_t base) {
    if (Context(base) != b.context || Pointer(b.context + kContextActor) != b.actor ||
        Pointer(b.context + kContextPlayer) != b.player ||
        Pointer(b.actor + 8) != b.entity || Pointer(b.player + 8) != b.entity)
        throw std::runtime_error("Context/character changed; no replay");
}
void ValidateSkill(const Block& b, uint64_t base) {
    if (Pointer(Pointer(b.actor) + 0x168) != base + kManagerGetterRva)
        throw std::runtime_error("Actor skill-manager getter changed");
    uint8_t bytes[sizeof(kManagerGetterSignature)]{};
    if (!ReadInto(base + kManagerGetterRva, bytes, sizeof(bytes)) ||
        std::memcmp(bytes, kManagerGetterSignature, sizeof(bytes)))
        throw std::runtime_error("Skill-manager getter fingerprint mismatch");
    const auto manager = b.actor + kActorSkillManager;
    if (Pointer(manager + 0x140) != b.entity)
        throw std::runtime_error("Skill manager belongs to another entity");
    const auto begin = Pointer(manager + kManagerBegin);
    const auto end = Read<uint64_t>(manager + kManagerEnd);
    const auto capacity = Read<uint64_t>(manager + kManagerCapacity);
    if (end <= begin || end - begin > kMaxManagedSkills * 16ULL || (end - begin) % 16 ||
        capacity < end || capacity - begin > kMaxManagedSkills * 16ULL || (capacity - begin) % 16)
        throw std::runtime_error("Invalid actor skill vector");
    uint32_t previous = 0;
    bool found = false;
    for (auto entry = begin; entry < end; entry += 16) {
        const auto skill = Pointer(entry);
        const auto id = Read<uint32_t>(skill + skilllayout::kRuntimeSkillId);
        if (!id || (entry != begin && id <= previous) || Pointer(skill + 0xB8) != manager)
            throw std::runtime_error("Skill vector changed or has invalid ownership/order");
        previous = id;
        if (id == b.skillId) found = true;
    }
    if (Read<uint64_t>(manager + kManagerBegin) != begin || Read<uint64_t>(manager + kManagerEnd) != end ||
        Read<uint64_t>(manager + kManagerCapacity) != capacity)
        throw std::runtime_error("Skill vector changed during validation");
    if (!found) throw std::runtime_error("ID no longer exists in actor skill manager; no call attempted");
}
void ValidateTarget(const Block& b) {
    const auto map = Pointer(b.entity + 0x78) + 0x8F8; // Embedded object: tail ADD, not dereference.
    const auto width = Read<uint32_t>(map + MAP_OFF_GRID_W);
    const auto height = Read<uint32_t>(map + MAP_OFF_GRID_H);
    if (!width || !height || width > kMaxGridDimension || height > kMaxGridDimension ||
        b.x < 0 || b.y < 0 || static_cast<uint32_t>(b.x) >= width * CELL_SIZE ||
        static_cast<uint32_t>(b.y) >= height * CELL_SIZE)
        throw std::runtime_error("Target coordinates outside current map");
}
void Capture(Block& b, uint32_t phase) {
    if (b.sampleCount >= kMaxSamples) throw std::runtime_error("Sample capacity exhausted");
    auto& s = b.samples[b.sampleCount++];
    s.tick = GetTickCount64();
    s.hp = Read<uint32_t>(b.life + kLife_HpOff);
    s.mana = Read<uint32_t>(b.life + kLifeMana);
    s.es = Read<uint32_t>(b.life + kLifeEs);
    s.x = Read<int32_t>(b.positioned + kPositioned_EntXOff);
    s.y = Read<int32_t>(b.positioned + kPositioned_EntYOff);
    s.pendingId = Read<uint32_t>(b.player + kPendingId);
    s.activeId = OptionalId(b.actor);
    s.phase = phase;
}
struct Position { int32_t x, y; };
// Seven arguments, matching the live caller at RVA 0x18577E..0x1857A7.
// No slot index, skill pointer or shared_ptr enters this function.
using CastFn = void (*)(uint64_t, uint32_t, uint32_t, Position*, uint16_t, uint8_t, uint8_t);
bool InvokeCast(uint64_t entry, Block* b, DWORD* exception) {
    Position pos{b->x, b->y};
    __try {
        reinterpret_cast<CastFn>(entry)(b->context, b->skillId, 0, &pos,
            static_cast<uint16_t>(b->flags), static_cast<uint8_t>(b->flagA),
            static_cast<uint8_t>(b->flagB));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *exception = GetExceptionCode();
        return false;
    }
}
bool InvokeStop(uint64_t entry, uint64_t context, DWORD* exception) {
    __try { reinterpret_cast<void (*)(uint64_t)>(entry)(context); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { *exception = GetExceptionCode(); return false; }
}
void Run(Block& b) {
    if (b.magic != kMagic || b.version != kVersion || b.bytes != sizeof(Block) || b.cast > 1 ||
        !b.skillId || (b.flags != 0x400 && b.flags != 0x401) || b.flagA || b.flagB ||
        b.holdMs > 1800 || b.observeMs > 1500 || b.x < 0 || b.y < 0)
        throw std::runtime_error("Invalid probe request");
    const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    std::string error;
    if (!gamebuild::VerifyLocal(base, error)) throw std::runtime_error(error);
    uint8_t bytes[sizeof(kIdCallSignature)]{};
    if (!ReadInto(base + kIdCallRva, bytes, sizeof(bytes)) ||
        std::memcmp(bytes, kIdCallSignature, sizeof(bytes)))
        throw std::runtime_error("ID call fingerprint mismatch");
    b.context = Context(base);
    b.actor = Pointer(b.context + kContextActor);
    b.player = Pointer(b.context + kContextPlayer);
    b.entity = Pointer(b.actor + 8);
    if (b.entity != b.expectedEntity || b.context != b.expectedContext)
        throw std::runtime_error("Preflight context does not match request");
    b.life = Component(b.entity, "Life");
    b.positioned = Component(b.entity, "Positioned");
    if (Pointer(Pointer(b.player) + 0x108) != base + kPendingActionRva)
        throw std::runtime_error("Pending-action vtable fingerprint mismatch");
    if (Read<uint8_t>(b.context + 0x7B10) || Read<uint8_t>(b.context + 0x8C08))
        throw std::runtime_error("ID call is gated by the current session");
    ValidateIdentity(b, base);
    ValidateSkill(b, base);
    ValidateTarget(b);
    Capture(b, 0);
    Sleep(200);
    ValidateIdentity(b, base);
    Capture(b, 0);
    if (b.samples[0].x != b.samples[1].x || b.samples[0].y != b.samples[1].y)
        throw std::runtime_error("Player moved during preflight; stop navigation first");
    if (!b.samples[0].hp || !b.samples[1].hp || b.samples[0].pendingId || b.samples[1].pendingId)
        throw std::runtime_error("Player must be alive and idle before the test");
    if (!b.cast) { b.status = 1; return; }
    ValidateSkill(b, base);
    ValidateTarget(b);
    if (Read<uint8_t>(b.context + 0x7B10) || Read<uint8_t>(b.context + 0x8C08))
        throw std::runtime_error("Session gated before cast; no call attempted");
    DWORD exception = 0;
    if (!InvokeCast(base + kIdCallRva, &b, &exception)) {
        b.exceptionCode = exception;
        throw std::runtime_error("ID call raised SEH; no automatic retry");
    }
    b.castReturned = 1;
    const auto calledAt = GetTickCount64();
    // Cleanup also runs if observation fails after a completed cast.
    try {
        do {
            ValidateIdentity(b, base);
            Capture(b, 1);
            Sleep(20);
        } while (GetTickCount64() - calledAt < b.holdMs);
    } catch (...) {
        if (Context(base) == b.context)
            b.stopReturned = InvokeStop(base + kSkillStopRva, b.context, &exception) ? 1U : 0U;
        throw;
    }
    ValidateIdentity(b, base);
    b.stopReturned = InvokeStop(base + kSkillStopRva, b.context, &exception) ? 1U : 0U;
    if (!b.stopReturned) {
        b.exceptionCode = exception;
        throw std::runtime_error("Stop call raised SEH; no automatic retry");
    }
    const auto stoppedAt = GetTickCount64();
    do {
        ValidateIdentity(b, base);
        Capture(b, 2);
        Sleep(20);
    } while (GetTickCount64() - stoppedAt < b.observeMs);
    b.status = 1; // Transport and observation complete; NOT a gameplay success claim.
}
}

extern "C" __declspec(dllexport) DWORD WINAPI ProbeRun(void* parameter) {
    auto& b = *static_cast<idprobe::Block*>(parameter);
    try { Run(b); }
    catch (const std::exception& e) {
        b.status = 2;
        std::snprintf(b.error, sizeof(b.error), "%s", e.what());
    }
    return b.status;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
