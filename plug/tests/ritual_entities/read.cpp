#include "../../Common/AppConfig.h"
#include "../../Common/Entity.h"
#include "../../Common/GameBuild.h"
#include "../../Common/GameMemory.h"
#include "../../Common/GameOffsets.h"
#include "../../Common/OlaDriver.h"
#include "../../Common/PointerChain.h"
#include "../../Plug/Surroundings.h"
#include "../../Plug/WorldArea.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
constexpr size_t kMaxNodes = 8192;

struct RawComponent {
    std::string name;
    uint64_t address = 0;
};

struct EntityRow {
    uint32_t key = 0;
    uint64_t node = 0;
    uint64_t entity = 0;
    poe::EntityKind kind = poe::EntityKind::None;
    std::string metadata;
    std::string name;
    std::string components;
    int32_t x = 0;
    int32_t y = 0;
    bool hasPosition = false;
    bool ritualCandidate = false;
};

bool Read(const mem::GameMemory& memory, uint64_t address, void* out, size_t size)
{
    return memory.IsPlausible(address) && memory.ReadInto(address, out, size);
}

std::string Csv(const std::string& value)
{
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else if (ch == '\r') out += " ";
        else if (ch == '\n') out += " ";
        else out += ch;
    }
    out += '"';
    return out;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool HasRitualKeyword(const std::string& metadata, const std::string& name)
{
    const std::string combined = LowerAscii(metadata + " " + name);
    const char* keywords[] = {"ritual", "altar", "sacrifice", "祭祀", "祭坛", "祭壇", "祭獻", "祭献"};
    for (const auto* keyword : keywords) {
        if (combined.find(keyword) != std::string::npos) return true;
    }
    return false;
}

bool ReadRawComponents(const mem::GameMemory& memory, uint64_t entity,
                       std::vector<RawComponent>& out, std::string& error)
{
    out.clear();
    error.clear();

    uint64_t pairsHeader = memory.Follow(entity + kEntityPairsOff0);
    if (!pairsHeader) { error = "component-pairs owner unavailable"; return false; }
    pairsHeader = memory.Follow(pairsHeader + kEntityPairsOff1);
    if (!pairsHeader) { error = "component-pairs header unavailable"; return false; }
    const uint64_t pairs = memory.Follow(pairsHeader + kEntityPairsOff2);
    uint64_t pairsEnd = 0;
    if (!pairs || !Read(memory, pairsHeader + kEntityPairsEndOff, &pairsEnd, sizeof(pairsEnd)) ||
        pairsEnd < pairs || (pairsEnd - pairs) % kComponentPairSize != 0) {
        error = "component-pairs range invalid";
        return false;
    }

    const uint64_t components = memory.Follow(entity + kEntityComponentsOff);
    uint64_t componentsEnd = 0;
    if (!components || !Read(memory, entity + kEntityComponentsEndOff, &componentsEnd, sizeof(componentsEnd)) ||
        componentsEnd < components || (componentsEnd - components) % kComponentPtrSize != 0) {
        error = "component array range invalid";
        return false;
    }

    const uint64_t pairCount = (pairsEnd - pairs) / kComponentPairSize;
    const uint64_t componentCount = (componentsEnd - components) / kComponentPtrSize;
    if (pairCount != componentCount || pairCount > kMaxComponentCount) {
        error = "component counts mismatch or exceed limit";
        return false;
    }

    out.reserve(static_cast<size_t>(pairCount));
    for (uint64_t i = 0; i < pairCount; ++i) {
        const uint64_t pair = pairs + i * kComponentPairSize;
        const uint64_t nameAddress = memory.Follow(pair);
        uint32_t index = 0;
        if (!nameAddress || !Read(memory, pair + kComponentPairIndexOff, &index, sizeof(index)) || index >= componentCount) {
            error = "component entry invalid";
            return false;
        }
        const uint64_t component = memory.Follow(components + static_cast<uint64_t>(index) * kComponentPtrSize);
        const std::string name = memory.ReadText(nameAddress, mem::TextKind::Ansi);
        if (!component || name.empty()) {
            error = "component pointer or name unavailable";
            return false;
        }
        out.push_back({name, component});
    }
    return true;
}

