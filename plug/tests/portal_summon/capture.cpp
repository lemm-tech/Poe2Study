#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include "CaptureProtocol.h"
#include "../../Common/GameBuild.h"
#include "../../third_party/minhook/include/MinHook.h"

namespace {
using portalprobe::Event;
using portalprobe::Block;
using BuildFn = uint64_t (*)(uint64_t, void*);
using WriteFn = uint64_t (*)(uint64_t, void*, uint64_t);
BuildFn originalBuild = nullptr;
WriteFn originalWrite = nullptr;
uint64_t base = 0, watchedState = 0, watchedProtocol = 0, watchedStream = 0;
uint64_t watchedWorld = 0, watchedEntity = 0, watchedPositioned = 0;
std::atomic<bool> enabled{false};
std::atomic<uint32_t> callbacks{0};
std::atomic<uint64_t> sequence{0}, dropped{0};
bool buildCreated = false, writeCreated = false, initialized = false;
SRWLOCK queueLock = SRWLOCK_INIT;
constexpr uint32_t kCapacity = 512;
Event queue[kCapacity]{};
uint32_t queueBegin = 0, queueCount = 0;
thread_local uint64_t currentBuild = 0;
thread_local uint32_t currentOpcode = 0;

bool Read(uint64_t address, void* out, size_t size) noexcept {
    SIZE_T done = 0;
    return IsPlausibleAddress(address) && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), out, size, &done) && done == size;
}
template<class T> T Number(uint64_t address) noexcept {
    T value{};
    Read(address, &value, sizeof(value));
    return value;
}
uint64_t State() noexcept {
    auto address = Number<uint64_t>(base + BaseOffset);
    for (const auto offset : kPacketContextChain) address = Number<uint64_t>(address + offset);
    return address;
}
bool SameContext() noexcept {
    const auto state = State();
    const auto protocol = Number<uint64_t>(state + kPacketProtocolOff);
    const auto world = Number<uint64_t>(protocol + kPacketWorldOff);
    const auto playerHolder = Number<uint64_t>(world + 0x5C0);
    return state == watchedState && protocol == watchedProtocol && world == watchedWorld &&
        Number<uint64_t>(protocol + kPacketStreamOff) == watchedStream &&
        Number<uint64_t>(playerHolder) == watchedEntity &&
        Number<uint64_t>(watchedPositioned + skilllayout::kComponentOwnerEntity) == watchedEntity;
}
void Push(const Event& event) noexcept {
    if (!TryAcquireSRWLockExclusive(&queueLock)) { ++dropped; return; }
    if (queueCount < kCapacity) {
        queue[(queueBegin + queueCount) % kCapacity] = event;
        ++queueCount;
    } else ++dropped;
    ReleaseSRWLockExclusive(&queueLock);
}
void BeginEvent(Event& event, uint32_t kind, uint64_t caller) noexcept {
    event.sequence = ++sequence;
    event.tick = GetTickCount64();
    event.kind = kind;
    event.threadId = GetCurrentThreadId();
    event.caller = caller;
    event.world = watchedWorld;
    event.entity = watchedEntity;
    event.x = Number<int32_t>(watchedPositioned + kPositioned_EntXOff);
    event.y = Number<int32_t>(watchedPositioned + kPositioned_EntYOff);
    void* frames[24]{};
    event.stackCount = CaptureStackBackTrace(1, 24, frames, nullptr);
    for (uint32_t i = 0; i < event.stackCount; ++i) event.stack[i] = reinterpret_cast<uint64_t>(frames[i]);
}
uint64_t BuildHook(uint64_t protocol, void* object) {
    ++callbacks;
    const auto oldBuild = currentBuild;
    const auto oldOpcode = currentOpcode;
    uint64_t result = 0;
    __try {
        if (enabled.load() && protocol == watchedProtocol && SameContext()) {
            Event event{};
            BeginEvent(event, 1, reinterpret_cast<uint64_t>(_ReturnAddress()));
            event.rcx = protocol; event.rdx = reinterpret_cast<uint64_t>(object);
            event.buildSequence = event.sequence;
            for (const uint32_t length : {256u, 128u, 64u, 32u, 16u}) {
                if (Read(event.rdx, event.object, length)) { event.objectLength = length; break; }
            }
            if (event.objectLength >= 16) {
                std::memcpy(&event.vtable, event.object, 8);
                uint16_t opcode = 0;
                std::memcpy(&opcode, event.object + 8, 2);
                event.opcode = opcode;
                event.serializer = Number<uint64_t>(event.vtable + 8);
            }
            currentBuild = event.sequence; currentOpcode = event.opcode;
            Push(event);
        }
        result = originalBuild(protocol, object);
    } __finally {
        currentBuild = oldBuild; currentOpcode = oldOpcode;
        --callbacks;
    }
    return result;
}
uint64_t WriteHook(uint64_t stream, void* bytes, uint64_t length) {
    ++callbacks;
    uint64_t result = 0;
    __try {
        if (enabled.load() && stream == watchedStream && length) {
            Event event{};
            BeginEvent(event, 2, reinterpret_cast<uint64_t>(_ReturnAddress()));
            event.rcx = stream; event.rdx = reinterpret_cast<uint64_t>(bytes); event.r8 = length;
            event.buildSequence = currentBuild; event.opcode = currentOpcode;
            event.originalLength = uint32_t((std::min)(length, uint64_t(UINT32_MAX)));
            event.dataLength = uint32_t((std::min)(length, uint64_t(sizeof(event.data))));
            event.flags = length > sizeof(event.data) ? 1u : 0u;
            if (!Read(event.rdx, event.data, event.dataLength)) { event.dataLength = 0; event.flags |= 2; }
            Push(event);
        }
        result = originalWrite(stream, bytes, length);
    } __finally { --callbacks; }
    return result;
}
template<size_t N> bool Signature(uint64_t rva, const uint8_t (&expected)[N]) noexcept {
    uint8_t actual[N]{};
    return Read(base + rva, actual, N) && !std::memcmp(actual, expected, N);
}
bool RemoveHooks() noexcept {
    enabled = false;
    bool ok = true;
    if (buildCreated) {
        const auto status = MH_DisableHook(reinterpret_cast<void*>(base + kPacketBuildRva));
        ok = (status == MH_OK || status == MH_ERROR_DISABLED) && ok;
    }
    if (writeCreated) {
        const auto status = MH_DisableHook(reinterpret_cast<void*>(base + kPacketWriteRva));
        ok = (status == MH_OK || status == MH_ERROR_DISABLED) && ok;
    }
    const auto until = GetTickCount64() + 3000;
    while (callbacks.load() && GetTickCount64() < until) Sleep(5);
    if (callbacks.load()) return false;
    if (buildCreated) {
        const auto status = MH_RemoveHook(reinterpret_cast<void*>(base + kPacketBuildRva));
        if (status == MH_OK) buildCreated = false; else ok = false;
    }
    if (writeCreated) {
        const auto status = MH_RemoveHook(reinterpret_cast<void*>(base + kPacketWriteRva));
        if (status == MH_OK) writeCreated = false; else ok = false;
    }
    if (initialized && !buildCreated && !writeCreated) {
        if (MH_Uninitialize() == MH_OK) initialized = false; else ok = false;
    }
    return ok;
}
void Drain(Block& block) noexcept {
    AcquireSRWLockExclusive(&queueLock);
    block.eventCount = (std::min)(queueCount, portalprobe::kBatch);
    for (uint32_t i = 0; i < block.eventCount; ++i) {
        block.events[i] = queue[queueBegin];
        queueBegin = (queueBegin + 1) % kCapacity;
    }
    queueCount -= block.eventCount;
    ReleaseSRWLockExclusive(&queueLock);
    block.totalEvents = sequence.load(); block.dropped = dropped.load();
    block.hooksActive = buildCreated || writeCreated || callbacks.load() ? 1u : 0u;
}
uint32_t Run(Block& block) {
    block.status = 2; block.eventCount = 0;
    if (block.magic != portalprobe::kMagic || block.version != portalprobe::kVersion ||
        block.bytes != sizeof(Block) || block.operation < 1 || block.operation > 3) return 10;
    if (block.operation == 1) {
        if (buildCreated || writeCreated) return 11;
        base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        std::string error;
        if (!gamebuild::VerifyLocal(base, error) ||
            !Signature(kPacketBuildRva, kPacketBuildSignature) || !Signature(kPacketWriteRva, kPacketIoSignature)) return 12;
        watchedState = block.state; watchedProtocol = block.protocol; watchedStream = block.stream;
        watchedWorld = block.world; watchedEntity = block.entity; watchedPositioned = block.positioned;
        if (!SameContext()) return 13;
        if (MH_Initialize() != MH_OK) return 14;
        initialized = true;
        if (MH_CreateHook(reinterpret_cast<void*>(base + kPacketBuildRva), reinterpret_cast<void*>(&BuildHook),
                          reinterpret_cast<void**>(&originalBuild)) != MH_OK) { RemoveHooks(); return 15; }
        buildCreated = true;
        if (MH_CreateHook(reinterpret_cast<void*>(base + kPacketWriteRva), reinterpret_cast<void*>(&WriteHook),
                          reinterpret_cast<void**>(&originalWrite)) != MH_OK) { RemoveHooks(); return 16; }
        writeCreated = true;
        if (MH_QueueEnableHook(reinterpret_cast<void*>(base + kPacketBuildRva)) != MH_OK ||
            MH_QueueEnableHook(reinterpret_cast<void*>(base + kPacketWriteRva)) != MH_OK || MH_ApplyQueued() != MH_OK) {
            RemoveHooks(); return 17;
        }
        enabled = true;
    } else if (block.operation == 3 && !RemoveHooks()) return 18;
    Drain(block); block.status = 1;
    return 0;
}
}
extern "C" __declspec(dllexport) DWORD WINAPI ProbeRun(void* parameter) {
    auto& block = *static_cast<Block*>(parameter);
    uint32_t code = 0;
    try { code = Run(block); }
    catch (...) { code = 99; }
    if (code) { block.status = 2; block.exceptionCode = code; Drain(block); }
    return code;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
