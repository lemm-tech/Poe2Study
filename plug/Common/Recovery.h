#pragma once
#include <cstdint>
#include <cstddef>

namespace recovery {
constexpr uint16_t kFlaskOpcode = 0x8C;
constexpr size_t kResources = 3, kFlasks = 2;
// Resource order: life, mana, energy shield. Flask 0 = life, 1 = mana.
struct Config {
    uint32_t enabled = 0;
    uint32_t threshold[kResources]{60, 40, 30};
    uint32_t flask[kResources]{0, 1, 0};
    uint32_t intervalMs[kResources]{1500, 1500, 1500};
};
struct Snapshot {
    uint32_t current[kResources]{}, maximum[kResources]{};
    uint32_t enabled = 0, valid = 0, sent[kFlasks]{};
    uint32_t lastResult = 0, lastFlask = UINT32_MAX;
};
static_assert(sizeof(Config) == 40 && sizeof(Snapshot) == 48, "recovery wire layout");
inline bool Valid(const Config& config) {
    if (config.enabled > 1) return false;
    bool any = false;
    for (size_t i = 0; i < kResources; ++i) {
        if (config.threshold[i] > 100 || config.flask[i] >= kFlasks ||
            config.intervalMs[i] < 500 || config.intervalMs[i] > 60000) return false;
        any = any || config.threshold[i] != 0;
    }
    return !config.enabled || any;
}
inline bool Below(uint32_t current, uint32_t maximum, uint32_t percent) {
    return percent && percent <= 100 && maximum && current <= maximum &&
        uint64_t(current) * 100 < uint64_t(maximum) * percent;
}
inline uint32_t Due(const Config& config, const Snapshot& state, const uint64_t (&last)[kFlasks], uint64_t now) {
    if (!config.enabled || !Valid(config) || !state.valid || !state.current[0]) return 0;
    uint32_t mask = 0;
    for (size_t i = 0; i < kResources; ++i) {
        const auto flask = config.flask[i];
        if (Below(state.current[i], state.maximum[i], config.threshold[i]) &&
            (!last[flask] || (now >= last[flask] && now - last[flask] >= config.intervalMs[i])))
            mask |= uint32_t(1) << flask;
    }
    return mask;
}
} // namespace recovery
