#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../../Common/GameBuild.h"
#include "BossClassifier.h"
#include "ProbeOffsets.h"

static_assert(sizeof(void*) == 8 && sizeof(wchar_t) == 2, "Windows x64 required");
namespace fs = std::filesystem;
using namespace bossprobe;

namespace {
std::string Hex(uint64_t v) {
    std::ostringstream s; s << "0x" << std::hex << std::uppercase << v; return s.str();
}
std::string Json(const std::string& v) {
    std::ostringstream s; s << '"';
    for (unsigned char ch : v) {
        if (ch == '"' || ch == '\\') s << '\\' << ch;
        else if (ch < 0x20) s << "\\u00" << std::hex << std::setw(2)
                             << std::setfill('0') << static_cast<unsigned>(ch);
        else s << ch;
    }
    s << '"'; return s.str();
}
std::string Utf8(const std::wstring& v) {
    if (v.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, v.data(),
        static_cast<int>(v.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid UTF-16");
    std::string s(n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, v.data(),
        static_cast<int>(v.size()), s.data(), n, nullptr, nullptr))
        throw std::runtime_error("UTF-16 conversion failed");
    return s;
}
std::string Canonical(std::string s) {
    const auto at = s.rfind('@');
    if (at != std::string::npos && at + 1 < s.size() &&
        std::all_of(s.begin() + at + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; }))
        s.resize(at);
    return s;
}
std::string Now(bool filename = false) {
    SYSTEMTIME utc{}, local{}; FILETIME ft{}; GetSystemTime(&utc);
    if (!SystemTimeToFileTime(&utc, &ft)) throw std::runtime_error("Clock read failed");
    ULARGE_INTEGER t{}; t.LowPart = ft.dwLowDateTime; t.HighPart = ft.dwHighDateTime;
    t.QuadPart += 8ULL * 60 * 60 * 10000000;
    ft.dwLowDateTime = t.LowPart; ft.dwHighDateTime = t.HighPart;
    FileTimeToSystemTime(&ft, &local);
    char s[64]{};
    if (filename) std::snprintf(s, sizeof(s), "%04u%02u%02u-%02u%02u%02u-%03u",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    else std::snprintf(s, sizeof(s), "%04u-%02u-%02uT%02u:%02u:%02u.%03u+08:00",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    return s;
}
struct Handle {
    HANDLE h;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
bool GameName(const wchar_t* s) {
    return !_wcsicmp(s, L"PathOfExile.exe") || !_wcsicmp(s, L"PathOfExileSteam.exe") ||
           !_wcsicmp(s, L"PathOfExile_x64.exe");
}
DWORD FindGame() {
    Handle h(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (h.h == INVALID_HANDLE_VALUE) throw std::runtime_error("Process snapshot failed");
    PROCESSENTRY32W e{}; e.dwSize = sizeof(e); std::vector<DWORD> ids;
    if (Process32FirstW(h.h, &e)) do {
        if (GameName(e.szExeFile)) ids.push_back(e.th32ProcessID);
    } while (Process32NextW(h.h, &e));
    if (ids.empty()) throw std::runtime_error("Game not running");
    if (ids.size() != 1) throw std::runtime_error("Multiple games; specify --pid");
    return ids.front();
}
class Process {
    Handle handle;
public:
    DWORD pid;
    uint64_t base = 0;
    explicit Process(DWORD id) : handle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, id)), pid(id) {
        if (!handle.h) throw std::runtime_error("OpenProcess failed, Win32=" + std::to_string(GetLastError()));
        std::array<wchar_t, 32768> path{}; DWORD n = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(handle.h, 0, path.data(), &n) ||
            !GameName(fs::path(path.data()).filename().c_str()))
            throw std::runtime_error("Selected PID is not a supported game executable");
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
        MODULEENTRY32W module{}; module.dwSize = sizeof(module);
        if (snapshot.h == INVALID_HANDLE_VALUE || !Module32FirstW(snapshot.h, &module))
            throw std::runtime_error("Module snapshot failed");
        do {
            if (!_wcsicmp(module.szExePath, path.data())) {
                base = reinterpret_cast<uint64_t>(module.modBaseAddr); break;
            }
        } while (Module32NextW(snapshot.h, &module));
        if (!base) throw std::runtime_error("Main module not found");
        std::string err;
        if (!gamebuild::Verify(base, [this](uint64_t a, void* b, size_t size) { return ReadInto(a, b, size); }, err))
            throw std::runtime_error(err);
    }
    bool ReadInto(uint64_t a, void* out, size_t n) const {
        if (!out || !n || n > 1024 * 1024 || !IsPlausibleAddress(a) ||
            a > UINT64_MAX - n || !IsPlausibleAddress(a + n - 1)) return false;
        SIZE_T done = 0;
        return ReadProcessMemory(handle.h, reinterpret_cast<LPCVOID>(a), out, n, &done) && done == n;
    }
    template<class T> T Read(uint64_t a) const {
        T v{};
        if (!ReadInto(a, &v, sizeof(v))) throw std::runtime_error("Unreadable memory at " + Hex(a));
        return v;
    }
    uint64_t Ptr(uint64_t a) const {
        const auto v = Read<uint64_t>(a);
        if (!IsPlausibleAddress(v)) throw std::runtime_error("Invalid pointer at " + Hex(a));
        return v;
    }
    std::string Wide(uint64_t a) const {
        std::wstring s;
        for (unsigned i = 0; i < 512; ++i) {
            auto ch = Read<wchar_t>(a + 2ULL * i);
            if (!ch) return Utf8(s);
            if (ch < 0x20) throw std::runtime_error("Control character in text");
            s += ch;
        }
        throw std::runtime_error("Unterminated wide text");
    }
    std::string StdWide(uint64_t a) const {
        auto length = Read<uint64_t>(a + 0x10), capacity = Read<uint64_t>(a + 0x18);
        if (length > 512 || capacity < length || capacity > 1048576)
            throw std::runtime_error("Invalid std::wstring at " + Hex(a));
        if (!length) return {};
        auto data = capacity > 7 ? Ptr(a) : a;
        std::wstring s(static_cast<size_t>(length), L'\0');
        if (!ReadInto(data, s.data(), s.size() * sizeof(wchar_t)))
            throw std::runtime_error("Unreadable std::wstring");
        if (std::any_of(s.begin(), s.end(), [](wchar_t c) { return c < 0x20; }))
            throw std::runtime_error("Invalid std::wstring characters");
        return Utf8(s);
    }
    std::string Ascii(uint64_t a) const {
        std::string s;
        for (unsigned i = 0; i < 96; ++i) {
            auto c = Read<unsigned char>(a + i);
            if (!c) return s;
            if (c < 0x20 || c > 0x7e) throw std::runtime_error("Invalid component name");
            s += static_cast<char>(c);
        }
        throw std::runtime_error("Unterminated component name");
    }
    void Table(uint64_t table, const std::string& expected) const {
        if (!IsPlausibleAddress(table) || StdWide(table + 8) != expected)
            throw std::runtime_error("Data table identity mismatch: " + expected);
    }
    std::vector<uint64_t> Vector(uint64_t a, uint64_t stride, size_t limit) const {
        auto b = Read<uint64_t>(a), e = Read<uint64_t>(a + 8), c = Read<uint64_t>(a + 16);
        if (!b && !e && !c) return {};
        if (!IsPlausibleAddress(b) || e < b || c < e || (e - b) % stride || (c - b) % stride ||
            c - b > stride * limit) throw std::runtime_error("Invalid vector bounds at " + Hex(a));
        std::vector<uint64_t> result;
        for (auto v = b; v < e; v += stride) result.push_back(v);
        return result;
    }
};

struct ExpectedBoss { uint64_t row = 0; std::string metadata, name; };
struct Scene {
    uint64_t world = 0, sentinel = 0, record = 0, player = 0;
    int32_t px = 0, py = 0;
    bool playerPosition = false;
    std::string id, name;
    std::vector<ExpectedBoss> expected;
};
using Components = std::map<std::string, uint64_t>;
Components ReadComponents(const Process& p, uint64_t entity) {
    auto header = p.Ptr(p.Ptr(entity + kEntityPairsOff0) + kEntityPairsOff1);
    auto b = p.Ptr(header + kEntityPairsOff2), e = p.Read<uint64_t>(header + kEntityPairsEndOff);
    auto cb = p.Ptr(entity + kEntityComponentsOff), ce = p.Read<uint64_t>(entity + kEntityComponentsEndOff);
    if (e <= b || (e - b) % kComponentPairSize || (e - b) / kComponentPairSize > kMaxComponentCount ||
        ce < cb || (ce - cb) / kComponentPtrSize != (e - b) / kComponentPairSize ||
        (ce - cb) % kComponentPtrSize) throw std::runtime_error("Component table bounds mismatch");
    const auto count = (e - b) / kComponentPairSize;
    Components out; std::unordered_set<uint32_t> indices;
    for (auto slot = b; slot < e; slot += kComponentPairSize) {
        auto name = p.Ascii(p.Ptr(slot));
        auto index = p.Read<uint32_t>(slot + kComponentPairIndexOff);
        if (name.empty() || index >= count || !indices.insert(index).second || out.count(name))
            throw std::runtime_error("Invalid/duplicate component index/name");
        auto component = p.Ptr(cb + index * kComponentPtrSize);
        out.emplace(std::move(name), component);
    }
    return out;
}
void ValidateComponents(const Process& p, uint64_t entity, const Components& c,
                        uint64_t sharedOwner = 0, const Components* ownerComponents = nullptr) {
    for (const auto& [name, address] : c) {
        const auto owner = p.Read<uint64_t>(address + 8);
        if (owner == entity) continue;
        if (name != "Positioned" && sharedOwner && owner == sharedOwner && ownerComponents) {
            auto i = ownerComponents->find(name);
            if (i != ownerComponents->end() && i->second == address) continue;
        }
        throw std::runtime_error("Component owner mismatch: " + name + " entity=" + Hex(entity) + " owner=" + Hex(owner));
    }
}
Scene ReadScene(const Process& p) {
    Scene s; s.world = p.base;
    for (int i = 0; i < kSurroundingsChainDepth - 1; ++i) s.world = p.Ptr(s.world + kSurroundingsChain[i]);
    s.sentinel = p.Ptr(s.world + kSurroundingsChain[kSurroundingsChainDepth - 1]);
    s.record = p.Ptr(s.world + kWorldAreaRecordOff);
    p.Table(p.Ptr(s.world + kWorldAreaTableOff), "Data/Balance/WorldAreas.dat");
    s.id = p.Wide(p.Ptr(s.record + kWorldAreaIdOff)); s.name = p.Wide(p.Ptr(s.record + kWorldAreaNameOff));
    if (s.id.empty() || s.name.empty()) throw std::runtime_error("Missing world area identity");
    auto n = p.Read<uint64_t>(s.record + kWorldBossCount), data = p.Read<uint64_t>(s.record + kWorldBossData);
    if (n > 64 || (n && !IsPlausibleAddress(data))) throw std::runtime_error("Invalid area boss list");
    for (uint64_t i = 0; i < n; ++i) {
        auto row = p.Ptr(data + i * kReferenceStride);
        p.Table(p.Ptr(data + i * kReferenceStride + 8), "Data/Balance/MonsterVarieties.dat");
        ExpectedBoss b; b.row = row;
        b.metadata = p.Wide(p.Ptr(row)); b.name = p.Wide(p.Ptr(row + kVarietyName));
        if (b.metadata.find("Metadata/Monsters/") != 0 || b.name.empty())
            throw std::runtime_error("Invalid area boss variety");
        s.expected.push_back(std::move(b));
    }
    s.player = p.Ptr(p.Ptr(s.world + 0x5C0));
    auto components = ReadComponents(p, s.player);
    ValidateComponents(p, s.player, components);
    auto pos = components.find("Positioned");
    if (pos != components.end()) {
        s.px = p.Read<int32_t>(pos->second + kPositioned_EntXOff);
        s.py = p.Read<int32_t>(pos->second + kPositioned_EntYOff);
        s.playerPosition = true;
    }
    return s;
}
bool SameScene(const Scene& a, const Scene& b) {
    if (a.world != b.world || a.sentinel != b.sentinel || a.record != b.record || a.player != b.player ||
        a.id != b.id || a.name != b.name || a.expected.size() != b.expected.size()) return false;
    for (size_t i = 0; i < a.expected.size(); ++i)
        if (a.expected[i].row != b.expected[i].row || a.expected[i].metadata != b.expected[i].metadata) return false;
    return true;
}
struct Node {
    uint64_t node = 0, left = 0, right = 0, parent = 0, entity = 0;
    uint32_t key = 0;
    bool operator==(const Node& b) const {
        return node == b.node && left == b.left && right == b.right && parent == b.parent && entity == b.entity && key == b.key;
    }
};
std::vector<Node> ReadTree(const Process& p, uint64_t head) {
    if (p.Read<uint8_t>(head + kNodeFlagOff) != 1) throw std::runtime_error("Invalid surroundings sentinel");
    std::vector<std::pair<uint64_t, uint64_t>> pending{{p.Ptr(head + kNodeParentOff), head}};
    std::unordered_set<uint64_t> seen; std::vector<Node> out;
    while (!pending.empty()) {
        auto [a, parent] = pending.back(); pending.pop_back();
        if (a == head) continue;
        if (!seen.insert(a).second || seen.size() > 8192) throw std::runtime_error("Tree cycle/node limit");
        if (p.Read<uint8_t>(a + kNodeFlagOff) != 0) throw std::runtime_error("Invalid tree node flag");
        Node n; n.node = a; n.parent = p.Read<uint64_t>(a + kNodeParentOff);
        if (n.parent != parent) throw std::runtime_error("Tree changed: parent mismatch");
        n.left = p.Ptr(a + kNodeLeftOff); n.right = p.Ptr(a + kNodeRightOff);
        n.key = p.Read<uint32_t>(a + kNodeKeyOff); n.entity = p.Ptr(a + kNodeEntityOff);
        pending.emplace_back(n.right, a); pending.emplace_back(n.left, a); out.push_back(n);
    }
    std::sort(out.begin(), out.end(), [](const Node& a, const Node& b) { return a.node < b.node; });
    return out;
}

struct ModDef { std::string id; std::vector<std::string> stats; };
struct Stat { std::string mod, name; int32_t value = 0; };
struct Row {
    uint32_t key = 0, maxHp = 0;
    uint64_t entity = 0, variety = 0, combatOwner = 0;
    std::string metadata, name;
    int32_t x = 0, y = 0;
    double distance = -1;
    Evidence evidence;
    bool metadataBoss = false, minionMod = false;
    std::vector<std::string> mods;
    std::vector<Stat> stats;
};
class Sampler {
    const Process& p;
    const std::unordered_set<uint64_t>& loaded;
    std::unordered_map<uint64_t, ModDef> definitions;
    const ModDef& Definition(uint64_t row, uint64_t table) {
        p.Table(table, "Data/Balance/Mods.dat");
        auto it = definitions.find(row); if (it != definitions.end()) return it->second;
        ModDef d; d.id = p.Wide(p.Ptr(row));
        if (d.id.empty()) throw std::runtime_error("Empty mod ID");
        bool ended = false;
        for (unsigned i = 0; i < kModStatSlots; ++i) {
            auto a = row + kModStatRefs + i * kReferenceStride;
            auto stat = p.Read<uint64_t>(a);
            if (!stat) { ended = true; continue; }
            if (ended) throw std::runtime_error("Noncontiguous mod stat slots");
            p.Table(p.Ptr(a + 8), "Data/Balance/Stats.dat");
            auto name = p.Wide(p.Ptr(stat));
            if (name.empty()) throw std::runtime_error("Empty stat ID");
            d.stats.push_back(std::move(name));
        }
        return definitions.emplace(row, std::move(d)).first->second;
    }
public:
    Sampler(const Process& process, const std::unordered_set<uint64_t>& entities) : p(process), loaded(entities) {}
    Row Entity(const Node& node, const Scene& s, std::string metadata) {
        Row r; r.key = node.key; r.entity = node.entity; r.metadata = std::move(metadata);
        r.metadataBoss = r.metadata.find("Boss") != std::string::npos;
        auto c = ReadComponents(p, r.entity);
        r.evidence.monster = c.count("Monster") != 0;
        r.combatOwner = r.entity;
        if (r.evidence.monster) r.combatOwner = p.Ptr(c.at("Monster") + 8);
        if (r.combatOwner == r.entity) ValidateComponents(p, r.entity, c);
        else {
            if (!loaded.count(r.combatOwner)) throw std::runtime_error("Shared monster owner absent from current tree");
            auto ownerComponents = ReadComponents(p, r.combatOwner);
            ValidateComponents(p, r.combatOwner, ownerComponents);
            if (!ownerComponents.count("Monster") || ownerComponents.at("Monster") != c.at("Monster") ||
                p.StdWide(p.Ptr(r.combatOwner + kMetadataPathOff0) + kMetadataPathOff1) != r.metadata)
                throw std::runtime_error("Shared monster identity mismatch");
            ValidateComponents(p, r.entity, c, r.combatOwner, &ownerComponents);
            r.evidence.isCombatOwner = false;
        }
        if (auto i = c.find("Render"); i != c.end()) r.name = p.StdWide(i->second + kRender_NameOff);
        if (auto i = c.find("Positioned"); i != c.end()) {
            r.x = p.Read<int32_t>(i->second + kPositioned_EntXOff);
            r.y = p.Read<int32_t>(i->second + kPositioned_EntYOff);
            r.evidence.positionValid = r.x >= 0 && r.y >= 0 && r.x < 1000000 && r.y < 1000000;
            if (r.evidence.positionValid && s.playerPosition)
                r.distance = std::hypot(double(r.x) - s.px, double(r.y) - s.py);
        }
        if (!r.evidence.monster) return r;
        if (auto i = c.find("Life"); i != c.end()) {
            r.evidence.hp = p.Read<uint32_t>(i->second + kLife_HpOff);
            r.maxHp = p.Read<uint32_t>(i->second + kLife_MaxHpOff);
            r.evidence.hpValid = true;
        }
        auto monster = c.at("Monster");
        if (p.Read<uint64_t>(monster) != p.base + kMonsterVtable) throw std::runtime_error("Monster vtable mismatch");
        auto data = p.Ptr(monster + kMonsterData); r.variety = p.Ptr(data + kVarietyRow);
        p.Table(p.Ptr(data + kVarietyTable), "Data/Balance/MonsterVarieties.dat");
        auto varietyId = p.Wide(p.Ptr(r.variety));
        if (Canonical(r.metadata) != varietyId) throw std::runtime_error("Entity/variety metadata mismatch");
        for (const auto& b : s.expected)
            if (b.row == r.variety || b.metadata == varietyId) r.evidence.configuredForArea = true;
        auto om = c.find("ObjectMagicProperties");
        if (om == c.end()) { r.evidence.valid = false; return r; }
        if (p.Read<uint64_t>(om->second) != p.base + kMagicVtable) throw std::runtime_error("Magic-properties vtable mismatch");
        r.evidence.rarity = p.Read<uint32_t>(om->second + kMagicRarity);
        if (r.evidence.rarity > 3) throw std::runtime_error("Invalid monster rarity");
        bool gotTier = false;
        for (auto entry : p.Vector(om->second + kRuntimeMods, kRuntimeModStride, 256)) {
            const auto& mod = Definition(p.Ptr(entry + kRuntimeModRow), p.Ptr(entry + kRuntimeModTable));
            auto values = p.Vector(entry, 4, kModStatSlots);
            if (values.size() != mod.stats.size()) throw std::runtime_error("Mod stat/value count mismatch: " + mod.id);
            r.mods.push_back(mod.id);
            if (mod.id.find("BossMinionFlaskChargeIncrease") == 0) r.minionMod = true;
            for (size_t i = 0; i < values.size(); ++i) {
                auto value = p.Read<int32_t>(values[i]); const auto& name = mod.stats[i];
                if (name == "monster_rarity" && value != static_cast<int32_t>(r.evidence.rarity))
                    throw std::runtime_error("Rarity field/runtime modifier mismatch");
                if (name.find("boss") != std::string::npos || name == "monster_rarity")
                    r.stats.push_back({mod.id, name, value});
                if (name == "i_am_boss_of_tier") {
                    if (gotTier && r.evidence.bossTier != value) throw std::runtime_error("Conflicting runtime boss tiers");
                    r.evidence.bossTier = value; gotTier = true;
                }
            }
        }
        return r;
    }
};
struct Snapshot {
    Scene scene; size_t nodes = 0; std::vector<Row> rows;
    std::vector<Row> checkpoints;
    std::string time, status;
};
Snapshot Capture(const Process& p) {
    std::string error;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        try {
            Snapshot v; v.time = Now(); v.scene = ReadScene(p);
            auto nodes = ReadTree(p, v.scene.sentinel); v.nodes = nodes.size();
            std::unordered_set<uint64_t> entities;
            for (const auto& n : nodes) entities.insert(n.entity);
            Sampler sample(p, entities);
            if (std::none_of(nodes.begin(), nodes.end(), [&](const Node& n) { return n.entity == v.scene.player; }))
                throw std::runtime_error("Local player absent from tree; scene may be loading");
            for (const auto& n : nodes) {
                auto metadata = p.StdWide(p.Ptr(n.entity + kMetadataPathOff0) + kMetadataPathOff1);
                if (metadata.empty()) throw std::runtime_error("Empty entity metadata");
                if (metadata.find("Metadata/Monsters/") == 0)
                    v.rows.push_back(sample.Entity(n, v.scene, std::move(metadata)));
                else if (metadata.find("Metadata/MiscellaneousObjects/Checkpoints/") == 0)
                    v.checkpoints.push_back(sample.Entity(n, v.scene, std::move(metadata)));
            }
            if (nodes != ReadTree(p, v.scene.sentinel) || !SameScene(v.scene, ReadScene(p)))
                throw std::runtime_error("Scene/tree changed during scan");
            // Refresh combat state last; zero HP stays a recorded boss, never a destination.
            for (auto& r : v.rows) if (r.evidence.configuredForArea) {
                auto c = ReadComponents(p, r.entity);
                if (r.evidence.hpValid) r.evidence.hp = p.Read<uint32_t>(c.at("Life") + kLife_HpOff);
                if (r.evidence.positionValid) {
                    r.x = p.Read<int32_t>(c.at("Positioned") + kPositioned_EntXOff);
                    r.y = p.Read<int32_t>(c.at("Positioned") + kPositioned_EntYOff);
                    r.evidence.positionValid = r.x >= 0 && r.y >= 0 && r.x < 1000000 && r.y < 1000000;
                    if (v.scene.playerPosition) r.distance = std::hypot(double(r.x) - v.scene.px, double(r.y) - v.scene.py);
                }
            }
            const auto configuredSeen = std::count_if(v.rows.begin(), v.rows.end(), [](const Row& r) {
                return r.evidence.configuredForArea;
            });
            if (!SameScene(v.scene, ReadScene(p)) || (configuredSeen && nodes != ReadTree(p, v.scene.sentinel)))
                throw std::runtime_error("Scene/tree changed after target validation");
            auto count = [&](const std::string& role) {
                return std::count_if(v.rows.begin(), v.rows.end(), [&](const Row& r) { return Role(r.evidence) == role; });
            };
            auto targets = std::count_if(v.rows.begin(), v.rows.end(), [](const Row& r) { return Destination(r.evidence); });
            if (targets) v.status = targets == 1 ? "ready" : "multiple_targets";
            else if (v.scene.expected.empty()) v.status = "no_map_boss_configuration";
            else if (count("map_boss_unconfirmed")) v.status = "unconfirmed";
            else if (count("map_boss")) {
                bool allDead = true;
                for (const auto& r : v.rows) if (Role(r.evidence) == "map_boss")
                    allDead = allDead && r.evidence.hpValid && r.evidence.hp == 0;
                v.status = allDead ? "dead" : "target_state_unavailable";
            } else if (configuredSeen) v.status = "target_state_unavailable";
            else v.status = "configured_boss_not_in_loaded_tree";
            return v;
        } catch (const std::exception& e) { error = e.what(); if (attempt < 2) Sleep(100); }
    }
    throw std::runtime_error("No complete snapshot after 3 attempts: " + error);
}

std::string AsJson(const Snapshot& v, DWORD pid) {
    std::ostringstream o;
    o << "{\n  \"schemaVersion\":1,\"readOnly\":true,\"scanComplete\":true,\"pid\":" << pid
      << ",\"capturedAt\":" << Json(v.time) << ",\"visitedNodes\":" << v.nodes
      << ",\"destinationStatus\":" << Json(v.status)
      << ",\n  \"scope\":\"Loaded surroundings tree plus current WorldAreas.dat configured boss varieties\","
      << "\n  \"area\":{\"id\":" << Json(v.scene.id) << ",\"name\":" << Json(v.scene.name)
      << ",\"world\":" << Json(Hex(v.scene.world)) << ",\"record\":" << Json(Hex(v.scene.record)) << "},"
      << "\n  \"configuredBosses\":[";
    for (size_t i = 0; i < v.scene.expected.size(); ++i) {
        const auto& b = v.scene.expected[i]; if (i) o << ',';
        o << "{\"name\":" << Json(b.name) << ",\"metadata\":" << Json(b.metadata)
          << ",\"varietyRow\":" << Json(Hex(b.row)) << '}';
    }
    o << "],\n  \"destinationKeys\":["; bool comma = false;
    for (const auto& r : v.rows) if (Destination(r.evidence)) {
        if (comma) o << ','; comma = true; o << Json(Hex(r.key));
    }
    o << "],\n  \"entranceCheckpoints\":[";
    for (size_t i = 0; i < v.checkpoints.size(); ++i) {
        const auto& r = v.checkpoints[i]; if (i) o << ',';
        o << "{\"key\":" << Json(Hex(r.key)) << ",\"entity\":" << Json(Hex(r.entity))
          << ",\"name\":" << Json(r.name) << ",\"metadata\":" << Json(r.metadata)
          << ",\"bossEntrance\":" << (BossEntrance(r.metadata) ? "true" : "false")
          << ",\"position\":";
        if (r.evidence.positionValid) o << '[' << r.x << ',' << r.y << ']'; else o << "null";
        o << '}';
    }
    o << "],\n  \"entities\":[\n";
    for (size_t i = 0; i < v.rows.size(); ++i) {
        const auto& r = v.rows[i]; const auto& e = r.evidence; if (i) o << ",\n";
        o << "    {\"key\":" << Json(Hex(r.key)) << ",\"entity\":" << Json(Hex(r.entity))
          << ",\"name\":" << Json(r.name) << ",\"metadata\":" << Json(r.metadata)
          << ",\"role\":" << Json(Role(e)) << ",\"rarity\":";
        if (e.monster && e.valid) o << e.rarity; else o << "null";
        o << ",\"bossTierFromRuntimeMods\":" << e.bossTier << ",\"configuredForArea\":" << (e.configuredForArea ? "true" : "false")
          << ",\"combatOwner\":" << Json(Hex(r.combatOwner)) << ",\"isCombatOwner\":" << (e.isCombatOwner ? "true" : "false")
          << ",\"varietyRow\":" << Json(Hex(r.variety)) << ",\"metadataContainsBoss\":" << (r.metadataBoss ? "true" : "false")
          << ",\"hasBossMinionMod\":" << (r.minionMod ? "true" : "false") << ",\"hp\":";
        if (e.hpValid) o << e.hp; else o << "null";
        o << ",\"maxHp\":"; if (e.hpValid) o << r.maxHp; else o << "null";
        o << ",\"position\":"; if (e.positionValid) o << '[' << r.x << ',' << r.y << ']'; else o << "null";
        o << ",\"distanceWorldPixels\":"; if (r.distance >= 0) o << std::fixed << std::setprecision(2) << r.distance; else o << "null";
        o << ",\"destinationEligible\":" << (Destination(e) ? "true" : "false") << ",\"mods\":[";
        for (size_t j = 0; j < r.mods.size(); ++j) { if (j) o << ','; o << Json(r.mods[j]); }
        o << "],\"evidenceStats\":[";
        for (size_t j = 0; j < r.stats.size(); ++j) {
            const auto& s = r.stats[j]; if (j) o << ',';
            o << "{\"mod\":" << Json(s.mod) << ",\"stat\":" << Json(s.name) << ",\"value\":" << s.value << '}';
        }
        o << "]}";
    }
    o << "\n  ]\n}\n"; return o.str();
}
std::string Summary(const Snapshot& v) {
    std::ostringstream o; std::map<std::string, size_t> counts;
    for (const auto& r : v.rows) ++counts[Role(r.evidence)];
    o << "时间: " << v.time << "\n地图: " << v.scene.name << " (" << v.scene.id << ")\n"
      << "完整遍历: " << v.nodes << " 个实体节点；怪物组件实体 " << v.rows.size() - counts["monster_path_object"]
      << " 个，怪物路径附属对象 " << counts["monster_path_object"] << " 个\n"
      << "地图 Boss 配置名单:\n";
    if (v.scene.expected.empty()) o << "  当前地图数据未提供名单，不能据此断言没有 Boss。\n";
    for (const auto& b : v.scene.expected) o << "  " << b.name << " | " << b.metadata << '\n';
    o << "分类统计: "; for (const auto& [role, n] : counts) if (n) o << role << '=' << n << ' ';
    o << "\n目的地状态: " << v.status << "\n";
    if (v.status == "configured_boss_not_in_loaded_tree")
        o << "配置名单中的 Boss 尚未在当前已加载实体树中找到，无法提供其坐标。其他 Boss 可能来自额外内容或运行时替换，需继续核实。\n";
    o << "已加载 Boss / 唯一怪物 / 带 Boss 路径的精英:\n";
    for (const auto& r : v.rows) {
        auto role = Role(r.evidence);
        if (!r.evidence.monster || (!r.evidence.configuredForArea && !r.evidence.bossTier &&
            !(r.evidence.monster && r.evidence.rarity == 3) && !r.metadataBoss)) continue;
        o << "  [" << role << "] " << r.name << " key=" << Hex(r.key)
          << " rarity=" << r.evidence.rarity << " bossTier=" << r.evidence.bossTier << " HP=";
        if (r.evidence.hpValid) o << r.evidence.hp; else o << "unknown";
        o << " pos="; if (r.evidence.positionValid) o << '(' << r.x << ',' << r.y << ')'; else o << "unknown";
        o << " 目的地=" << (Destination(r.evidence) ? "是" : "否") << '\n';
    }
    o << "Boss 入口参考记录点:\n";
    bool foundEntrance = false;
    for (const auto& r : v.checkpoints) if (BossEntrance(r.metadata)) {
        foundEntrance = true;
        o << "  key=" << Hex(r.key) << " pos=";
        if (r.evidence.positionValid) o << '(' << r.x << ',' << r.y << ')'; else o << "unknown";
        o << " | " << r.metadata << '\n';
    }
    if (!foundEntrance) o << "  当前已加载树内未找到 Boss 专用记录点。\n";
    o << "只读测试程序；完整实体、词缀及判定证据见同名 JSON。\n"; return o.str();
}
void Write(const fs::path& file, const std::string& data) {
    auto tmp = file; tmp += ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << data;
      if (!f) throw std::runtime_error("Report write failed"); }
    fs::rename(tmp, file);
}
int SelfTest() {
    unsigned tests = 0;
    auto check = [&](const Evidence& e, const char* role, bool target) {
        ++tests;
        if (Role(e) != role || Destination(e) != target)
            throw std::runtime_error("Self-test " + std::to_string(tests) + " failed");
    };
    Evidence e; e.hpValid = e.positionValid = true; e.hp = 100;
    check(e, "ordinary", false);
    e.rarity = 1; check(e, "elite", false);
    e.rarity = 2; check(e, "elite", false);
    e.rarity = 3; check(e, "unique_unconfirmed", false);
    e.bossTier = 2; check(e, "other_boss", false); // Rust King sample in Stronghold.
    e.configuredForArea = true; check(e, "map_boss", true);
    e.hp = 0; check(e, "map_boss", false);
    e.hp = 100; e.hpValid = false; check(e, "map_boss", false);
    e.hpValid = true; e.positionValid = false; check(e, "map_boss", false);
    e.positionValid = true; e.bossTier = 0; check(e, "map_boss_unconfirmed", false);
    e.bossTier = 2; e.rarity = 2; check(e, "map_boss_unconfirmed", false);
    e.valid = false; check(e, "unknown", false);
    e.monster = false; check(e, "monster_path_object", false);
    e = {}; e.rarity = 3; e.bossTier = 2; e.configuredForArea = true;
    e.hpValid = e.positionValid = true; e.hp = 100; e.isCombatOwner = false;
    check(e, "shared_monster_proxy", false);
    ++tests;
    if (!BossEntrance("Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame_Boss") ||
        BossEntrance("Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame") ||
        BossEntrance("Metadata/MiscellaneousObjects/Checkpoints/Checkpoint_Endgame_BossEffect"))
        throw std::runtime_error("Boss/ordinary checkpoint distinction failed");
    ++tests;
    if (Canonical("Metadata/Monsters/Example@78") != "Metadata/Monsters/Example" ||
        Canonical("Metadata/Monsters/Example@Phase") != "Metadata/Monsters/Example@Phase")
        throw std::runtime_error("Metadata normalization test failed");
    ++tests;
    if (Json("quote\"\\\n") != "\"quote\\\"\\\\\\u000a\"") throw std::runtime_error("JSON escaping test failed");
    std::cout << "PASS " << tests << " classification/target/encoding cases\n"; return 0;
}
unsigned Number(const wchar_t* text, unsigned min, unsigned max) {
    std::wstring s(text); size_t end = 0;
    if (s.empty() || s.front() == L'-') throw std::runtime_error("Invalid numeric argument");
    auto n = std::stoull(s, &end);
    if (end != s.size() || n < min || n > max) throw std::runtime_error("Numeric argument out of range");
    return static_cast<unsigned>(n);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    DWORD pid = 0; unsigned samples = 1, interval = 1000; fs::path output;
    try {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--self-test") { if (argc != 2) throw std::runtime_error("--self-test takes no other options"); return SelfTest(); }
            if (a == L"--help") {
                std::cout << "BossProbe [--pid PID] [--out DIRECTORY] [--samples 1..120] [--interval 100..60000]\nBossProbe --self-test\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::runtime_error("Missing option value");
            if (a == L"--pid") pid = Number(argv[++i], 1, MAXDWORD);
            else if (a == L"--samples") samples = Number(argv[++i], 1, 120);
            else if (a == L"--interval") interval = Number(argv[++i], 100, 60000);
            else if (a == L"--out") output = fs::path(argv[++i]);
            else throw std::runtime_error("Unknown command-line option");
        }
        if (output.empty()) {
            std::array<wchar_t, 32768> exe{};
            if (!GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())))
                throw std::runtime_error("Executable path unavailable");
            output = fs::path(exe.data()).parent_path() / L"output";
        }
        fs::create_directories(output);
        if (!pid) pid = FindGame();
        Process process(pid);
        for (unsigned i = 0; i < samples; ++i) {
            auto snapshot = Capture(process); auto stem = "boss-" + Now(true) + '-' + std::to_string(i + 1);
            Write(output / (stem + ".json"), AsJson(snapshot, pid));
            auto text = Summary(snapshot); Write(output / (stem + ".txt"), text);
            std::cout << text << "报告: " << Utf8((output / (stem + ".json")).wstring()) << "\n" << std::flush;
            if (i + 1 < samples) Sleep(interval);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        if (!output.empty()) try {
            auto file = output / ("failed-" + Now(true) + ".json");
            Write(file, "{\"readOnly\":true,\"scanComplete\":false,\"pid\":" + std::to_string(pid) +
                ",\"capturedAt\":" + Json(Now()) + ",\"error\":" + Json(e.what()) + "}\n");
        } catch (...) { std::cerr << "Could not save failure report\n"; }
        return 1;
    }
}