const RawComponent* FindComponent(const std::vector<RawComponent>& components, const char* name)
{
    for (const auto& component : components) {
        if (component.name == name) return &component;
    }
    return nullptr;
}

bool Scan(const mem::GameMemory& memory, uint64_t moduleBase, std::vector<EntityRow>& rows,
          size_t& visitedCount, std::string& error)
{
    rows.clear();
    visitedCount = 0;
    error.clear();
    const uint64_t sentinel = mem::PointerChain(kSurroundingsChain, kSurroundingsChainDepth, mem::TailOp::Deref)
        .Resolve(memory, moduleBase);
    if (!sentinel) { error = "surroundings sentinel unavailable"; return false; }
    const uint64_t root = memory.Follow(sentinel + kNodeParentOff);
    if (!root) { error = "surroundings root unavailable"; return false; }

    std::vector<uint64_t> pending{root};
    std::unordered_set<uint64_t> visited;
    while (!pending.empty()) {
        const uint64_t node = pending.back();
        pending.pop_back();
        if (node == sentinel) continue;
        if (!memory.IsPlausible(node)) { error = "invalid tree node address"; return false; }
        if (!visited.insert(node).second || visited.size() > kMaxNodes) {
            error = "tree cycle or node limit reached";
            return false;
        }

        uint8_t flag = 0;
        if (!Read(memory, node + kNodeFlagOff, &flag, sizeof(flag))) { error = "node flag unreadable"; return false; }
        if (flag == 1) continue;

        EntityRow row;
        row.node = node;
        uint64_t left = 0, right = 0;
        if (!Read(memory, node + kNodeKeyOff, &row.key, sizeof(row.key)) ||
            !Read(memory, node + kNodeEntityOff, &row.entity, sizeof(row.entity)) || !memory.IsPlausible(row.entity) ||
            !Read(memory, node + kNodeLeftOff, &left, sizeof(left)) ||
            !Read(memory, node + kNodeRightOff, &right, sizeof(right))) {
            error = "tree node fields unreadable";
            return false;
        }

        const poe::Entity entity(memory, row.entity);
        row.metadata = entity.MetadataPath();
        row.kind = poe::ClassifyByMetadata(row.metadata);
        std::vector<RawComponent> components;
        std::string componentError;
        if (ReadRawComponents(memory, row.entity, components, componentError)) {
            for (const auto& component : components) {
                if (!row.components.empty()) row.components += ';';
                row.components += component.name;
            }
            if (const auto* render = FindComponent(components, "Render"))
                row.name = memory.ReadStdString(render->address + kRender_NameOff);
            if (const auto* position = FindComponent(components, "Positioned")) {
                int32_t xy[2]{};
                if (Read(memory, position->address + kPositioned_EntXOff, xy, sizeof(xy))) {
                    row.x = xy[0]; row.y = xy[1]; row.hasPosition = true;
                }
            }
        } else {
            row.components = "<unreadable: " + componentError + ">";
        }
        row.ritualCandidate = HasRitualKeyword(row.metadata, row.name);
        rows.push_back(std::move(row));

        uint64_t checkedEntity = 0;
        uint32_t checkedKey = 0;
        if (!Read(memory, node + kNodeEntityOff, &checkedEntity, sizeof(checkedEntity)) || checkedEntity != rows.back().entity ||
            !Read(memory, node + kNodeKeyOff, &checkedKey, sizeof(checkedKey)) || checkedKey != rows.back().key) {
            error = "entity tree changed during scan";
            return false;
        }
        if (right && right != sentinel) pending.push_back(right);
        if (left && left != sentinel) pending.push_back(left);
    }

    if (mem::PointerChain(kSurroundingsChain, kSurroundingsChainDepth, mem::TailOp::Deref).Resolve(memory, moduleBase) != sentinel ||
        memory.Follow(sentinel + kNodeParentOff) != root) {
        error = "scene tree changed during scan";
        return false;
    }
    visitedCount = visited.size();
    return true;
}

