#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <string>
#include "CaptureProtocol.h"
#include "ProbeOffsets.h"
#include "../../Common/GameBuild.h"

namespace {
bool Read(uint64_t address, void* out, size_t size) {
    SIZE_T done = 0;
    return IsPlausibleAddress(address) && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), out, size, &done) && done == size;
}
template<class T> T Number(uint64_t address) {
    T value{}; Read(address, &value, sizeof(value)); return value;
}
bool CallOnce(uint64_t entry, uint64_t state, uint32_t& exception) {
    __try { reinterpret_cast<void (*)(uint64_t)>(entry)(state); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); return false; }
}
uint32_t Run(portalprobe::Block& block) {
    block.hooksActive = 0; block.eventCount = 0; block.totalEvents = 0;
    if (block.magic != portalprobe::kMagic || block.version != portalprobe::kVersion ||
        block.bytes != sizeof(block) || (block.operation != 0 && block.operation != 4)) return 10;
    const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    std::string error;
    if (!gamebuild::VerifyLocal(base, error)) return 11;
    uint8_t code[sizeof(portalprobe::kSummonCode)]{};
    if (!Read(base + portalprobe::kSummonRva, code, sizeof(code)) ||
        std::memcmp(code, portalprobe::kSummonCode, sizeof(code))) return 12;
    auto state = Number<uint64_t>(base + BaseOffset);
    for (const auto offset : kPacketContextChain) state = Number<uint64_t>(state + offset);
    const auto protocol = Number<uint64_t>(state + kPacketProtocolOff);
    const auto world = Number<uint64_t>(protocol + kPacketWorldOff);
    const auto playerHolder = Number<uint64_t>(world + 0x5C0);
    const auto player = Number<uint64_t>(state + kSkillStopObj188);
    if (state != block.state || protocol != block.protocol || world != block.world ||
        Number<uint64_t>(protocol + kPacketStreamOff) != block.stream ||
        Number<uint64_t>(playerHolder) != block.entity ||
        Number<uint64_t>(player + skilllayout::kComponentOwnerEntity) != block.entity ||
        Number<uint64_t>(block.positioned + skilllayout::kComponentOwnerEntity) != block.entity) return 13;
    uint8_t gate = 0xFF;
    uint32_t pending = UINT32_MAX;
    if (!Read(state + portalprobe::kSummonGateOff, &gate, sizeof(gate)) || gate ||
        !Read(player + kIdSkillCurrentIdOff, &pending, sizeof(pending)) || pending) return 14;
    if (block.operation == 0) return 0;
    // The native function retains its own stat and state checks, creates its own
    // packet object, reads the current sequence, and calls the native build/send.
    block.events[0].tick = GetTickCount64();
    block.events[0].kind = 3;
    block.events[0].rcx = state;
    block.events[0].caller = base + portalprobe::kSummonRva;
    block.totalEvents = 1;  // Attempted once; no implicit retry or fallback send.
    if (!CallOnce(base + portalprobe::kSummonRva, state, block.exceptionCode)) return 15;
    block.eventCount = 1;
    return 0;
}
}
extern "C" __declspec(dllexport) DWORD WINAPI ProbeRun(void* parameter) {
    auto& block = *static_cast<portalprobe::Block*>(parameter);
    uint32_t code = 0;
    try { code = Run(block); } catch (...) { code = 99; }
    block.status = code ? 2u : 1u;
    if (code && !block.exceptionCode) block.exceptionCode = code;
    return code;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
