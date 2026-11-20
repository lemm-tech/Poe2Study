#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ViewService.h"
#include "../Common/GameBuild.h"
#include "../Common/ViewControls.h"
#include "Log.h"
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace viewservice {
namespace {
std::mutex mutex;
viewcontrols::Config config;
managed::Route owner;
viewcontrols::ZoomLimit zoomLimit;
void* limitStorage = nullptr;
uint64_t nextTick = 0;
bool Read(uint64_t address, void* out, size_t bytes) {
    SIZE_T done = 0;
    return IsPlausibleAddress(address) && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
        out, bytes, &done) && done == bytes;
}
bool Write(uint64_t address, const void* value, size_t bytes) {
    SIZE_T done = 0;
    return IsPlausibleAddress(address) && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
        value, bytes, &done) && done == bytes;
}
bool Patch(uint64_t address, int32_t expected, int32_t replacement) {
    if (address & 3) return false;
    auto* operand = reinterpret_cast<volatile LONG*>(address);
    DWORD original = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(LONG), PAGE_EXECUTE_READWRITE, &original)) return false;
    const auto before = InterlockedCompareExchange(operand, LONG(replacement), LONG(expected));
    const bool changed = before == expected;
    const bool flushed = !changed || FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), sizeof(LONG));
    DWORD ignored = 0;
    const bool protectedAgain = VirtualProtect(reinterpret_cast<void*>(address), sizeof(LONG), original, &ignored) != FALSE;
    return changed && flushed && protectedAgain;
}
bool LimitStorage(uint64_t base) {
    if (limitStorage) return true;
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const uint64_t origin = base + viewlayout::kZoomClampRva + 8;
    const uint64_t low = origin > 0x7FFF0000 ? origin - 0x7FFF0000 : 0x10000;
    const uint64_t high = (std::min)(origin + 0x7FFF0000,
        reinterpret_cast<uint64_t>(info.lpMaximumApplicationAddress));
    uint64_t cursor = low;
    while (cursor < high) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region))) return false;
        const uint64_t end = reinterpret_cast<uint64_t>(region.BaseAddress) + region.RegionSize;
        if (end <= cursor) return false;
        const uint64_t aligned = (cursor + info.dwAllocationGranularity - 1) & ~(uint64_t(info.dwAllocationGranularity) - 1);
        if (region.State == MEM_FREE && aligned + 4096 <= end && aligned + 4096 <= high) {
            limitStorage = VirtualAlloc(reinterpret_cast<void*>(aligned), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (limitStorage) return true;
        }
        cursor = end;
    }
    return false;
}
bool WriteLimit(uint64_t address, const void* value, size_t bytes) {
    if (address != reinterpret_cast<uint64_t>(limitStorage) || bytes != sizeof(LONG)) return false;
    LONG bits = 0; std::memcpy(&bits, value, sizeof(bits));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(limitStorage), bits);
    return true;
}
template<size_t N> bool Signature(uint64_t base, uint64_t rva, const uint8_t (&expected)[N]) {
    uint8_t bytes[N]{};
    return Read(base + rva, bytes, N) && std::memcmp(bytes, expected, N) == 0;
}
bool Validate(uint64_t base, bool reveal, std::string& error) {
    using namespace viewlayout;
    if (!gamebuild::VerifyLocal(base, error)) return false;
    if (!Signature(base, kZoomInputRva, kZoomSignature) || !zoomLimit.Matches(base, Read) || (reveal &&
        (!Signature(base, kRevealCpuRva, kRevealCpuSignature) || !Signature(base, kRevealRenderRva, kRevealRenderSignature) ||
         !Signature(base, kRevealClearRva, kRevealClearSignature)))) {
        error = "视角或小地图指纹不匹配"; return false;
    }
    return true;
}
bool Resolve(uint64_t base, viewcontrols::Scene& scene) { return viewcontrols::Resolve(base, Read, scene); }
bool Stop(uint64_t base) {
    const float nativeMaximum = 1.0f;
    if (limitStorage) WriteLimit(reinterpret_cast<uint64_t>(limitStorage), &nativeMaximum, sizeof(nativeMaximum));
    const bool restored = zoomLimit.Restore(Read, Patch);
    if (!restored) LOGI("[View] restore wheel limit failed");
    // Keep this tiny data page until process exit. An in-flight wheel instruction
    // can still hold the old operand address after the displacement is restored;
    // it must never dereference freed storage during DLL shutdown/reload.
    viewcontrols::Scene scene;
    if (restored && Resolve(base, scene)) viewcontrols::ClampTarget(base, scene, Read, Write, 1.0f);
    config.enabled = 0; owner = {};
    return restored;
}
viewcontrols::Snapshot Snapshot(uint64_t base) {
    viewcontrols::Snapshot state;
    state.enabled = config.enabled; state.maximum = config.maximum;
    viewcontrols::Scene scene;
    if (Resolve(base, scene) && Read(scene.camera + viewlayout::kZoomCurrent, &state.current, sizeof(state.current)) &&
        std::isfinite(state.current)) state.valid = 1;
    return state;
}
}
packet::Reply Handle(const packet::Request& request, const managed::Route& route) {
    std::lock_guard<std::mutex> lock(mutex);
    packet::Reply reply;
    const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    const bool configure = request.operation == packet::Operation::ConfigureView;
    const bool reveal = request.operation == packet::Operation::RevealMinimap;
    viewcontrols::Config requested;
    auto finish = [&](packet::Result result, const char* detail) {
        reply.result = result;
        std::snprintf(reply.detail, sizeof(reply.detail), "%s", detail);
        const auto state = Snapshot(base);
        reply.objectLength = sizeof(state); std::memcpy(reply.object, &state, sizeof(state));
        return reply;
    };
    if (request.version != packet::kVersion || request.options || request.captureMask || request.itemKey || request.actionId ||
        request.x || request.y || request.expectedEntity || request.length != (configure ? sizeof(requested) : 0) ||
        (!configure && !reveal && request.operation != packet::Operation::ViewStatus))
        return finish(packet::Result::BadRequest, "视角请求参数无效");
    if (configure) {
        std::memcpy(&requested, request.bytes, sizeof(requested));
        if (!viewcontrols::Valid(requested)) return finish(packet::Result::BadRequest, "缩放上限范围为 0.25–3.00");
        if (!requested.enabled) {
            if (!Stop(base)) return finish(packet::Result::Exception, "恢复原生缩放上限失败，请重试取消");
            config = requested;
            return finish(packet::Result::Ok, "已恢复原生缩放上限");
        }
    }
    if (!configure && !reveal) return finish(packet::Result::Ok, "");
    std::string error;
    if (!Validate(base, reveal, error)) return finish(packet::Result::UnsupportedBuild, error.c_str());
    viewcontrols::Scene scene;
    if (!Resolve(base, scene)) return finish(packet::Result::ContextUnavailable, "请进入场景后重试");
    if (configure) {
        if (!LimitStorage(base) || !zoomLimit.Apply(base, reinterpret_cast<uint64_t>(limitStorage),
                Read, WriteLimit, Patch, requested.maximum) ||
            !viewcontrols::ClampTarget(base, scene, Read, Write, requested.maximum)) {
            Stop(base);
            return finish(packet::Result::ContextChanged, "缩放上限设置未完成，请重新操作");
        }
        config = requested; owner = route; nextTick = 0;
        LOGI("[View] wheel zoom maximum: %.2f", config.maximum);
        return finish(packet::Result::Ok, "缩放上限已设置，可使用滚轮调整视角");
    }
    uint64_t minimap = 0, checkedMap = 0;
    viewcontrols::Scene checked;
    if (!viewcontrols::Minimap(scene, Read, minimap)) return finish(packet::Result::ContextUnavailable, "小地图数据暂不可用");
    uint8_t flags[3]{};
    if (!Read(minimap + viewlayout::kRevealWalkable, flags, sizeof(flags)))
        return finish(packet::Result::ContextUnavailable, "小地图请求状态不可读");
    if (flags[0] || flags[1] || flags[2]) return finish(packet::Result::Busy, "地图正在更新，请稍后重试");
    if (!Resolve(base, checked) || !(checked == scene) || !viewcontrols::Minimap(checked, Read, checkedMap) || minimap != checkedMap)
        return finish(packet::Result::ContextChanged, "地图变化，未执行全显");
    const uint8_t requestFull = 1;
    if (!Write(minimap + viewlayout::kRevealFull, &requestFull, sizeof(requestFull))) return finish(packet::Result::Exception, "全显请求写入失败");
    reply.invoked = 1;
    LOGI("[View] minimap full reveal requested");
    return finish(packet::Result::Ok, "已触发小地图全显");
}
void Tick(const std::function<bool(const managed::Route&)>& current) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!config.enabled) return;
    const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    if (!current(owner)) { Stop(base); return; }
    const auto now = GetTickCount64();
    if (now < nextTick) return;
    nextTick = now + 100;
    viewcontrols::Scene scene;
    if (!Resolve(base, scene)) return;
    std::string error;
    if (!Validate(base, false, error)) { Stop(base); LOGI("[View] stopped: %s", error.c_str()); return; }
    if (!zoomLimit.Matches(base, Read) || !viewcontrols::ClampTarget(base, scene, Read, Write, config.maximum)) {
        Stop(base); LOGI("[View] stopped after limit/scene failure");
    }
}
bool Shutdown() {
    std::lock_guard<std::mutex> lock(mutex);
    return Stop(reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)));
}
}
