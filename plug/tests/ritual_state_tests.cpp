#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../Common/RitualState.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {
int passed = 0, failed = 0;
#define CHECK(value) do { if (value) ++passed; else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #value); } } while (0)
struct Fixture {
    uint64_t entity = 0x10000, info = 0x20000, header = 0x30000, pairs = 0x40000;
    uint64_t array = 0x50000, machine = 0x60000, definition = 0x70000, keys = 0x80000, values = 0x90000;
    std::unordered_map<uint64_t, uint8_t> bytes;
    template<class T> void Put(uint64_t address, const T& value) {
        const auto* data = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) bytes[address + i] = data[i];
    }
    bool Read(uint64_t address, void* out, size_t size) const {
        for (size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            static_cast<uint8_t*>(out)[i] = it->second;
        }
        return true;
    }
    void Key(size_t i, const char* name) {
        const auto key = keys + i * kStateMachineKeyStride;
        const uint64_t length = std::strlen(name), capacity = length <= 15 ? 15 : 31;
        Put(key + kStdStringLengthOff, length); Put(key + kStdStringMaxLenOff, capacity);
        const uint64_t data = capacity == 15 ? key : 0xA0000 + i * 0x100;
        if (capacity > 15) Put(key, data);
        for (size_t c = 0; c <= length; ++c) Put(data + c, name[c]);
    }
    Fixture() {
        Put(entity + kEntityPairsOff0, info); Put(info + kEntityPairsOff1, header);
        Put(header + kEntityPairsOff2, pairs); Put(header + kEntityPairsEndOff, pairs + 2 * kComponentPairSize);
        Put(entity + kEntityComponentsOff, array); Put(entity + kEntityComponentsEndOff, array + 2 * kComponentPtrSize);
        Put(pairs, uint64_t(0xB0000)); Put(uint64_t(0xB0000), "StateMachine");
        Put(pairs + kComponentPairIndexOff, uint32_t(1));
        Put(pairs + kComponentPairSize, uint64_t(0xB1000)); Put(uint64_t(0xB1000), "InteractionAction");
        Put(pairs + kComponentPairSize + kComponentPairIndexOff, uint32_t(0));
        Put(array, uint64_t(0xC0000)); Put(array + kComponentPtrSize, machine);
        Put(machine + skilllayout::kComponentOwnerEntity, entity);
        Put(machine + kStateMachineDefinitionOff, definition);
        Put(definition + kStateMachineKeysBeginOff, keys);
        Put(definition + kStateMachineKeysEndOff, keys + 3 * kStateMachineKeyStride);
        Put(machine + kStateMachineValuesBeginOff, values);
        Put(machine + kStateMachineValuesEndOff, values + 3 * kStateMachineValueStride);
        // Intentionally reorder keys and component indices; neither is fixed.
        Key(0, "interaction_enabled"); Key(1, "disabled_by_ritual_npc"); Key(2, "current_state");
        Put(values, uint64_t(1)); Put(values + 8, uint64_t(0)); Put(values + 16, uint64_t(1));
    }
    bool Sample(ritualstate::Snapshot& out) const {
        return ritualstate::ReadEntity(entity, [this](uint64_t a, void* b, size_t n) { return Read(a, b, n); }, out);
    }
};
void Phases() {
    Fixture f; ritualstate::Snapshot state;
    CHECK(f.Sample(state) && state.CanStart() && !state.Fighting() && !state.Completed());
    f.Put(f.values, uint64_t(0)); f.Put(f.values + 16, uint64_t(2));
    CHECK(f.Sample(state) && !state.CanStart() && state.Fighting() && !state.Completed());
    f.Put(f.values, uint64_t(1)); f.Put(f.values + 16, uint64_t(3));
    CHECK(f.Sample(state) && !state.CanStart() && !state.Fighting() && state.Completed());
    f.Put(f.values + 16, uint64_t(4));
    CHECK(f.Sample(state) && !state.CanStart() && !state.Fighting() && !state.Completed());
    f.Put(f.values + 16, uint64_t(1)); f.Put(f.values + 8, uint64_t(1));
    CHECK(f.Sample(state) && !state.CanStart());
    f.Put(f.values + 8, uint64_t(0)); f.Put(f.values, uint64_t(0));
    CHECK(f.Sample(state) && !state.CanStart());
    f.Key(0, "other"); f.Key(1, "another"); f.Put(f.values + 16, uint64_t(3));
    CHECK(f.Sample(state) && state.Completed() && !state.CanStart());
    f.Put(f.values + 16, uint64_t(1));
    CHECK(f.Sample(state) && !state.CanStart());
}
void InvalidReads() {
    ritualstate::Snapshot state;
    Fixture f; f.bytes.erase(f.values);
    CHECK(!f.Sample(state) && !state.valid && !state.CanStart() && !state.Completed());
    f = Fixture{}; f.Put(f.machine + skilllayout::kComponentOwnerEntity, uint64_t(0xDEAD));
    CHECK(!f.Sample(state) && !state.valid);
    f = Fixture{}; f.Put(f.machine + kStateMachineValuesEndOff, f.values + 16);
    CHECK(!f.Sample(state) && !state.valid);
    f = Fixture{}; f.Put(f.pairs + kComponentPairIndexOff, uint32_t(2));
    CHECK(!f.Sample(state) && !state.valid);
    f = Fixture{}; f.Key(0, "current_state");
    CHECK(!f.Sample(state) && !state.valid);
    f = Fixture{}; f.Put(f.keys + kStdStringLengthOff, uint64_t(100000));
    CHECK(!f.Sample(state) && !state.valid);
    f = Fixture{}; unsigned reads = 0;
    CHECK(!ritualstate::ReadEntity(f.entity, [&](uint64_t a, void* b, size_t n) {
        if (a == f.values && ++reads == 2) f.Put(f.values + 16, uint64_t(3));
        return f.Read(a, b, n);
    }, state) && !state.valid);
}
}
int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--read-only") == 0) {
        const auto handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, std::strtoul(argv[2], nullptr, 10));
        if (!handle) return 2;
        bool all = true;
        for (int i = 3; i < argc; ++i) {
            const auto entity = std::strtoull(argv[i], nullptr, 0);
            ritualstate::Snapshot state;
            const bool ok = ritualstate::ReadEntity(entity, [&](uint64_t address, void* out, size_t size) {
                SIZE_T done = 0;
                return ReadProcessMemory(handle, reinterpret_cast<const void*>(address), out, size, &done) && done == size;
            }, state);
            std::printf("entity=0x%llX valid=%d current=%d interaction=%d npc=%d canStart=%d fighting=%d completed=%d\n",
                entity, int(ok), state.current, state.interaction, state.disabledByNpc,
                int(state.CanStart()), int(state.Fighting()), int(state.Completed()));
            all = all && ok;
        }
        CloseHandle(handle); return all ? 0 : 1;
    }
    Phases(); InvalidReads();
    std::printf("Ritual state: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
