#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../Common/PlayerVitals.h"
#include "../Common/MonitorInterface.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {
int passed = 0, failed = 0;
#define CHECK(value) do { if (value) ++passed; else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #value); } } while (0)
struct Fixture {
    uint64_t base = 0x140000000, entity = 0, world = 0;
    uint64_t life = 0x30000000, metadata = 0x31000000, header = 0x32000000;
    uint64_t pairs = 0x33000000, array = 0x34000000;
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
    Fixture() {
        uint64_t cursor = 0x20000000; Put(base + BaseOffset, cursor);
        for (int i = 0; i < kLocalPlayerChainDepth; ++i) {
            const uint64_t next = cursor + 0x10000;
            Put(cursor + kMapObjectChain[i], next); cursor = next;
            if (i + 1 == kWorldAreaWorldChainDepth) world = cursor;
        }
        entity = cursor;
        Put(entity + kEntityPairsOff0, metadata); Put(metadata + kEntityPairsOff1, header);
        Put(header + kEntityPairsOff2, pairs); Put(header + kEntityPairsEndOff, pairs + 2 * kComponentPairSize);
        Put(entity + kEntityComponentsOff, array); Put(entity + kEntityComponentsEndOff, array + 2 * kComponentPtrSize);
        const uint64_t name = 0x35000000, playerName = 0x35000100;
        Put(pairs, name); Put(name, "Life"); Put(pairs + kComponentPairIndexOff, uint32_t(1));
        Put(pairs + kComponentPairSize, playerName); Put(playerName, "Player");
        Put(pairs + kComponentPairSize + kComponentPairIndexOff, uint32_t(0));
        Put(array, uint64_t(0x36000000)); Put(array + kComponentPtrSize, life);
        Put(life, base + kLifeVtableRva); Put(life + skilllayout::kComponentOwnerEntity, entity);
        const uint32_t maxima[] = {1666, 600, 2612}, currents[] = {1000, 200, 100};
        for (size_t i = 0; i < recovery::kResources; ++i) {
            const auto address = life + kLifeResourceOff[i];
            Put(address, base + kLifeResourceVtableRva[i]); Put(address + kLifeResourceOwnerOff, life);
            Put(address + kLifeResourceMaximumOff, maxima[i]); Put(address + kLifeResourceCurrentOff, currents[i]);
        }
    }
    bool Sample(recovery::Vitals& result) const {
        return recovery::ReadVitals(base, [this](uint64_t a, void* b, size_t n) { return Read(a, b, n); }, result);
    }
};
void Policy() {
    recovery::Config config;
    CHECK(recovery::Valid(config));
    recovery::Snapshot state; state.valid = 1;
    state.current[0] = 599; state.maximum[0] = 1000;
    state.current[1] = 40; state.maximum[1] = 100;
    uint64_t last[2]{};
    CHECK(recovery::Due(config, state, last, 1000) == 0);
    config.enabled = 1;
    CHECK(recovery::Due(config, state, last, 1000) == 1);
    CHECK(!recovery::Below(600, 1000, 60));
    CHECK(recovery::Below(599, 1000, 60));
    CHECK(!recovery::Below(0, 0, 30));
    CHECK(!recovery::Below(101, 100, 60));
    CHECK(!recovery::Below(0, 100, 0));
    CHECK(recovery::Below(1, UINT32_MAX, 100));
    state.current[2] = 10; state.maximum[2] = 100;
    CHECK(recovery::Due(config, state, last, 1000) == 1); // life + shield share one flask
    state.current[1] = 39;
    CHECK(recovery::Due(config, state, last, 1000) == 3);
    last[0] = 1000;
    CHECK(recovery::Due(config, state, last, 2499) == 2);
    CHECK(recovery::Due(config, state, last, 2500) == 3);
    CHECK(recovery::Due(config, state, last, 999) == 2);
    state.current[0] = 0;
    CHECK(recovery::Due(config, state, last, 2500) == 0);
    state.current[0] = 599; state.valid = 0;
    CHECK(recovery::Due(config, state, last, 2500) == 0);
    state.valid = 1; config.threshold[0] = 101;
    CHECK(!recovery::Valid(config)); CHECK(recovery::Due(config, state, last, 2500) == 0);
    config.threshold[0] = 60; config.flask[1] = 2;
    CHECK(!recovery::Valid(config)); config.flask[1] = 1;
    config.intervalMs[0] = 499; CHECK(!recovery::Valid(config));
    config.intervalMs[0] = 60001; CHECK(!recovery::Valid(config));
    config.intervalMs[0] = 500; CHECK(recovery::Valid(config));
    for (auto& threshold : config.threshold) threshold = 0;
    CHECK(!recovery::Valid(config)); config.enabled = 0; CHECK(recovery::Valid(config));
    auto info = monitorabi::Info{};
    CHECK(monitorabi::Compatible(info));
    info.capabilities &= ~monitorabi::kAutomaticRecovery;
    CHECK(!monitorabi::Compatible(info));
}
void IndependentIntervals() {
    recovery::Config config; config.enabled = 1;
    config.intervalMs[0] = 500; config.intervalMs[1] = 1000; config.intervalMs[2] = 3000;
    recovery::Snapshot state; state.valid = 1;
    for (int i = 0; i < 3; ++i) { state.current[i] = 10; state.maximum[i] = 100; }
    uint64_t last[2]{1000,1000};
    CHECK(recovery::Due(config,state,last,1499) == 0);
    CHECK(recovery::Due(config,state,last,1500) == 1);
    CHECK(recovery::Due(config,state,last,1999) == 1);
    CHECK(recovery::Due(config,state,last,2000) == 3);
    state.current[0] = 100; // Shield has a separate, slower rule on the same flask.
    CHECK(recovery::Due(config,state,last,3999) == 2);
    CHECK(recovery::Due(config,state,last,4000) == 3);
    state.current[0] = 10;
    CHECK(recovery::Due(config,state,last,4000) == 3); // Deduplicate life + shield.
    last[0] = 4000;
    CHECK(recovery::Due(config,state,last,4499) == 2);
    CHECK(recovery::Due(config,state,last,4500) == 3);
    config.threshold[0] = 0;
    CHECK(recovery::Due(config,state,last,6999) == 2);
    CHECK(recovery::Due(config,state,last,7000) == 3);
    CHECK(recovery::Due(config,state,last,999) == 0);
    config.intervalMs[1] = 499; CHECK(!recovery::Valid(config));
    config.intervalMs[1] = 1000; config.intervalMs[2] = 60001; CHECK(!recovery::Valid(config));
    config.intervalMs[2] = 60000; CHECK(recovery::Valid(config));
    static_assert(sizeof(recovery::Config) == 40, "three interval values on wire");
}
void Reads() {
    Fixture fixture; recovery::Vitals result;
    CHECK(fixture.Sample(result));
    CHECK(result.entity == fixture.entity && result.world == fixture.world && result.life == fixture.life);
    CHECK(result.current[0] == 1000 && result.maximum[0] == 1666);
    CHECK(result.current[1] == 200 && result.maximum[1] == 600);
    CHECK(result.current[2] == 100 && result.maximum[2] == 2612);
    auto corrupt = fixture; corrupt.Put(fixture.life + skilllayout::kComponentOwnerEntity, uint64_t(0x40000000));
    CHECK(!corrupt.Sample(result)); CHECK(!result.entity);
    corrupt = fixture; corrupt.Put(fixture.life + kLifeResourceOff[1] + kLifeResourceOwnerOff, uint64_t(0x40000000));
    CHECK(!corrupt.Sample(result));
    corrupt = fixture; corrupt.Put(fixture.life, fixture.base + kLifeVtableRva + 8);
    CHECK(!corrupt.Sample(result));
    corrupt = fixture; corrupt.Put(fixture.life + kLifeResourceOff[1], fixture.base + kLifeResourceVtableRva[0]);
    CHECK(!corrupt.Sample(result));
    corrupt = fixture; corrupt.Put(fixture.life + kLifeResourceOff[0] + kLifeResourceMaximumOff, uint32_t(0));
    CHECK(!corrupt.Sample(result));
    corrupt = fixture; corrupt.Put(fixture.life + kLifeResourceOff[2] + kLifeResourceMaximumOff, uint32_t(0));
    corrupt.Put(fixture.life + kLifeResourceOff[2] + kLifeResourceCurrentOff, uint32_t(0));
    CHECK(corrupt.Sample(result)); CHECK(!result.maximum[2]);
    corrupt = fixture; corrupt.Put(fixture.life + kLifeResourceOff[1] + kLifeResourceCurrentOff, uint32_t(601));
    CHECK(!corrupt.Sample(result));
    corrupt = fixture; corrupt.bytes.erase(fixture.life + kLifeResourceOff[1] + kLifeResourceCurrentOff);
    CHECK(!corrupt.Sample(result));
    corrupt = fixture; corrupt.Put(fixture.header + kEntityPairsEndOff, fixture.pairs + 0x10000);
    CHECK(!corrupt.Sample(result));
    corrupt = fixture; corrupt.Put(fixture.pairs + kComponentPairIndexOff, uint32_t(9));
    CHECK(!corrupt.Sample(result));
    size_t rootReads = 0;
    const bool changed = recovery::ReadVitals(fixture.base, [&](uint64_t a, void* b, size_t n) {
        if (a == fixture.base + BaseOffset && ++rootReads > 1) return false;
        return fixture.Read(a, b, n);
    }, result);
    CHECK(!changed); CHECK(!result.entity);
}
} // namespace
int main() {
    Policy(); Reads(); IndependentIntervals();
    std::printf("Recovery: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
