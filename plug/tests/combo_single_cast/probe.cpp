#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../skill_id_call/ProbeProtocol.h"
#include "../../Common/GameBuild.h"
#include "../../Monitor/IdSkillCaster.h"

namespace {
using idprobe::Block;
template<class T> T Read(uint64_t address) {
    T value{}; SIZE_T done = 0;
    if (!IsPlausibleAddress(address) || !ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), &value, sizeof(value), &done) || done != sizeof(value))
        throw std::runtime_error("Unreadable diagnostic field");
    return value;
}
uint64_t Context(uint64_t base) {
    auto value = Read<uint64_t>(base + BaseOffset);
    for (auto off : {0x348ULL, 0x48ULL, 0ULL}) value = Read<uint64_t>(value + off);
    return value;
}
void Validate(const Block& b, uint64_t base) {
    if (Context(base) != b.context || Read<uint64_t>(b.context + 0x198) != b.actor ||
        Read<uint64_t>(b.context + 0x188) != b.player || Read<uint64_t>(b.actor + 8) != b.entity ||
        Read<uint64_t>(b.player + 8) != b.entity)
        throw std::runtime_error("Map/player changed; no call on old context");
}
void Capture(Block& b, uint32_t phase) {
    if (b.sampleCount >= idprobe::kMaxSamples) throw std::runtime_error("Sample capacity exhausted");
    auto& s = b.samples[b.sampleCount++];
    s.tick = GetTickCount64(); s.phase = phase;
    s.hp = Read<uint32_t>(b.life + kLife_HpOff);
    s.mana = Read<uint32_t>(b.life + idprobe::kLifeMana);
    s.es = Read<uint32_t>(b.life + idprobe::kLifeEs);
    s.x = Read<int32_t>(b.positioned + kPositioned_EntXOff);
    s.y = Read<int32_t>(b.positioned + kPositioned_EntYOff);
    s.pendingId = Read<uint32_t>(b.player + idprobe::kPendingId);
    s.activeId = 0; // Experimental Actor action offset is not a validated observation.
}
bool Stop(uint64_t base, uint64_t context) {
    __try { reinterpret_cast<void (*)(uint64_t)>(base + kSkillStopRva)(context); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Run(Block& b) {
    if (b.magic != idprobe::kMagic || b.version != idprobe::kVersion || b.bytes != sizeof(b) ||
        b.cast > 1 || !b.skillId || b.holdMs > 3000 || b.observeMs > 3500)
        throw std::runtime_error("Invalid single-cast probe request");
    const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    std::string error;
    if (!gamebuild::VerifyLocal(base, error)) throw std::runtime_error(error);
    b.context = b.expectedContext; b.entity = b.expectedEntity;
    Validate(b, base); Capture(b, 0); Sleep(200); Validate(b, base); Capture(b, 0);
    if (!b.samples[0].hp || !b.samples[1].hp || b.samples[0].pendingId || b.samples[1].pendingId ||
        b.samples[0].x != b.samples[1].x || b.samples[0].y != b.samples[1].y)
        throw std::runtime_error("Player must be alive, idle and stationary before probe");
    if (!b.cast) { b.status = 1; return; }
    navskill::IdSkillCaster caster;
    managed::Route route{1, 1, 1};
    combo::CastRequest request{};
    if (b.skillId == kDodgeRollSkillId) request.operation = combo::Operation::DodgeRoll;
    request.token = GetTickCount64(); request.expectedEntity = b.entity;
    request.expectedWorld = Read<uint64_t>(b.entity + 0x78);
    request.skillId = b.skillId; request.chargeMs = b.holdMs; request.x = b.x; request.y = b.y;
    bool attempted = false;
    try {
        attempted = true;
        const auto begun = caster.Handle(request, route);
        b.castReturned = begun.invoked; b.exceptionCode = begun.exceptionCode;
        if (begun.result != combo::Result::Ok) throw std::runtime_error(begun.detail);
        Capture(b, 1);
        while (caster.Active() && GetTickCount64() < begun.pressedAt + b.holdMs) {
            Sleep(40); Validate(b, base); caster.Tick(true); Capture(b, 1);
        }
        request.operation = combo::Operation::Release;
        const auto released = caster.Handle(request, route);
        b.stopReturned = released.invoked; b.exceptionCode = released.exceptionCode;
        std::snprintf(b.error, sizeof(b.error), "begin=%s; release=%s", begun.detail, released.detail);
        if (released.result != combo::Result::Ok) throw std::runtime_error(released.detail);
        const auto stoppedAt = GetTickCount64();
        do {
            Validate(b, base); Capture(b, 2); Sleep(40);
        } while (GetTickCount64() - stoppedAt < b.observeMs);
        // Explicit final stop is required even when the OLD implementation falsely
        // reports a completed token release. Never unload while leaving a cast held.
        Validate(b, base); caster.Cancel();
        if (b.skillId != kDodgeRollSkillId && !Stop(base, b.context))
            throw std::runtime_error("Final cleanup stop failed");
        attempted = false;
        Sleep(200); Validate(b, base); Capture(b, 3);
        b.status = 1;
    } catch (...) {
        caster.Cancel();
        if (attempted && Context(base) == b.context) Stop(base, b.context);
        throw;
    }
}
}
extern "C" __declspec(dllexport) DWORD WINAPI ProbeRun(void* parameter) {
    auto& b = *static_cast<Block*>(parameter);
    try { Run(b); }
    catch (const std::exception& e) {
        b.status = 2; std::snprintf(b.error, sizeof(b.error), "%s", e.what());
    }
    return b.status;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
