#pragma once
#include "GameOffsets.h"
#include <array>
#include <string>
#include <vector>

// Shared by the external reader and Monitor. No game functions are invoked.
namespace ritualstate {
struct Snapshot {
    bool valid = false, hasInteraction = false, hasNpc = false;
    int32_t current = -1, interaction = -1, disabledByNpc = -1;
    uint64_t component = 0;
    bool CanStart() const {
        return valid && current == 1 && hasInteraction && interaction == 1 && hasNpc && disabledByNpc == 0;
    }
    bool Fighting() const { return valid && current == 2; }
    // Per-node completion. Reward inspection unlocks after all nodes finish.
    bool Completed() const { return valid && current == 3; }
};
inline const char* Text(const Snapshot& state) {
    if (!state.valid) return "状态不可读";
    if (state.Completed()) return "节点已完成";
    if (state.Fighting()) return "祭祀进行中";
    if (state.CanStart()) return "可开始祭祀";
    if (state.current == 1) return state.hasInteraction ? "暂不可开始" : "待开始／待确认";
    return "未验证状态";
}

template<class Read>
bool ReadEntity(uint64_t entity, Read read, Snapshot& out) {
    out = {};
    auto get = [&](uint64_t address, auto& value) {
        return IsPlausibleAddress(address) && read(address, &value, sizeof(value));
    };
    auto pointer = [&](uint64_t address, uint64_t& value) {
        return get(address, value) && IsPlausibleAddress(value);
    };
    auto unchanged = [&](uint64_t address, uint64_t expected) {
        uint64_t value = 0; return get(address, value) && value == expected;
    };
    uint64_t info = 0, header = 0, pairs = 0, end = 0, array = 0, arrayEnd = 0;
    if (!pointer(entity + kEntityPairsOff0, info) || !pointer(info + kEntityPairsOff1, header) ||
        !pointer(header + kEntityPairsOff2, pairs) || !pointer(header + kEntityPairsEndOff, end) ||
        !pointer(entity + kEntityComponentsOff, array) || !pointer(entity + kEntityComponentsEndOff, arrayEnd) ||
        end <= pairs || (end - pairs) % kComponentPairSize ||
        (end - pairs) / kComponentPairSize > kMaxComponentCount ||
        arrayEnd < array || arrayEnd - array != (end - pairs) / kComponentPairSize * kComponentPtrSize) return false;
    uint64_t machine = 0, machineSlot = 0;
    for (uint64_t i = 0; i < (end - pairs) / kComponentPairSize; ++i) {
        uint64_t name = 0; uint32_t index = 0; char text[sizeof("StateMachine")]{};
        const auto pair = pairs + i * kComponentPairSize;
        if (!pointer(pair, name) || !read(name, text, sizeof(text)) ||
            !get(pair + kComponentPairIndexOff, index) || index >= (arrayEnd - array) / kComponentPtrSize) return false;
        if (std::memcmp(text, "StateMachine", sizeof(text)) != 0) continue;
        if (machine) return false;
        machineSlot = array + index * kComponentPtrSize;
        if (!pointer(machineSlot, machine)) return false;
    }
    if (!machine || !unchanged(machine + skilllayout::kComponentOwnerEntity, entity)) return false;
    uint64_t definition = 0, keys = 0, keysEnd = 0, values = 0, valuesEnd = 0;
    if (!pointer(machine + kStateMachineDefinitionOff, definition) ||
        !pointer(definition + kStateMachineKeysBeginOff, keys) ||
        !pointer(definition + kStateMachineKeysEndOff, keysEnd) ||
        !pointer(machine + kStateMachineValuesBeginOff, values) ||
        !pointer(machine + kStateMachineValuesEndOff, valuesEnd) ||
        keysEnd <= keys || (keysEnd - keys) % kStateMachineKeyStride ||
        (keysEnd - keys) / kStateMachineKeyStride > kStateMachineMaxStates || valuesEnd < values ||
        valuesEnd - values != (keysEnd - keys) / kStateMachineKeyStride * kStateMachineValueStride) return false;
    const auto count = (keysEnd - keys) / kStateMachineKeyStride;
    std::vector<uint64_t> raw(size_t(count), 0), checked(size_t(count), 0);
    if (!read(values, raw.data(), raw.size() * sizeof(uint64_t))) return false;
    Snapshot result; result.component = machine;
    bool hasCurrent = false;
    std::vector<std::string> names;
    for (uint64_t i = 0; i < count; ++i) {
        const auto key = keys + i * kStateMachineKeyStride;
        uint64_t length = 0, capacity = 0, data = key;
        if (!get(key + kStdStringLengthOff, length) || !get(key + kStdStringMaxLenOff, capacity) ||
            !length || length > kStateMachineMaxName || capacity < length || capacity > 4096) return false;
        if (capacity > kStdNarrowStringSsoLimit && !pointer(key + kStdStringDataOff, data)) return false;
        std::array<char, kStateMachineMaxName + 1> text{};
        if (!read(data, text.data(), size_t(length) + 1) || text[size_t(length)] != 0) return false;
        for (size_t c = 0; c < length; ++c) {
            const auto ch = text[c];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
                  (c && ch >= '0' && ch <= '9'))) return false;
        }
        const std::string name(text.data(), size_t(length));
        for (const auto& previous : names) if (previous == name) return false;
        names.push_back(name);
        const auto value = static_cast<int32_t>(raw[size_t(i)] & UINT32_MAX);
        if (name == "current_state") { hasCurrent = true; result.current = value; }
        else if (name == "interaction_enabled") { result.hasInteraction = true; result.interaction = value; }
        else if (name == "disabled_by_ritual_npc") { result.hasNpc = true; result.disabledByNpc = value; }
    }
    if (!hasCurrent || !read(values, checked.data(), checked.size() * sizeof(uint64_t)) || checked != raw ||
        !unchanged(machine + kStateMachineDefinitionOff, definition) ||
        !unchanged(definition + kStateMachineKeysBeginOff, keys) || !unchanged(definition + kStateMachineKeysEndOff, keysEnd) ||
        !unchanged(machine + kStateMachineValuesBeginOff, values) || !unchanged(machine + kStateMachineValuesEndOff, valuesEnd) ||
        !unchanged(entity + kEntityPairsOff0, info) || !unchanged(info + kEntityPairsOff1, header) ||
        !unchanged(header + kEntityPairsOff2, pairs) || !unchanged(header + kEntityPairsEndOff, end) ||
        !unchanged(entity + kEntityComponentsOff, array) || !unchanged(entity + kEntityComponentsEndOff, arrayEnd) ||
        !unchanged(machineSlot, machine) || !unchanged(machine + skilllayout::kComponentOwnerEntity, entity)) return false;
    result.valid = true; out = result; return true;
}
} // namespace ritualstate
