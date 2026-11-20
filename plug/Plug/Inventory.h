#pragma once
// ============================================================
// Inventory.h —— 背包遍历（用例层）
//
// 重构后这里不再有自己的 Entity 子类和自己的组件工厂：
// 组件表解析与组件对象统一由 Common\Entity / Common\Component 提供。
// InventoryEntity 这个类已删除（它唯一的存在理由就是解析组件表，
// 而那件事已经搬进通用 Entity）。
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <string>
#include <vector>

#include "../Common/Entity.h"
#include "../Common/GameMemory.h"
#include "../Common/GameOffsets.h"

namespace poe {

// 装备品级
enum Grade {
    GRADE_NONE   = -1,
    GRADE_WHITE  = 0,
    GRADE_BLUE   = 1,
    GRADE_GOLD   = 2,
    GRADE_ORANGE = 3
};

// 品级 -> 显示名
const char* GradeName(int grade);

// 一个背包物品（纯数据，不依赖内存访问）
struct InventoryItem {
    uint8_t     key    = 0;       // 格子索引
    uint64_t    entity = 0;       // 物品实体地址
    std::string name;             // 物品名
    std::string prefix;           // 装备前缀（前半 + 后半）
    uint32_t    num    = 1;       // 堆叠数量
    int         grade  = GRADE_NONE;
};

// 背包遍历器
class InventoryTraverser {
public:
        explicit InventoryTraverser(const mem::GameMemory& memory);


    // 沿背包链取哨兵 -> 根 -> 前序遍历
    bool Traverse(std::vector<InventoryItem>& out, uint64_t moduleBase) const;

    // 从一个已知的根节点开始遍历（调试/回归用）
    bool TraverseFromRoot(std::vector<InventoryItem>& out, uint64_t root) const;

private:const mem::GameMemory* m_mem;

    void PreOrder(uint64_t node, std::vector<InventoryItem>& out) const;
    bool ReadItem(uint64_t node, InventoryItem& item) const;
};

} // namespace poe
