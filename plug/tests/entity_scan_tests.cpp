// Reads synthetic entities in this test process only; no game or injection.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../Plug/Surroundings.h"
#include "../Common/MonsterReader.h"
#include "../Common/RunDiagnostics.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
struct Fixture {
    std::vector<void*> blocks;
    uint64_t base = 0, sentinel = 0;
    std::vector<uint64_t> nodes;
    ola::OlaDriver driver;
    mem::GameMemory memory{driver};
    uint64_t Alloc(size_t size = 0x3000) {
        void* block = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!block) throw std::runtime_error("test allocation failed");
        blocks.push_back(block); return uint64_t(block);
    }
    template<class T> void Put(uint64_t address, T value) { memcpy(reinterpret_cast<void*>(address), &value, sizeof(value)); }
    uint64_t Ascii(const std::string& value) {
        auto p = Alloc(); memcpy(reinterpret_cast<void*>(p), value.c_str(), value.size() + 1); return p;
    }
    void WideString(uint64_t object, const std::string& value) {
        const auto data = Alloc();
        for (size_t i = 0; i < value.size(); ++i) Put(data + i * 2, uint16_t(uint8_t(value[i])));
        Put(object, data); Put(object + kStdStringLengthOff, uint64_t(value.size()));
        Put(object + kStdStringMaxLenOff, uint64_t(value.size() + 8));
    }
    Fixture() {
        base = uint64_t(VirtualAlloc(nullptr, BaseOffset + 0x2000, MEM_RESERVE, PAGE_READWRITE));
        if (!base) throw std::runtime_error("module reserve failed");
        blocks.push_back(reinterpret_cast<void*>(base));
        if (!VirtualAlloc(reinterpret_cast<void*>((base + BaseOffset) & ~uint64_t(4095)), 8192, MEM_COMMIT, PAGE_READWRITE))
            throw std::runtime_error("module commit failed");
        auto current = base;
        for (auto offset : kSurroundingsChain) { const auto next = Alloc(); Put(current + offset, next); current = next; }
        sentinel = current;
        Put(sentinel + kNodeFlagOff, uint8_t(1)); Put(sentinel + kNodeParentOff, sentinel);
        if (!driver.InitializeLocal(GetCurrentProcessId())) throw std::runtime_error("self RPM failed");
    }
    ~Fixture() { for (auto block : blocks) VirtualFree(block, 0, MEM_RELEASE); }
    struct Mob { uint64_t entity = 0, life = 0, pos = 0, node = 0, pairs = 0, header = 0; };
    Mob Add(uint32_t key, const std::string& path, bool isMonster, bool hasLife = true) {
        Mob mob; mob.entity = Alloc(); mob.pos = Alloc(); mob.life = hasLife ? Alloc() : 0;
        const auto info = Alloc(); mob.header = Alloc(); mob.pairs = Alloc(); const auto array = Alloc();
        Put(mob.entity + kEntityPairsOff0, info); Put(info + kEntityPairsOff1, mob.header);
        WideString(info + kMetadataPathOff1, path);
        std::vector<std::pair<std::string, uint64_t>> parts{{"Positioned", mob.pos}};
        if (isMonster) parts.push_back({"Monster", Alloc()});
        if (mob.life) parts.push_back({"Life", mob.life});
        Put(mob.header + kEntityPairsOff2, mob.pairs);
        Put(mob.header + kEntityPairsEndOff, mob.pairs + parts.size() * kComponentPairSize);
        Put(mob.entity + kEntityComponentsOff, array);
        Put(mob.entity + kEntityComponentsEndOff, array + parts.size() * kComponentPtrSize);
        for (size_t i = 0; i < parts.size(); ++i) {
            Put(mob.pairs + i * kComponentPairSize, Ascii(parts[i].first));
            Put(mob.pairs + i * kComponentPairSize + kComponentPairIndexOff, uint32_t(i));
            Put(array + i * kComponentPtrSize, parts[i].second);
            Put(parts[i].second + monsterlayout::kComponentOwner, mob.entity);
        }
        Put(mob.pos + kPositioned_EntXOff, int32_t(10 + key)); Put(mob.pos + kPositioned_EntYOff, int32_t(10));
        if (mob.life) Put(mob.life + kLife_HpOff, uint32_t(100));
        mob.node = Alloc(); Put(mob.node + kNodeKeyOff, key); Put(mob.node + kNodeEntityOff, mob.entity);
        Put(mob.node + kNodeLeftOff, sentinel); Put(mob.node + kNodeRightOff, sentinel);
        if (nodes.empty()) Put(sentinel + kNodeParentOff, mob.node);
        else Put(nodes.back() + kNodeRightOff, mob.node);
        nodes.push_back(mob.node); return mob;
    }
};
void ScanCompatibility() {
    Fixture f;
    const auto alive = f.Add(1, "Metadata/Monsters/Test/Live", true);
    f.Add(2, "Metadata/Monsters/TwilightOrder/Objects/PillarLeft", false, false);
    const auto corpse = f.Add(3, "Metadata/Monsters/Test/Dead", true);
    f.Put(corpse.life + kLife_HpOff, uint32_t(0));
    const auto missingLife = f.Add(4, "Metadata/Monsters/Test/Unloading", true, false);
    const auto freed = f.Add(5, "Metadata/Monsters/Test/Freed", true);
    f.Put(freed.entity + kEntityPairsOff0, uint64_t(0));
    const auto corrupt = f.Add(6, "Metadata/Monsters/Test/Corrupt", true);
    f.Put(corrupt.pairs + kComponentPairSize + kComponentPairIndexOff, uint32_t(0));
    const auto metadataGone = f.Add(7, "Metadata/Monsters/Test/Gone", true);
    f.Put(metadataGone.node + kNodeEntityOff, uint64_t(0));
    std::vector<poe::EntitySnapshot> entities; poe::EntityScanInfo info;
    poe::SurroundingsTraverser scanner(f.memory);
    CHECK(scanner.ListMonsters(f.base, entities, info));
    CHECK(entities.size() == 2 && entities[0].key == 1 && entities[1].key == 3);
    CHECK(entities[0].hp == 100 && entities[0].combat.hpValid);
    CHECK(entities[1].hp == 0 && monster::ConfirmedDead(entities[1].combat));
    CHECK(info.nonCombat == 1 && info.unreadable == 4 && !info.Complete());
    CHECK(info.error.empty() && info.issues.size() == 5);
    CHECK(poe::ScanSummary(info, entities.size()).find("PillarLeft") != std::string::npos);
    // A cleaned up monster never appears as a confirmed zero-HP target.
    CHECK(std::none_of(entities.begin(), entities.end(), [&](auto& e) { return e.entity == missingLife.entity; }));
    // Zero HP is included only while Life is readable. Its later removal must
    // leave the other living monster selectable, with a partial-scan marker.
    f.Put(corpse.entity + kEntityPairsOff0, uint64_t(0));
    CHECK(scanner.ListMonsters(f.base, entities, info) && entities.size() == 1 && entities[0].entity == alive.entity);
    CHECK(info.unreadable == 5 && !info.Complete());
    CHECK(!scanner.ListWorldItems(f.base, entities, info) && entities.empty());
    // Ritual observations also tolerate unrelated entities being cleaned up.
    f.Add(8, "Metadata/Terrain/Leagues/Ritual/RitualRuneObject", false);
    CHECK(scanner.ListRitualCandidates(f.base, entities, info));
    CHECK(entities.size() == 1 && entities[0].key == 8 && !entities[0].ritual.valid);
    CHECK(info.unreadable >= 1);
    const auto inaccessible = f.Add(9, "Metadata/Monsters/Test/UnreadableHp", true);
    DWORD protection = 0;
    CHECK(VirtualProtect(reinterpret_cast<void*>(inaccessible.life), 4096, PAGE_NOACCESS, &protection));
    CHECK(scanner.ListMonsters(f.base, entities, info) && entities.size() == 1 && info.unreadable == 6);
    CHECK(VirtualProtect(reinterpret_cast<void*>(inaccessible.life), 4096, protection, &protection));
    const auto proxy = f.Add(10, "Metadata/Monsters/Test/Proxy", true);
    const auto array = f.memory.Follow(proxy.entity + kEntityComponentsOff);
    const auto component = f.memory.Follow(array + kComponentPtrSize);
    f.Put(component + monsterlayout::kComponentOwner, alive.entity);
    CHECK(scanner.ListMonsters(f.base, entities, info) && entities.size() == 2 && info.nonCombat == 2);
    CHECK(std::none_of(entities.begin(), entities.end(), [](auto& e) { return e.key == 10; }));
    // A dangling child from a concurrently removed tree node also preserves
    // valid siblings and advertises that this was a partial sample.
    f.Put(f.nodes.back() + kNodeRightOff, uint64_t(1));
    CHECK(scanner.ListMonsters(f.base, entities, info) && entities.size() == 2 && info.unreadable == 6);
    f.Put(f.nodes.back() + kNodeRightOff, f.sentinel);
    std::atomic_bool stop{true};
    CHECK(!scanner.ListMonsters(f.base, entities, info, &stop) && entities.empty());
    f.Put(f.nodes.back() + kNodeRightOff, f.nodes.front());
    CHECK(!scanner.ListMonsters(f.base, entities, info) && entities.empty());
    Fixture unknown; unknown.Add(1, "Metadata/Monsters/Test/Unloading", true, false);
    CHECK(poe::SurroundingsTraverser(unknown.memory).ListMonsters(unknown.base, entities, info));
    CHECK(entities.empty() && !info.Complete() && info.unreadable == 1);
    Fixture good; good.Add(1, "Metadata/Monsters/Test/Alive", true);
    CHECK(poe::SurroundingsTraverser(good.memory).ListMonsters(good.base, entities, info));
    CHECK(info.Complete() && entities.size() == 1 && entities[0].hp == 100);
}
std::string Read(const std::wstring& path) {
    std::ifstream file{std::filesystem::path(path), std::ios::binary};
    return std::string(std::istreambuf_iterator<char>(file), {});
}
void LogPersistence() {
    const auto folder = std::filesystem::current_path().wstring() + L"\\diagnostics_" + std::to_wstring(GetCurrentProcessId());
    std::wstring first;
    {
        poelog::RunDiagnostics a(folder, L"test"), b(folder, L"test");
        CHECK(a.Good() && b.Good() && a.Path() != b.Path()); first = a.Path();
        a.Write("begin", "quotes\" slash\\ newline\n\xE4\xB8\xAD");
        a.Changed("scan", "unchanged");
        for (int i = 0; i < 1000; ++i) a.Changed("scan", "unchanged", 60000);
        a.Changed("scan", "changed");
        auto content = Read(first); // Active file permits external reading.
        CHECK(content.find("\\\"") != std::string::npos && content.find("\\u000a") != std::string::npos);
        CHECK(content.find("\"suppressed\":1000") != std::string::npos);
        CHECK(std::count(content.begin(), content.end(), '\n') == 3);
        std::vector<std::thread> writers;
        for (int i = 0; i < 4; ++i) writers.emplace_back([&] { for (int j = 0; j < 20; ++j) a.Write("thread", "value"); });
        for (auto& writer : writers) writer.join();
        content = Read(first); CHECK(std::count(content.begin(), content.end(), '\n') == 83);
        for (int i = 0; i < 10; ++i) b.Write("rotation", std::string(1024 * 1024, 'x'));
        CHECK(std::filesystem::exists(b.Path().substr(0, b.Path().size() - 6) + L"_part2.jsonl"));
        CHECK(std::filesystem::file_size(b.Path()) <= 8 * 1024 * 1024);
    }
    poelog::RunDiagnostics next(folder, L"test"); next.Write("next", "new run");
    CHECK(next.Path() != first && Read(first).find("\"event\":\"begin\"") != std::string::npos);
    poelog::RunDiagnostics bad(folder + L"\\missing\\nested"); CHECK(!bad.Good());
    bad.Write("ignored", "failure does not crash the caller");
}
}
int main() {
    try { ScanCompatibility(); LogPersistence(); }
    catch (const std::exception& e) { ++failed; printf("UNEXPECTED: %s\n", e.what()); }
    printf("Entity scan/log tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
