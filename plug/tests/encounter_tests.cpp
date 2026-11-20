#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Common/MonsterReader.h"
#include "../Common/EncounterRoute.h"
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>

namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
struct Memory {
    uint64_t next = 0x20000000, fail = 0;
    std::map<uint64_t, uint8_t> bytes;
    bool IsPlausible(uint64_t p) const { return IsPlausibleAddress(p); }
    bool ReadInto(uint64_t a, void* out, size_t n) const {
        if (fail && a <= fail && fail < a + n) return false;
        for (size_t i = 0; i < n; ++i) {
            auto it = bytes.find(a + i); if (it == bytes.end()) return false;
            static_cast<uint8_t*>(out)[i] = it->second;
        }
        return true;
    }
    template<class T> void Put(uint64_t a, T value) {
        for (size_t i = 0; i < sizeof(value); ++i) bytes[a + i] = reinterpret_cast<uint8_t*>(&value)[i];
    }
    uint64_t Alloc(size_t n) {
        const auto p = next; next += (n + 63) / 64 * 64;
        for (size_t i = 0; i < n; ++i) bytes[p + i] = 0;
        return p;
    }
    uint64_t Ascii(const std::string& text) {
        const auto p = Alloc(text.size() + 1);
        for (size_t i = 0; i < text.size(); ++i) Put(p + i, text[i]); return p;
    }
    uint64_t Wide(const std::u16string& text) {
        const auto p = Alloc((text.size() + 1) * 2);
        for (size_t i = 0; i < text.size(); ++i) Put(p + i * 2, text[i]); return p;
    }
    uint64_t Wide(const std::string& text) { return Wide(std::u16string(text.begin(), text.end())); }
    uint64_t Table(const std::string& name) {
        const auto p = Alloc(64); Put(p + 8, Wide(name));
        Put(p + 8 + kStdStringLengthOff, uint64_t(name.size()));
        Put(p + 8 + kStdStringMaxLenOff, uint64_t(name.size() + 8)); return p;
    }
    void Vector(uint64_t h, uint64_t data, uint64_t bytes) {
        Put(h, data); Put(h + 8, data + bytes); Put(h + 16, data + bytes);
    }
};
struct Fixture {
    Memory mem;
    uint64_t base = 0x140000000, entity, monster, magic, life, position, variety, world, area, modEntry, modRow, modValues;
    std::vector<monster::ConfiguredBoss> catalog;
    std::string metadata = "Metadata/Monsters/PrisonerBoss/PrisonerBossMAP_";
    uint64_t Parts(uint64_t owner, std::vector<std::pair<std::string, uint64_t>> parts) {
        const auto info = mem.Alloc(128), header = mem.Alloc(128), pairs = mem.Alloc(parts.size() * kComponentPairSize);
        const auto array = mem.Alloc(parts.size() * kComponentPtrSize);
        mem.Put(owner + kEntityPairsOff0, info); mem.Put(info + kEntityPairsOff1, header);
        mem.Put(header + kEntityPairsOff2, pairs); mem.Put(header + kEntityPairsEndOff, pairs + parts.size() * kComponentPairSize);
        mem.Put(owner + kEntityComponentsOff, array); mem.Put(owner + kEntityComponentsEndOff, array + parts.size() * kComponentPtrSize);
        for (size_t i = 0; i < parts.size(); ++i) {
            mem.Put(pairs + i * kComponentPairSize, mem.Ascii(parts[i].first));
            mem.Put(pairs + i * kComponentPairSize + kComponentPairIndexOff, uint32_t(i));
            mem.Put(array + i * kComponentPtrSize, parts[i].second);
        }
        return pairs;
    }
    Fixture() {
        entity = mem.Alloc(128); monster = mem.Alloc(128); magic = mem.Alloc(1024);
        life = mem.Alloc(1024); position = mem.Alloc(1280);
        for (const auto part : {monster, magic, life, position}) mem.Put(part + monsterlayout::kComponentOwner, entity);
        Parts(entity, {{"Monster", monster}, {"ObjectMagicProperties", magic}, {"Life", life}, {"Positioned", position}});
        mem.Put(monster, base + monsterlayout::kMonsterVtable); mem.Put(magic, base + monsterlayout::kMagicVtable);
        const auto data = mem.Alloc(64); variety = mem.Alloc(512);
        mem.Put(monster + monsterlayout::kMonsterData, data); mem.Put(data + monsterlayout::kVarietyRow, variety);
        const auto vt = mem.Table("Data/Balance/MonsterVarieties.dat"); mem.Put(data + monsterlayout::kVarietyTable, vt);
        mem.Put(variety, mem.Wide(metadata)); mem.Put(variety + monsterlayout::kVarietyName, mem.Wide(u"被束縛的恐懼"));
        world = mem.Alloc(512); area = mem.Alloc(512); mem.Put(world + kWorldAreaRecordOff, area);
        mem.Put(world + kWorldAreaTableOff, mem.Table("Data/Balance/WorldAreas.dat"));
        const auto refs = mem.Alloc(16); mem.Put(refs, variety); mem.Put(refs + 8, vt);
        mem.Put(area + monsterlayout::kWorldBossCount, uint64_t(1)); mem.Put(area + monsterlayout::kWorldBossData, refs);
        mem.Put(magic + monsterlayout::kMagicRarity, uint32_t(3)); mem.Put(life + kLife_HpOff, uint32_t(12345));
        modEntry = mem.Alloc(monsterlayout::kRuntimeModStride); modRow = mem.Alloc(128); modValues = mem.Alloc(8);
        mem.Vector(magic + monsterlayout::kRuntimeMods, modEntry, monsterlayout::kRuntimeModStride);
        mem.Vector(modEntry, modValues, 8); mem.Put(modValues, int32_t(3)); mem.Put(modValues + 4, int32_t(2));
        mem.Put(modEntry + monsterlayout::kRuntimeModRow, modRow);
        mem.Put(modEntry + monsterlayout::kRuntimeModTable, mem.Table("Data/Balance/Mods.dat"));
        const auto st = mem.Table("Data/Balance/Stats.dat");
        for (unsigned i = 0; i < 2; ++i) {
            const auto row = mem.Alloc(32); mem.Put(row, mem.Wide(i ? "i_am_boss_of_tier" : "monster_rarity"));
            mem.Put(modRow + monsterlayout::kModStatRefs + i * monsterlayout::kReferenceStride, row);
            mem.Put(modRow + monsterlayout::kModStatRefs + i * monsterlayout::kReferenceStride + 8, st);
        }
        catalog = monster::Reader<Memory>(mem, base).Catalog(world, area);
    }
    monster::Evidence Read() {
        uint64_t owner = 0; std::string name;
        auto result = monster::Reader<Memory>(mem, base).Identity(entity, metadata + "@78", catalog, owner, name);
        CHECK(owner == entity); CHECK(name == "被束縛的恐懼"); return result;
    }
    bool Reject() { try { Read(); return false; } catch (const std::exception&) { return true; } }
};
void Identity() {
    Fixture f;
    CHECK(f.catalog.size() == 1 && f.catalog[0].metadata == f.metadata);
    auto e = f.Read(); CHECK(monster::MapBoss(e)); CHECK(!monster::OptionalElite(e));
    CHECK(e.hpValid && e.hp == 12345 && e.bossTier == 2);
    CHECK(!monster::Destination(e)); e.positionValid = true; CHECK(monster::Destination(e));
    f.mem.Put(f.life + kLife_HpOff, uint32_t(0)); e = f.Read(); CHECK(monster::ConfirmedDead(e)); CHECK(!monster::Destination(e));
    f.mem.fail = f.life + kLife_HpOff; CHECK(f.Reject()); f.mem.fail = 0;
    f.catalog.clear(); e = f.Read(); CHECK(monster::Role(e) == "other_boss"); CHECK(monster::OptionalElite(e));
    f.mem.Put(f.magic + monsterlayout::kMagicRarity, uint32_t(2)); e = f.Read(); CHECK(monster::OptionalElite(e));
    f.mem.Put(f.magic + monsterlayout::kMagicRarity, uint32_t(1)); e = f.Read(); CHECK(monster::OptionalElite(e));
    f.mem.Put(f.magic + monsterlayout::kMagicRarity, uint32_t(0)); e = f.Read(); CHECK(!monster::OptionalElite(e));
    f.mem.Put(f.magic + monsterlayout::kMagicRarity, uint32_t(4)); CHECK(f.Reject());
    CHECK(monster::Canonical("a@78") == "a"); CHECK(monster::Canonical("a@phase") == "a@phase");
    CHECK(monster::Canonical("a@") == "a@");
    CHECK(monster::BossEntrance("Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame_Boss"));
    CHECK(!monster::BossEntrance("Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame"));
    CHECK(!monster::BossEntrance("Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame_BossEffect"));
    e = {}; e.hpValid = true; e.hp = 0; e.valid = false; CHECK(!monster::ConfirmedDead(e));
    e.valid = true; e.isCombatOwner = false; CHECK(!monster::ConfirmedDead(e));
    e.isCombatOwner = true; e.configuredForArea = true; e.rarity = 3;
    CHECK(!monster::MapBoss(e)); CHECK(!monster::OptionalElite(e));
}
void InvalidLayouts() {
    { Fixture f; f.mem.Put(f.monster, uint64_t(0)); CHECK(f.Reject()); }
    { Fixture f; f.mem.Put(f.magic, uint64_t(0)); CHECK(f.Reject()); }
    { Fixture f; f.mem.Put(f.life + monsterlayout::kComponentOwner, uint64_t(0)); CHECK(f.Reject()); }
    { Fixture f; f.mem.Put(f.modValues + 4, int32_t(0)); CHECK(!monster::MapBoss(f.Read())); }
    { Fixture f; f.mem.Vector(f.modEntry, f.modValues, 4); CHECK(f.Reject()); }
    { Fixture f; f.mem.Put(f.modValues, int32_t(1)); CHECK(f.Reject()); }
    { Fixture f; f.mem.Put(f.modEntry + monsterlayout::kRuntimeModTable, f.mem.Table("Data/Balance/Other.dat")); CHECK(f.Reject()); }
    { Fixture f; f.mem.Put(f.area + monsterlayout::kWorldBossCount, uint64_t(65));
      bool rejected = false; try { monster::Reader<Memory>(f.mem, f.base).Catalog(f.world, f.area); } catch (...) { rejected = true; }
      CHECK(rejected); }
    { Fixture f; const auto pairs = f.Parts(f.entity, {{"Monster", f.monster}, {"ObjectMagicProperties", f.magic}, {"Life", f.life}});
      f.mem.Put(pairs + kComponentPairSize + kComponentPairIndexOff, uint32_t(0)); CHECK(f.Reject()); }
    { Fixture f; CHECK(f.Reject() == false);
      const auto proxy = f.mem.Alloc(128), pos = f.mem.Alloc(1280); f.mem.Put(pos + monsterlayout::kComponentOwner, proxy);
      f.Parts(proxy, {{"Monster", f.monster}, {"ObjectMagicProperties", f.magic}, {"Life", f.life}, {"Positioned", pos}});
      uint64_t owner = 0; std::string name;
      const auto e = monster::Reader<Memory>(f.mem, f.base).Identity(proxy, f.metadata, f.catalog, owner, name);
      CHECK(owner == f.entity); CHECK(monster::Role(e) == "shared_monster_proxy");
      CHECK(!monster::Destination(e) && !monster::OptionalElite(e));
      f.mem.Put(pos + monsterlayout::kComponentOwner, f.entity);
      bool rejected = false; try { monster::Reader<Memory>(f.mem, f.base).Identity(proxy, f.metadata, f.catalog, owner, name); } catch (...) { rejected = true; }
      CHECK(rejected);
    }
}
double Length(const std::vector<size_t>& order, const std::vector<std::vector<double>>& d,
              const std::vector<double>& start, const std::vector<double>& tail) {
    double total = start[order.front()] + tail[order.back()];
    for (size_t i = 1; i < order.size(); ++i) total += d[order[i - 1]][order[i]]; return total;
}
void Routes() {
    using encounterroute::Kind;
    CHECK(encounterroute::Mixed(Kind::Ritual, false)); CHECK(!encounterroute::Mixed(Kind::Elite, false));
    CHECK(encounterroute::Mixed(Kind::Elite, true));
    CHECK(!encounterroute::Mixed(Kind::MapBoss, true)); CHECK(!encounterroute::Mixed(Kind::BossEntrance, true));
    // Mixed ritual/elite labels do not affect ordering. A far-away boss tail
    // changes the best final mixed node, without entering the permutation.
    std::vector<std::vector<double>> d{{0, 10}, {10, 0}};
    CHECK(encounterroute::VisitOrder(d, {1, 1}, {0, 100}) == std::vector<size_t>({1, 0}));
    CHECK(encounterroute::VisitOrder(d, {1, 1}, {100, 0}) == std::vector<size_t>({0, 1}));
    CHECK(encounterroute::VisitOrder(d, {1, 1}, {INFINITY, INFINITY}).empty());
    CHECK(encounterroute::VisitOrder(d, {1}, {0}).empty());
    std::mt19937 random(42);
    for (unsigned trial = 0; trial < 15; ++trial) {
        constexpr size_t n = 6; d.assign(n, std::vector<double>(n));
        std::vector<double> start(n), tail(n);
        for (size_t i = 0; i < n; ++i) {
            start[i] = random() % 100 + 1; tail[i] = random() % 100 + 1;
            for (size_t j = 0; j < i; ++j) d[i][j] = d[j][i] = random() % 100 + 1;
        }
        const auto order = encounterroute::VisitOrder(d, start, tail);
        std::vector<size_t> permutation(n); std::iota(permutation.begin(), permutation.end(), 0);
        double optimum = INFINITY;
        do { optimum = (std::min)(optimum, Length(permutation, d, start, tail)); }
        while (std::next_permutation(permutation.begin(), permutation.end()));
        CHECK(order.size() == n); CHECK(Length(order, d, start, tail) == optimum);
    }
    constexpr size_t n = 100; d.assign(n, std::vector<double>(n));
    std::vector<double> start(n), tail(n);
    for (size_t i = 0; i < n; ++i) {
        start[i] = double(i + 1); tail[i] = double(n - i);
        for (size_t j = 0; j < n; ++j) d[i][j] = std::abs(double(i) - double(j));
    }
    auto order = encounterroute::VisitOrder(d, start, tail); CHECK(order.size() == n);
    auto sorted = order; std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK(order.front() == 0 && order.back() == n - 1);
}
} // namespace
int main() {
    try { Identity(); InvalidLayouts(); Routes(); }
    catch (const std::exception& e) { ++failed; std::printf("UNEXPECTED: %s\n", e.what()); }
    std::printf("Encounter tests: %d passed, %d failed\n", passed, failed); return failed ? 1 : 0;
}
