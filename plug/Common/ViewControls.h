#pragma once
#include "GameOffsets.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace viewcontrols {
constexpr float kMinZoom = 0.25f, kMaxZoom = 3.0f;
struct Config { uint32_t enabled = 0; float maximum = 1.0f; };
struct Snapshot {
    uint32_t enabled = 0, valid = 0;
    float maximum = 1.0f, current = 1.0f;
};
static_assert(sizeof(Config) == 8 && sizeof(Snapshot) == 16, "view controls wire layout");
inline bool Valid(const Config& config) {
    return config.enabled <= 1 && std::isfinite(config.maximum) && config.maximum >= kMinZoom && config.maximum <= kMaxZoom;
}
struct Scene {
    uint64_t client = 0, camera = 0, world = 0;
    bool operator==(const Scene& b) const { return client == b.client && camera == b.camera && world == b.world; }
};
template<class Read> bool Resolve(uint64_t base, Read read, Scene& out) {
    using namespace viewlayout;
    out = {};
    auto pointer = [&](uint64_t slot, uint64_t& value) {
        return IsPlausibleAddress(slot) && read(slot, &value, sizeof(value)) && IsPlausibleAddress(value);
    };
    uint64_t manager = 0, global = 0, owner = 0, state = 0, world = 0, reference = 0;
    Scene next;
    if (!pointer(base + kManagerRva, manager) || !pointer(manager + kManagerClient, next.client) ||
        !pointer(next.client + kClientCamera, next.camera) || !pointer(base + BaseOffset, global) ||
        !pointer(global + kGlobalOwner, owner) || !pointer(owner + kOwnerState, state) ||
        !pointer(state, state) || !pointer(state + kStateWorld, next.world) ||
        !pointer(next.camera + kCameraWorld, world) || world != next.world ||
        !pointer(global + kGlobalCamera, reference) || reference != next.camera) return false;
    out = next;
    return true;
}
template<class Read> bool Minimap(const Scene& scene, Read read, uint64_t& out) {
    using namespace viewlayout;
    out = 0;
    uint64_t map = 0, reference = 0, render = 0, descriptor = 0;
    uint64_t begin = 0, end = 0, capacity = 0;
    uint32_t dimensions[2]{}; float world[2]{}; uint8_t flags[3]{};
    auto pointer = [&](uint64_t slot, uint64_t& value) {
        return IsPlausibleAddress(slot) && read(slot, &value, sizeof(value)) && IsPlausibleAddress(value);
    };
    if (!pointer(scene.client + kClientMinimap, map) || !pointer(map, reference) || reference != scene.world + kWorldMinimap ||
        !pointer(map + kMinimapRender, render) || !pointer(map + kDescriptor, descriptor) ||
        !read(descriptor + kDimensions, dimensions, sizeof(dimensions)) ||
        !read(descriptor + kWorldDimensions, world, sizeof(world)) ||
        !pointer(descriptor + kPixels, begin) || !pointer(descriptor + kPixelsEnd, end) ||
        !pointer(descriptor + kPixelsCapacity, capacity) || !read(map + kRevealWalkable, flags, sizeof(flags))) return false;
    const uint64_t count = uint64_t(dimensions[0]) * dimensions[1];
    if (!dimensions[0] || !dimensions[1] || dimensions[0] > 4096 || dimensions[1] > 4096 || count > 1048576 ||
        !std::isfinite(world[0]) || !std::isfinite(world[1]) || world[0] <= 0 || world[1] <= 0 ||
        end < begin || end - begin != count * sizeof(float) || capacity < end || capacity - begin > 0x1000000 ||
        flags[0] > 1 || flags[1] > 1 || flags[2] > 1) return false;
    out = map;
    return true;
}

// Redirect only this wheel clamp's aligned disp32 to a private float. The
// original shared 1.0f constant and the wheel's lower clamp stay untouched.
class ZoomLimit {
    uint64_t base_ = 0, slot_ = 0;
    int32_t displacement_ = 0;
    bool tracked_ = false;
    static int32_t Original() {
        int32_t value = 0;
        std::memcpy(&value, viewlayout::kZoomClampSignature + 4, sizeof(value));
        return value;
    }
    template<class Read> bool Instruction(uint64_t base, Read read, int32_t& displacement) const {
        uint8_t code[8]{};
        if (!read(base + viewlayout::kZoomClampRva, code, sizeof(code)) ||
            std::memcmp(code, viewlayout::kZoomClampSignature, 4)) return false;
        std::memcpy(&displacement, code + 4, sizeof(displacement));
        return true;
    }
public:
    template<class Read> bool Matches(uint64_t base, Read read) const {
        int32_t displacement = 0;
        return Instruction(base, read, displacement) && (tracked_ ?
            base == base_ && displacement == displacement_ : displacement == Original());
    }
    // Patch compares the old displacement atomically before replacing it.
    template<class Read, class Write, class Patch>
    bool Apply(uint64_t base, uint64_t slot, Read read, Write write, Patch patch, float maximum) {
        if (!Valid(Config{1, maximum}) || !IsPlausibleAddress(slot) || (slot & 3) ||
            ((base + viewlayout::kZoomClampRva + 4) & 3)) return false;
        const int64_t relative = int64_t(slot) - int64_t(base + viewlayout::kZoomClampRva + 8);
        if (relative < (std::numeric_limits<int32_t>::min)() || relative > (std::numeric_limits<int32_t>::max)() ||
            (tracked_ && (base != base_ || slot != slot_)) || !Matches(base, read)) return false;
        if (!write(slot, &maximum, sizeof(maximum))) return false;
        if (tracked_) return true;
        base_ = base; slot_ = slot; displacement_ = int32_t(relative); tracked_ = true;
        // Keep ownership on an ambiguous patch failure, so Restore can inspect
        // and undo a write that succeeded before page protection/cache failed.
        return patch(base + viewlayout::kZoomClampRva + 4, Original(), displacement_);
    }
    template<class Read, class Patch> bool Restore(Read read, Patch patch) {
        if (!tracked_) return true;
        int32_t current = 0;
        if (!Instruction(base_, read, current)) return false;
        if (current != Original() && (current != displacement_ ||
            !patch(base_ + viewlayout::kZoomClampRva + 4, displacement_, Original()))) return false;
        tracked_ = false; base_ = slot_ = 0;
        return true;
    }
};
// Only bring an already-outside target back inside a newly lowered cap. Normal
// wheel targets below it are never rewritten, including the native zero bound.
template<class Read, class Write>
bool ClampTarget(uint64_t base, const Scene& scene, Read read, Write write, float maximum) {
    float target = 0; Scene checked;
    if (!Valid(Config{1, maximum}) || !read(scene.camera + viewlayout::kZoomTarget, &target, sizeof(target)) ||
        !std::isfinite(target) || !Resolve(base, read, checked) || !(checked == scene)) return false;
    return target <= maximum || write(scene.camera + viewlayout::kZoomTarget, &maximum, sizeof(maximum));
}
} // namespace viewcontrols