void WriteCsv(const std::string& path, const std::vector<EntityRow>& rows)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output file: " + path);
    output << "candidate,classified,key,node,entity,name,metadata,x,y,components\r\n";
    for (const auto& row : rows) {
        output << (row.ritualCandidate ? "yes" : "no") << ','
               << (row.kind == poe::EntityKind::None ? "no" : "yes") << ','
               << "0x" << std::hex << row.key << ",0x" << row.node << ",0x" << row.entity << std::dec << ','
               << Csv(row.name) << ',' << Csv(row.metadata) << ',';
        if (row.hasPosition) output << row.x << ',' << row.y;
        else output << ',';
        output << ',' << Csv(row.components) << "\r\n";
    }
}
} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: RitualEntitiesProbe <game-pid> [output.csv]\n");
        return 2;
    }
    char* end = nullptr;
    const auto pid = std::strtoul(argv[1], &end, 10);
    if (!pid || !end || *end) return 2;

    ola::OlaDriver driver;
    if (!driver.InitializeLocal(static_cast<DWORD>(pid))) {
        std::fprintf(stderr, "Cannot open game process %lu for read-only access\n", pid);
        return 1;
    }
    const mem::GameMemory memory(driver);
    const auto moduleBase = memory.ModuleBase(app::Config().gameModuleName);
    std::string error;
    if (!moduleBase || !gamebuild::Verify(moduleBase, [&memory](uint64_t address, void* data, size_t size) {
            return memory.ReadInto(address, data, size);
        }, error)) {
        std::fprintf(stderr, "%s\n", error.empty() ? "Game module unavailable" : error.c_str());
        return 1;
    }

    poe::WorldAreaSnapshot area;
    std::string areaError;
    const bool hasArea = poe::ReadWorldArea(memory, moduleBase, area, areaError);
    std::vector<EntityRow> rows;
    size_t visited = 0;
    if (!Scan(memory, moduleBase, rows, visited, error)) {
        std::fprintf(stderr, "Scan failed: %s\n", error.c_str());
        return 1;
    }

    size_t unknown = 0, candidates = 0, unknownCandidates = 0;
    for (const auto& row : rows) {
        if (row.kind == poe::EntityKind::None) ++unknown;
        if (row.ritualCandidate) {
            ++candidates;
            if (row.kind == poe::EntityKind::None) ++unknownCandidates;
        }
    }
    const std::string outputPath = argc == 3 ? argv[2] : ("ritual_scan_" + std::to_string(pid) + ".csv");
    try {
        WriteCsv(outputPath, rows);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    if (hasArea) std::printf("area=%s (%s)\n", area.name.c_str(), area.id.c_str());
    else std::printf("area=unavailable (%s)\n", areaError.c_str());
    std::printf("pid=%lu visited=%zu exported=%zu unclassified=%zu keyword_candidates=%zu unclassified_candidates=%zu\n",
        pid, visited, rows.size(), unknown, candidates, unknownCandidates);
    std::printf("csv=%s\n", outputPath.c_str());
    for (const auto& row : rows) {
        if (!row.ritualCandidate) continue;
        std::printf("candidate key=0x%X entity=0x%llX pos=[%d,%d] classified=%s name=%s metadata=%s components=%s\n",
            row.key, static_cast<unsigned long long>(row.entity), row.x, row.y,
            row.kind == poe::EntityKind::None ? "no" : "yes", row.name.c_str(),
            row.metadata.c_str(), row.components.c_str());
    }
    return 0;
}
