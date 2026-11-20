#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Common/PortalReturn.h"
#include "../Common/MonitorInterface.h"
#include <cstdio>
#include <map>

namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
struct Portal {
    uint32_t key = 0xABCD3318;
    uint64_t entity = 0x20000000;
    int32_t x = 105, y = 105;
    std::string metadataPath = portalreturn::kMetadata, name = "藏身處：海岸線";
};
struct Memory {
    uint64_t base = 0x140000000, world = 0x20001000, player = 0x20003000, record = 0x20004000;
    uint64_t fail = 0, change = 0;
    int reads = 0;
    std::map<uint64_t, uint64_t> slots;
    Memory() {
        uint64_t p = 0x20010000;
        slots[base + BaseOffset] = p;
        for (int i = 0; i <= kLocalPlayerChainDepth; ++i) {
            auto next = 0x20020000ull + uint64_t(i) * 0x1000;
            if (i + 1 == kWorldAreaWorldChainDepth || i == kLocalPlayerChainDepth) next = world;
            if (i + 1 == kLocalPlayerChainDepth) next = player;
            slots[p + kMapObjectChain[i]] = next; p = next;
        }
        slots[world + kWorldAreaRecordOff] = record;
    }
    bool Read(uint64_t a, void* out, size_t n) {
        if (n != sizeof(uint64_t) || a == fail || !slots.count(a)) return false;
        if (a == change && ++reads > 1) slots[a] += 0x1000;
        std::memcpy(out, &slots[a], n); return true;
    }
    bool Scene(packet::PortalScene& out) {
        return portalreturn::ReadScene(base, [&](uint64_t a, void* p, size_t n) { return Read(a, p, n); }, out);
    }
};
void Selection() {
    using namespace portalreturn;
    Portal old, fresh; fresh.key++; fresh.entity += 0x1000;
    Ledger ledger; ledger.originX = ledger.originY = 100; ledger.before = {Id(old)};
    std::vector<Portal> portals{old, fresh};
    CHECK(!SelectNew(portals, ledger, 100, 100));
    ledger.summonSubmitted = true;
    CHECK(SelectNew(portals, ledger, 100, 100) == &portals[1]);
    portals = {old}; CHECK(!SelectNew(portals, ledger, 100, 100));
    fresh.entity = old.entity; portals = {old, fresh};
    CHECK(SelectNew(portals, ledger, 100, 100) == &portals[1]); // Address reuse, new full key.
    fresh = old; fresh.entity += 0x1000; portals = {fresh};
    CHECK(SelectNew(portals, ledger, 100, 100) == &portals[0]);
    CHECK(Id(portals[0]).key == 0xABCD3318); // Do not truncate to uint16.
    CHECK(!SelectNew(portals, ledger, -1, 100));
    auto invalid = fresh; invalid.metadataPath += "Fake"; CHECK(!IsHideoutPortal(invalid));
    invalid = fresh; invalid.name = "藏身处：海岸线"; CHECK(!IsHideoutPortal(invalid));
    invalid.name = "要塞"; CHECK(!IsHideoutPortal(invalid));
    invalid.name = "返回藏身處"; CHECK(IsHideoutPortal(invalid));
    invalid.key = 0; CHECK(!IsHideoutPortal(invalid));
    invalid = fresh; invalid.entity = 0; CHECK(!IsHideoutPortal(invalid));
    invalid = fresh; invalid.x = -1; CHECK(!IsHideoutPortal(invalid));
    invalid = fresh; invalid.y = -1; CHECK(!IsHideoutPortal(invalid));
    fresh.x = 221; fresh.y = 100; portals = {fresh};
    CHECK(!SelectNew(portals, ledger, 100, 100));
    fresh.x = 220; portals = {fresh}; CHECK(SelectNew(portals, ledger, 100, 100));
    CHECK(!SelectNew(portals, ledger, 99, 100));
    fresh.x = 250; portals = {fresh}; CHECK(!SelectNew(portals, ledger, 250, 100)); // Far from cast origin.
    fresh.x = 105; fresh.y = 105;
    Portal farther = fresh; farther.key++; farther.x = 115;
    portals = {farther, fresh}; CHECK(SelectNew(portals, ledger, 100, 100) == &portals[1]);
    farther.x = fresh.x; portals = {fresh, farther}; CHECK(!SelectNew(portals, ledger, 100, 100)); // Ambiguous.
    portals = {}; CHECK(!SelectNew(portals, ledger, 100, 100));
    CHECK(Distance(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX) > 6000000000.0);
    CHECK(Distance(100, 100, 105, 105) < kEnterDistance);
}
void Scenes() {
    using namespace portalreturn;
    Memory memory; packet::PortalScene scene;
    CHECK(memory.Scene(scene));
    CHECK(scene.world == memory.world && scene.mapOwner == memory.world && scene.player == memory.player && scene.record == memory.record);
    CHECK(ValidScene(scene));
    const auto original = scene;
    memory.fail = memory.world + kWorldAreaRecordOff;
    CHECK(!memory.Scene(scene)); CHECK(!scene.world && !scene.player);
    memory = Memory{}; memory.slots[memory.player + kMapObjectChain[kLocalPlayerChainDepth]] += 0x1000;
    CHECK(!memory.Scene(scene)); CHECK(!ValidScene(scene));
    memory = Memory{}; memory.change = memory.world + kWorldAreaRecordOff;
    CHECK(!memory.Scene(scene));
    memory = Memory{}; memory.slots[memory.world + kWorldAreaRecordOff] = 0;
    CHECK(!memory.Scene(scene));
    auto changed = original; changed.player += 0x1000;
    CHECK(SameMap(changed, original)); CHECK(!(changed == original));
    changed = original; changed.world += 0x1000; CHECK(!ValidScene(changed)); CHECK(!SameMap(changed, original));
    changed.mapOwner = changed.world; CHECK(ValidScene(changed));
    changed = original; changed.record += 0x1000; CHECK(!SameMap(changed, original));
}
void RequestsAndLedger() {
    using namespace portalreturn;
    Memory memory; packet::PortalScene scene; CHECK(memory.Scene(scene));
    packet::Request request; request.operation = packet::Operation::SummonPortal;
    CHECK(!ValidSummonRequest(request));
    request.length = sizeof(scene); std::memcpy(request.bytes, &scene, sizeof(scene));
    CHECK(ValidSummonRequest(request));
    request.options = packet::PreviewOnly; CHECK(ValidSummonRequest(request));
    request.options |= packet::AppendEnd; CHECK(!ValidSummonRequest(request)); request.options = 0;
    request.itemKey = 1; CHECK(!ValidSummonRequest(request)); request.itemKey = 0;
    request.expectedEntity = scene.player; CHECK(!ValidSummonRequest(request)); request.expectedEntity = 0;
    request.captureMask = packet::Send; CHECK(!ValidSummonRequest(request)); request.captureMask = 0;
    request.actionId = 1; CHECK(!ValidSummonRequest(request)); request.actionId = 0;
    request.x = 1; CHECK(!ValidSummonRequest(request)); request.x = 0;
    request.y = 1; CHECK(!ValidSummonRequest(request)); request.y = 0;
    request.length--; CHECK(!ValidSummonRequest(request)); request.length++;
    scene.world = 0; std::memcpy(request.bytes, &scene, sizeof(scene)); CHECK(!ValidSummonRequest(request));
    CHECK(packet::Sends(packet::Operation::SummonPortal)); CHECK(packet::Sends(packet::Operation::EnterPortal));
    CHECK(!packet::Sends(packet::Operation::RecoveryStatus));
    CHECK(uint32_t(packet::Operation::EnterPortal) == 7 && uint32_t(packet::Operation::RecoveryStatus) == 11);
    CHECK(sizeof(packet::Request) == 1584 && sizeof(packet::Reply) == 248);
    monitorabi::Info info; CHECK(monitorabi::Compatible(info));
    info.capabilities &= ~monitorabi::kPortalSummon; CHECK(!monitorabi::Compatible(info));
    Ledger ledger; CHECK(ledger.CanSummon()); CHECK(!ledger.CanEnter());
    ledger.summonSubmitted = true; CHECK(!ledger.CanSummon()); CHECK(!ledger.CanEnter()); // Lost reply is still one submission.
    ledger.target = {0xABCD3318, 0x20000000}; CHECK(ledger.CanEnter());
    ledger.enterSubmitted = true; CHECK(!ledger.CanSummon()); CHECK(!ledger.CanEnter());
    const auto resumed = ledger; CHECK(!resumed.CanSummon() && !resumed.CanEnter());
    ledger.arrived = true; CHECK(!ledger.CanSummon() && !ledger.CanEnter());
}
void ArrivalEvidence() {
    using namespace portalreturn;
    Memory memory; packet::PortalScene source; CHECK(memory.Scene(source));
    auto home = source; home.world += 0x100000; home.mapOwner = home.world; home.record += 0x100000; home.player += 0x100000;
    Arrival arrival;
    CHECK(!arrival.Observe(source, source, "藏身處", true, 100));
    auto respawn = source; respawn.player++; CHECK(!arrival.Observe(source, respawn, "藏身處", true, 1000));
    CHECK(!arrival.Observe(source, {}, "藏身處", true, 2000));
    CHECK(!arrival.Observe(source, home, "要塞", true, 3000));
    CHECK(!arrival.Observe(source, home, "藏身處", false, 4000));
    CHECK(!arrival.Observe(source, home, "海岸線藏身處", true, 5000));
    CHECK(!arrival.Observe(source, home, "海岸線藏身處", true, 5499));
    CHECK(arrival.Observe(source, home, "海岸線藏身處", true, 5500));
    CHECK(!arrival.Observe(source, {}, "", false, 5600)); // Loading resets stable evidence.
    CHECK(!arrival.Observe(source, home, "藏身處", true, 6000));
    auto other = home; other.player++;
    CHECK(!arrival.Observe(source, other, "藏身處", true, 6500));
    CHECK(!arrival.Observe(source, other, "藏身處", true, 6400)); // Clock moving backwards cannot complete.
    CHECK(arrival.Observe(source, other, "藏身處", true, 7000));
}
} // namespace
int main() {
    Selection(); Scenes(); RequestsAndLedger(); ArrivalEvidence();
    std::printf("portal_return: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
