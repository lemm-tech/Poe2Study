#pragma once
// ============================================================
// Surroundings.h —— 周围实体遍历（用例层）
//
// 重构要点：
//   1. 原来 12 个几乎逐字相同的 switch 分支 -> 一张 KindRecipe 表
//      + 一个通用的 Snapshot() 填值函数。以后加一种实体只加一行表。
//   2. SurroundingsEntity 子类已删除：它只多了 ToString()，
//      没有任何共享状态，属于"为继承而继承"。
//      现在用 Entity + 自由函数。
//   3. EntitySnapshot 是纯 POD，不碰内存，可以直接单测。
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#include "../Common/Entity.h"
#include "../Common/GameMemory.h"
#include "../Common/RitualState.h"
#include "../Common/MonsterIdentity.h"
#include "WorldArea.h"

namespace poe {

// 实体类型（元数据路径识别）
enum class EntityKind {
    None,
    Player,
    Monster,
    Portal,
    WorldItem,
    NPC,
    Hideout,
    CheckPoint,
    WaterWell,
    Chest,
    GuildStash,
    Stash
};

// 名字从哪个组件取
enum class NameSource {
    Render,     // Render 组件 +0x160
    Player,     // Player 组件 +0x1B0（取不到回退 Render）
    Base        // WorldItem -> 物品实体 -> Base 组件
};

// 每种实体的"读取配方"
struct KindRecipe {
    EntityKind  kind;
    const char* displayName;
    NameSource  nameSource;
    bool        wantsHp;      // 只有玩家/怪物要血量
};

// 元数据路径 -> 类型（顺序即优先级）
EntityKind ClassifyByMetadata(const std::string& metadataPath);

// 取某类型的配方
const KindRecipe& RecipeOf(EntityKind kind);

// 一个实体的快照（纯数据）
struct EntitySnapshot {
    uint32_t    key      = 0;
    uint64_t    entity   = 0;
    EntityKind  kind     = EntityKind::None;
    std::string name;
    uint32_t    hp       = 0;
    int32_t     x        = 0;      // +0x444，世界像素
    int32_t     y        = 0;      // +0x448，世界像素
    std::string metadataPath;
    ritualstate::Snapshot ritual;
    monster::Evidence combat{false};
    uint64_t combatOwner = 0;
};

// 按配方读取一个实体
EntitySnapshot ReadSnapshot(const mem::GameMemory& memory, uint64_t entityAddr, uint32_t key);

// 快照 -> 两行显示文本
std::string SnapshotToText(const EntitySnapshot& snapshot);

// 周围遍历
struct PlayerMatch {
    uint64_t entity = 0, positioned = 0;
    uint32_t matches = 0;
};
struct PlayerCandidate {
    uint64_t node = 0, entity = 0, playerComponent = 0, positioned = 0;
    std::string name, metadataPath;
    int32_t x = 0, y = 0;
    bool positionValid = false;
};
struct EntityScanIssue {
    uint32_t key = 0;
    uint64_t entity = 0;
    std::string metadata, reason;
};
struct EntityScanInfo {
    uint64_t sentinel = 0, root = 0;
    size_t visited = 0, matched = 0, unreadable = 0, nonCombat = 0;
    // Bounded samples; counters cover every skipped entity.
    std::vector<EntityScanIssue> issues;
    std::string error;
    bool Complete() const { return error.empty() && !unreadable; }
};
using PlayerScanInfo = EntityScanInfo;
std::string ScanSummary(const EntityScanInfo& info, size_t returned);
bool ReadPlayerCandidate(const mem::GameMemory& memory, uint64_t entity, PlayerCandidate& out);
class SurroundingsTraverser {
public:
        explicit SurroundingsTraverser(const mem::GameMemory& memory);


    // 沿周围树链遍历全部实体
    bool Traverse(std::vector<EntitySnapshot>& out, uint64_t moduleBase) const;

    // 从一个已知根节点开始（调试/回归用）
    bool TraverseFromRoot(std::vector<EntitySnapshot>& out, uint64_t root) const;
    bool FindPlayerByName(uint64_t moduleBase, const std::string& name, uint64_t exclude,
                          PlayerMatch& out, const std::atomic_bool* stop = nullptr) const;
    bool ListPlayers(uint64_t moduleBase, std::vector<PlayerCandidate>& out,
                     PlayerScanInfo& info, const std::atomic_bool* stop = nullptr) const;
    // All-or-nothing scan: failures must never be interpreted as an empty drop list.
    bool ListWorldItems(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                        EntityScanInfo& info, const std::atomic_bool* stop = nullptr) const;
    bool ListPortals(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                     EntityScanInfo& info, const std::atomic_bool* stop = nullptr) const;
    // Combat scans retain independently verified entities when another entity
    // despawns. info.unreadable reports unknowns; an empty result is not a kill.
    bool ListMonsters(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                      EntityScanInfo& info, const std::atomic_bool* stop = nullptr) const;
    bool ListEncounterTargets(uint64_t moduleBase, const WorldAreaSnapshot& area,
                              std::vector<EntitySnapshot>& out, EntityScanInfo& info,
                              const std::atomic_bool* stop = nullptr) const;
    // Keyword candidates are allowed to have EntityKind::None; only positioned
    // entities are returned so every row can be used as a navigation target.
    bool ListRitualCandidates(uint64_t moduleBase, std::vector<EntitySnapshot>& out,
                              EntityScanInfo& info, const std::atomic_bool* stop = nullptr) const;

private:const mem::GameMemory* m_mem;
    bool ListKind(uint64_t moduleBase, EntityKind kind, std::vector<EntitySnapshot>& out,
                  EntityScanInfo& info, const std::atomic_bool* stop) const;
    bool ListMatching(uint64_t moduleBase, EntityKind kind, bool ritualCandidates,
                      std::vector<EntitySnapshot>& out, EntityScanInfo& info,
                      const std::atomic_bool* stop, const WorldAreaSnapshot* encounterArea = nullptr) const;
};

} // namespace poe
