#include "Surroundings.h"
#include "../Common/Logger.h"
#include "../Common/PointerChain.h"
#include "../Common/MonsterReader.h"

#include <cstdio>
#include <algorithm>
#include <stack>
#include <sstream>
#include <unordered_set>

namespace poe {
namespace {

// 元数据路径 -> 实体类型 识别表（顺序即优先级；匹配串为日志实证）
struct KindPattern {
    EntityKind  kind;
    const char* pattern;
};

const KindPattern kPatterns[] = {
    { EntityKind::Monster,    "Monster" },          // Metadata/Monsters/...
    { EntityKind::Player,     "Player" },           // 带 Player 字样的玩家路径
    { EntityKind::Player,     "Characters" },       // Metadata/Characters/Dex/DexFour
    { EntityKind::Portal,     "MultiplexPortal" },  // 传送门
    { EntityKind::NPC,        "NPC" },              // Metadata/NPC/Four_Act4/...
    { EntityKind::WorldItem,  "WorldItem" },        // 掉落物
    { EntityKind::Hideout,    "Hideout" },          // 藏身处物件
    { EntityKind::CheckPoint, "Checkpoint" },       // 记录点
    { EntityKind::WaterWell,  "HealingWell" },      // 水井
    { EntityKind::Chest,      "CampsiteChest" },    // 地图箱子
    { EntityKind::GuildStash, "GuildStash" },       // 公会仓库（含 Stash 子串，须排在仓库前）
    { EntityKind::Stash,      "Stash" },            // 仓库
};

// 全部实体的读取配方（加新实体 = 这里加一行）
const KindRecipe kRecipes[] = {
    { EntityKind::Player,     "玩家",           NameSource::Player, true  },
    { EntityKind::Monster,    "怪物",           NameSource::Render, true  },
    { EntityKind::Portal,     "传送门",         NameSource::Render, false },
    { EntityKind::WorldItem,  "掉落物",         NameSource::Base,   false },
    { EntityKind::NPC,        "NPC",            NameSource::Render, false },
    { EntityKind::Hideout,    "藏身处物件",     NameSource::Render, false },
    { EntityKind::CheckPoint, "记录点",         NameSource::Render, false },
    { EntityKind::WaterWell,  "水井",           NameSource::Render, false },
    { EntityKind::Chest,      "地图特殊箱子",   NameSource::Render, false },
    { EntityKind::GuildStash, "公会仓库",       NameSource::Render, false },
    { EntityKind::Stash,      "仓库",           NameSource::Render, false },
    { EntityKind::None,       "未知",           NameSource::Render, false },
};

// 遍历节点上限（防止异常数据把遍历拖死）
constexpr size_t kMaxVisited = 8192;

bool HasRitualKeyword(std::string text)
{
    // Only fold ASCII; preserve the UTF-8 bytes of Chinese names.
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return char(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    for (const auto* keyword : {"ritual", "altar", "sacrifice", "祭祀", "祭坛", "祭壇", "祭獻", "祭献"})
        if (text.find(keyword) != std::string::npos) return true;
    return false;
}

} // namespace

EntityKind ClassifyByMetadata(const std::string& metadataPath)
{
    if (metadataPath.empty()) {
        return EntityKind::None;
    }
    for (const auto& p : kPatterns) {
        if (metadataPath.find(p.pattern) != std::string::npos) {
            return p.kind;
        }
    }
    return EntityKind::None;
}

const KindRecipe& RecipeOf(EntityKind kind)
{
    for (const auto& r : kRecipes) {
        if (r.kind == kind) {
            return r;
        }
    }
    return kRecipes[sizeof(kRecipes) / sizeof(kRecipes[0]) - 1];   // 兜底 = 未知
}

EntitySnapshot ReadSnapshot(const mem::GameMemory& memory, uint64_t entityAddr, uint32_t key)
{
    EntitySnapshot snap;
    snap.entity = entityAddr;
    snap.key = key;

    const Entity ent(memory, entityAddr);
    snap.metadataPath = ent.MetadataPath();
    snap.kind = ClassifyByMetadata(snap.metadataPath);
    const KindRecipe& recipe = RecipeOf(snap.kind);

    // ---- 名字（按配方选来源，取不到统一回退 Render）----
    switch (recipe.nameSource) {
    case NameSource::Player:
        if (auto player = ent.GetAs<PlayerComponent>("Player")) {
            snap.name = player->GetPlayerName();
        }
        break;
    case NameSource::Base:
        if (auto worldItem = ent.GetAs<WorldItemComponent>("WorldItem")) {
            const uint64_t itemEntity = worldItem->GetItemEntity();
            if (itemEntity != 0) {
                const Entity itemEnt(memory, itemEntity);
                if (auto base = itemEnt.GetAs<BaseComponent>("Base")) {
                    snap.name = base->GetItemName();
                }
            }
        }
        break;
    case NameSource::Render:
    default:
        break;
    }
    if (snap.name.empty()) {
        if (auto render = ent.GetAs<RenderComponent>("Render")) {
            snap.name = render->GetRenderName();
        }
    }

    // ---- 坐标（所有类型读法一致：+0x444/+0x448 的 int32，已是世界像素）----
    if (auto pos = ent.GetAs<PositionComponent>("Positioned")) {
        const EntityPosition p = pos->GetPosition();
        snap.x = p.x;
        snap.y = p.y;
    }

    // ---- 血量（只有配方要才读）----
    if (recipe.wantsHp) {
        if (auto life = ent.GetAs<LifeComponent>("Life")) {
            snap.hp = life->GetHp();
        }
    }

    return snap;
}

std::string SnapshotToText(const EntitySnapshot& snapshot)
{
    const KindRecipe& recipe = RecipeOf(snapshot.kind);

    // 未识别类型不输出（遍历阶段已过滤，这里只是防御性返回空串）
    if (snapshot.kind == EntityKind::None) {
        return std::string();
    }

    char head[256] = { 0 };
    _snprintf_s(head, sizeof(head), _TRUNCATE,
        "[%s]  key:0x%X  entity:0x%llX\n",
        recipe.displayName, snapshot.key, (unsigned long long)snapshot.entity);

    std::string out(head);
    out += "name:" + snapshot.name;

    if (recipe.wantsHp) {
        char hp[64] = { 0 };
        _snprintf_s(hp, sizeof(hp), _TRUNCATE, " hp:%u", snapshot.hp);
        out += hp;
    }

    char tail[128] = { 0 };
    _snprintf_s(tail, sizeof(tail), _TRUNCATE, " pos:[%d,%d]\n", snapshot.x, snapshot.y);
    out += tail;
    return out;
}

SurroundingsTraverser::SurroundingsTraverser(const mem::GameMemory& memory)
    : m_mem(&memory)
{
}
std::string ScanSummary(const EntityScanInfo& info, size_t returned) {
    std::ostringstream out;
    out << "visited=" << info.visited << " matched=" << info.matched << " returned=" << returned
        << " non_combat=" << info.nonCombat << " unreadable=" << info.unreadable
        << " complete=" << info.Complete() << " error=" << info.error;
    for (const auto& issue : info.issues)
        out << " | key=" << issue.key << " entity=0x" << std::hex << issue.entity << std::dec
            << " metadata=" << issue.metadata << " reason=" << issue.reason;
    return out.str();
}
bool SurroundingsTraverser::TraverseFromRoot(std::vector<EntitySnapshot>& out, uint64_t root) const
{
    out.clear();
    if (root == 0) {
        LOGW("[Surroundings] 根节点为空");
        return false;
    }

    std::stack<uint64_t> pending;
    pending.push(root);

    size_t visited = 0;
    while (!pending.empty()) {
        const uint64_t cur = pending.top();
        pending.pop();

        if (cur == 0 || !m_mem->IsPlausible(cur)) {
            continue;
        }
        if (++visited > kMaxVisited) {
            LOGW("[Surroundings] 遍历节点数超过 %zu，提前终止", kMaxVisited);
            break;
        }

        if (m_mem->Read<uint8_t>(cur + kNodeFlagOff) == 1) {
            continue;   // 哨兵
        }

        const uint32_t key = m_mem->Read<uint32_t>(cur + kNodeKeyOff);

        // 周围树的 entity 只解一层（背包树要解两层）
        const uint64_t entity = m_mem->Follow(cur + kNodeEntityOff);
        if (entity != 0) {
            EntitySnapshot snap = ReadSnapshot(*m_mem, entity, key);
            // 未识别类型（None）直接丢弃，不输出
            if (snap.kind != EntityKind::None) {
                out.push_back(std::move(snap));
            }
        }

        // 栈 LIFO：先压右再压左，保持「先左后右」的递归顺序
        pending.push(m_mem->Read<uint64_t>(cur + kNodeRightOff));
        pending.push(m_mem->Read<uint64_t>(cur + kNodeLeftOff));
    }
    return true;
}

bool SurroundingsTraverser::FindPlayerByName(uint64_t moduleBase, const std::string& name, uint64_t exclude,
                                           PlayerMatch& out, const std::atomic_bool* stop) const
{
    out = {};
    std::vector<PlayerCandidate> players;
    PlayerScanInfo info;
    if (!ListPlayers(moduleBase, players, info, stop)) return false;
    for (const auto& player : players) {
        if (player.entity != exclude && player.name == name) {
            ++out.matches; out.entity = player.entity; out.positioned = player.positioned;
        }
    }
    return true;
}

bool ReadPlayerCandidate(const mem::GameMemory& memory, uint64_t entity, PlayerCandidate& out)
{
    out = {}; out.entity = entity;
    Entity ent(memory, entity);
    out.metadataPath = ent.MetadataPath();
    if (ClassifyByMetadata(out.metadataPath) != EntityKind::Player) return false;
    auto player = ent.GetAs<PlayerComponent>("Player");
    auto pos = ent.GetAs<PositionComponent>("Positioned");
    if (!player || !pos) return false;
    out.playerComponent = player->Ptr(); out.positioned = pos->Ptr();
    out.name = player->GetPlayerName();
    static_assert(kPositioned_EntYOff == kPositioned_EntXOff + sizeof(int32_t));
    int32_t xy[2]{};
    out.positionValid = memory.ReadInto(out.positioned + kPositioned_EntXOff, xy, sizeof(xy));
    if (out.positionValid) { out.x = xy[0]; out.y = xy[1]; }
    return !out.name.empty();
}

bool SurroundingsTraverser::ListPlayers(uint64_t moduleBase, std::vector<PlayerCandidate>& out,
                                      PlayerScanInfo& info, const std::atomic_bool* stop) const
{
    out.clear(); info = {};
    auto fail = [&](const char* reason) { info.error = reason; out.clear(); return false; };
    const auto sentinel = mem::PointerChain(kSurroundingsChain, kSurroundingsChainDepth, mem::TailOp::Deref)
        .Resolve(*m_mem, moduleBase);
    const auto root = sentinel ? m_mem->Follow(sentinel + kNodeParentOff) : 0;
    info.sentinel = sentinel; info.root = root;
    if (!root) return fail("周围树链读取失败");
    std::stack<uint64_t> pending; pending.push(root);
    std::unordered_set<uint64_t> visited;
    while (!pending.empty()) {
        if (stop && stop->load()) return fail("周围玩家扫描已取消");
        const auto node = pending.top(); pending.pop();
        if (!node || !m_mem->IsPlausible(node)) continue;
        if (node == sentinel) continue;
        if (!visited.insert(node).second || visited.size() > kMaxVisited) return fail("周围树出现循环或超出节点上限");
        info.visited = visited.size();
        uint8_t flag = 0;
        if (!m_mem->ReadInto(node + kNodeFlagOff, &flag, sizeof(flag))) return fail("周围树节点读取失败");
        if (flag == 1) continue;
        const auto address = m_mem->Follow(node + kNodeEntityOff);
        if (address) {
            PlayerCandidate player;
            if (ReadPlayerCandidate(*m_mem, address, player)) {
                player.node = node; out.push_back(std::move(player));
            }
        }
        uint64_t left = 0, right = 0;
        if (!m_mem->ReadInto(node + kNodeLeftOff, &left, sizeof(left)) ||
            !m_mem->ReadInto(node + kNodeRightOff, &right, sizeof(right))) return fail("周围树子节点读取失败");
        if (right && right != sentinel) pending.push(right);
        if (left && left != sentinel) pending.push(left);
    }
    return true;
}

bool SurroundingsTraverser::ListWorldItems(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                                         EntityScanInfo& info, const std::atomic_bool* stop) const
{
    return ListKind(moduleBase, EntityKind::WorldItem, out, info, stop);
}

bool SurroundingsTraverser::ListPortals(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                                      EntityScanInfo& info, const std::atomic_bool* stop) const
{
    return ListKind(moduleBase, EntityKind::Portal, out, info, stop);
}

bool SurroundingsTraverser::ListMonsters(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                                       EntityScanInfo& info, const std::atomic_bool* stop) const
{
    return ListKind(moduleBase, EntityKind::Monster, out, info, stop);
}

bool SurroundingsTraverser::ListKind(uint64_t moduleBase, EntityKind kind, std::vector<EntitySnapshot>& out,
                                   EntityScanInfo& info, const std::atomic_bool* stop) const
{
    return ListMatching(moduleBase, kind, false, out, info, stop);
}

bool SurroundingsTraverser::ListRitualCandidates(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                                               EntityScanInfo& info, const std::atomic_bool* stop) const
{
    return ListMatching(moduleBase, EntityKind::None, true, out, info, stop);
}

bool SurroundingsTraverser::ListEncounterTargets(uint64_t moduleBase, const WorldAreaSnapshot& area,
                                                std::vector<EntitySnapshot>& out, EntityScanInfo& info,
                                                const std::atomic_bool* stop) const {
    return ListMatching(moduleBase, EntityKind::None, false, out, info, stop, &area);
}

bool SurroundingsTraverser::ListMatching(uint64_t moduleBase, EntityKind kind, bool ritualCandidates,
                                       std::vector<EntitySnapshot>& out, EntityScanInfo& info,
                                       const std::atomic_bool* stop, const WorldAreaSnapshot* encounterArea) const
{
    out.clear(); info = {};
    auto fail = [&](const char* reason) { info.error = reason; out.clear(); return false; };
    const bool tolerateExpired = kind == EntityKind::Monster || ritualCandidates || encounterArea;
    auto issue = [&](uint32_t key, uint64_t entity, const std::string& metadata,
                     const std::string& reason, bool nonCombat = false) {
        if (nonCombat) ++info.nonCombat; else ++info.unreadable;
        if (info.issues.size() < 8) info.issues.push_back({key, entity, metadata, reason});
    };
    monster::Reader<mem::GameMemory> reader(*m_mem, moduleBase);
    std::vector<monster::ConfiguredBoss> catalog;
    if (encounterArea) {
        try { catalog = reader.Catalog(encounterArea->world, encounterArea->record); }
        catch (const std::exception& e) { info.error = e.what(); return false; }
    }
    std::unordered_set<uint64_t> loaded;
    std::vector<std::pair<uint64_t, std::string>> proxies;
    const auto sentinel = mem::PointerChain(kSurroundingsChain, kSurroundingsChainDepth, mem::TailOp::Deref)
        .Resolve(*m_mem, moduleBase);
    const auto root = sentinel ? m_mem->Follow(sentinel + kNodeParentOff) : 0;
    info.sentinel = sentinel; info.root = root;
    if (!root) return fail("周围实体树链读取失败");
    std::stack<uint64_t> pending; pending.push(root);
    std::unordered_set<uint64_t> visited;
    std::unordered_set<uint32_t> keys;
    while (!pending.empty()) {
        if (stop && stop->load()) return fail("周围实体扫描已取消");
        const auto node = pending.top(); pending.pop();
        if (node == sentinel) continue;
        if (!m_mem->IsPlausible(node)) {
            if (!tolerateExpired) return fail("周围实体树节点指针失效");
            issue(0, 0, "", "树节点已失效 node=" + std::to_string(node)); continue;
        }
        if (!visited.insert(node).second || visited.size() > kMaxVisited) return fail("周围实体树出现循环或超出节点上限");
        info.visited = visited.size();
        uint8_t flag = 0;
        if (!m_mem->ReadInto(node + kNodeFlagOff, &flag, sizeof(flag))) {
            if (!tolerateExpired) return fail("周围实体树节点读取失败");
            issue(0, 0, "", "树节点已卸载 node=" + std::to_string(node)); continue;
        }
        if (flag == 1) continue;
        uint64_t address = 0, left = 0, right = 0;
        uint32_t key = 0;
        if (!m_mem->ReadInto(node + kNodeLeftOff, &left, sizeof(left)) ||
            !m_mem->ReadInto(node + kNodeRightOff, &right, sizeof(right))) {
            if (!tolerateExpired) return fail("周围实体树正在变化或读取失败");
            issue(0, 0, "", "树节点链接已失效 node=" + std::to_string(node)); continue;
        }
        const auto before = out.size();
        std::string metadata;
        try {
            if (!m_mem->ReadInto(node + kNodeEntityOff, &address, sizeof(address)) || !m_mem->IsPlausible(address) ||
                !m_mem->ReadInto(node + kNodeKeyOff, &key, sizeof(key)))
                throw std::runtime_error("实体已销毁或节点正在替换");
            if (key && !keys.insert(key).second) throw std::runtime_error("周围实体树包含重复实体 key");
            const Entity ent(*m_mem, address);
            metadata = ent.MetadataPath();
            if (metadata.empty()) throw std::runtime_error("周围实体元数据暂时无法读取");
            std::string renderName;
            if (ritualCandidates) {
                if (auto render = ent.GetAs<RenderComponent>("Render")) renderName = render->GetRenderName();
            }
            loaded.insert(address);
            if (encounterArea) {
                if (metadata.find("Metadata/Monsters/") == 0 || monster::BossEntrance(metadata)) {
                    ++info.matched;
                    auto item = ReadSnapshot(*m_mem, address, key);
                    if (monster::BossEntrance(metadata)) {
                        item.combat = {}; item.combat.monster = false;
                        item.name = "终极 Boss 入口（橙色记录点）";
                    } else {
                        item.combat = reader.Identity(address, metadata, catalog, item.combatOwner, item.name);
                        if (!item.combat.monster) issue(key, address, metadata, "路径匹配但没有 Monster 组件（机关／特效）", true);
                        if (item.combat.monster && item.combat.isCombatOwner && (!item.combat.valid || !item.combat.hpValid))
                            throw std::runtime_error("Monster 身份或生命组件暂不可读，保留旧目标；未判定死亡");
                        if (item.combat.monster && !item.combat.isCombatOwner) {
                            if (Entity(*m_mem, item.combatOwner).MetadataPath() != metadata)
                                throw std::runtime_error("Boss 代理与主体元数据不一致");
                            proxies.emplace_back(item.combatOwner, metadata);
                        }
                    }
                    if (auto pos = ent.GetAs<PositionComponent>("Positioned")) {
                        if (!key || m_mem->Read<uint64_t>(pos->Ptr() + monsterlayout::kComponentOwner) != address)
                            throw std::runtime_error("路线目标 key 或坐标组件归属不匹配");
                        int32_t xy[2]{};
                        if (!m_mem->ReadInto(pos->Ptr() + kPositioned_EntXOff, xy, sizeof(xy)))
                            throw std::runtime_error("路线目标坐标暂不可读");
                        item.x = xy[0]; item.y = xy[1];
                        item.combat.positionValid = item.x >= 0 && item.y >= 0;
                    } else { item.x = item.y = -1; item.combat.positionValid = false; }
                    item.hp = item.combat.hp;
                    if ((item.combat.monster && item.combat.isCombatOwner) || monster::BossEntrance(metadata))
                        out.push_back(std::move(item));
                }
            } else if (ritualCandidates ? HasRitualKeyword(metadata + " " + renderName) : ClassifyByMetadata(metadata) == kind) {
                ++info.matched;
                auto item = ReadSnapshot(*m_mem, address, key);
                bool combatEntity = true;
                if (!ritualCandidates && kind == EntityKind::Monster) {
                    item.combat = reader.Vitals(address);
                    combatEntity = item.combat.monster && item.combat.isCombatOwner;
                    if (!combatEntity) issue(key, address, metadata,
                        item.combat.monster ? "共享 Monster 代理，等待主体" : "路径匹配但没有 Monster 组件（机关／特效）", true);
                    else item.hp = item.combat.hp;
                }
                auto positioned = ent.GetAs<PositionComponent>("Positioned");
                int32_t xy[2]{};
                // Non-positioned effects cannot be navigation targets.
                if (combatEntity && (positioned || !ritualCandidates)) {
                    if (!key || item.metadataPath != metadata ||
                        (!ritualCandidates && (item.kind != kind || (kind != EntityKind::Monster && item.name.empty()))) ||
                        !positioned || !m_mem->ReadInto(positioned->Ptr() + kPositioned_EntXOff, xy, sizeof(xy)))
                        throw std::runtime_error("目标实体名称或坐标读取失败");
                    item.x = xy[0]; item.y = xy[1];
                    if (!ritualCandidates && kind == EntityKind::Monster) {
                        if (m_mem->Read<uint64_t>(positioned->Ptr() + monsterlayout::kComponentOwner) != address)
                            throw std::runtime_error("怪物坐标组件归属不匹配");
                        item.combat.positionValid = item.x >= 0 && item.y >= 0;
                    }
                    if (ritualCandidates) {
                        ritualstate::ReadEntity(item.entity, [&](uint64_t address, void* out, size_t size) {
                            return m_mem->ReadInto(address, out, size);
                        }, item.ritual);
                    }
                    out.push_back(std::move(item));
                }
            }
            uint64_t checkedEntity = 0; uint32_t checkedKey = 0;
            if (!m_mem->ReadInto(node + kNodeEntityOff, &checkedEntity, sizeof(checkedEntity)) || checkedEntity != address ||
                !m_mem->ReadInto(node + kNodeKeyOff, &checkedKey, sizeof(checkedKey)) || checkedKey != key)
                throw std::runtime_error("扫描期间周围实体已变化");
        } catch (const std::exception& e) {
            if (!tolerateExpired) return fail(e.what());
            out.resize(before);
            issue(key, address, metadata, e.what());
        }
        if (right && right != sentinel) pending.push(right);
        if (left && left != sentinel) pending.push(left);
    }
    const auto checkedSentinel = mem::PointerChain(kSurroundingsChain, kSurroundingsChainDepth, mem::TailOp::Deref)
        .Resolve(*m_mem, moduleBase);
    if (checkedSentinel != sentinel || m_mem->Follow(sentinel + kNodeParentOff) != root)
        return fail("扫描期间周围树已变化");
    for (const auto& proxy : proxies) if (!loaded.count(proxy.first))
        issue(0, proxy.first, proxy.second, "Boss 代理的主体不在当前实体树，保留目标等待重扫");
    if (encounterArea) {
        WorldAreaSnapshot checked; std::string error;
        if (!ReadWorldArea(*m_mem, moduleBase, checked, error) || !encounterArea->SameMap(checked))
            return fail("路线目标扫描期间地图实例变化");
    }
    return true;
}

bool SurroundingsTraverser::Traverse(std::vector<EntitySnapshot>& out, uint64_t moduleBase) const
{
    out.clear();

    const uint64_t sentinel = mem::PointerChain(
        kSurroundingsChain, kSurroundingsChainDepth, mem::TailOp::Deref)
        .Resolve(*m_mem, moduleBase);

    if (sentinel == 0) {
        LOGW("[Surroundings] 周围链解析失败（基址 0x%llX）", (unsigned long long)moduleBase);
        return false;
    }

    const uint64_t root = m_mem->Follow(sentinel + kNodeParentOff);
    if (root == 0) {
        LOGW("[Surroundings] 哨兵 0x%llX 的 parent 为空", (unsigned long long)sentinel);
        return false;
    }
    return TraverseFromRoot(out, root);
}

} // namespace poe
