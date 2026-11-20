#include "Inventory.h"
#include "../Common/Logger.h"
#include "../Common/PointerChain.h"

#include <stack>

namespace poe {
namespace {

// 品级名称表
const char* const kGradeNames[] = { "白装", "蓝装", "金装", "暗金" };

// 递归深度保护：背包树正常几十个节点，异常数据会退化成链表
constexpr size_t kMaxVisited = 4096;

} // namespace

const char* GradeName(int grade)
{
    if (grade >= 0 && grade <= 3) {
        return kGradeNames[grade];
    }
    return "NONE";
}

InventoryTraverser::InventoryTraverser(const mem::GameMemory& memory)
    : m_mem(&memory)
{
}
void InventoryTraverser::PreOrder(uint64_t node, std::vector<InventoryItem>& out) const
{
    // 用显式栈代替递归：数据异常时不会把调用栈打爆
    std::stack<uint64_t> pending;
    pending.push(node);

    size_t visited = 0;
    while (!pending.empty()) {
        const uint64_t cur = pending.top();
        pending.pop();

        if (cur == 0 || !m_mem->IsPlausible(cur)) {
            continue;
        }
        if (++visited > kMaxVisited) {
            LOGW("[Inventory] 遍历节点数超过 %zu，疑似数据异常，提前终止", kMaxVisited);
            break;
        }

        // flag == 1 => 哨兵/末尾节点，终止
        if (m_mem->Read<uint8_t>(cur + kNodeFlagOff) == 1) {
            continue;
        }

        InventoryItem item;
        if (ReadItem(cur, item)) {
            out.push_back(item);
        }

        // 栈是 LIFO：先压右、再压左，弹出顺序才是「先左后右」，
        // 与旧实现的递归 PreOrder(left) -> PreOrder(right) 保持一致
        pending.push(m_mem->Read<uint64_t>(cur + kNodeRightOff));
        pending.push(m_mem->Read<uint64_t>(cur + kNodeLeftOff));
    }
}

bool InventoryTraverser::ReadItem(uint64_t node, InventoryItem& item) const
{
    item.key = m_mem->Read<uint8_t>(node + kNodeKeyOff);
    item.num = 1;
    item.grade = GRADE_NONE;

    // 背包树的 entity 需要解两层：[[node+0x28]]
    // （周围遍历只解一层，这是两条链的关键差异）
    const uint64_t slot = m_mem->Follow(node + kNodeEntityOff);
    if (slot == 0) {
        return false;
    }
    const uint64_t entity = m_mem->Follow(slot);
    if (entity == 0) {
        return false;
    }
    item.entity = entity;

    // 组件表只解析一次，字段都从这个 Entity 取
    const Entity ent(*m_mem, entity);

    if (auto base = ent.GetAs<BaseComponent>("Base")) {
        item.name = base->GetItemName();
    }
    if (auto stack = ent.GetAs<StackComponent>("Stack")) {
        item.num = stack->GetCount();
    }
    if (auto mods = ent.GetAs<ModsComponent>("Mods")) {
        item.prefix = mods->GetPrefixName();
        item.grade = mods->GetGrade();
    }
    return true;
}

bool InventoryTraverser::TraverseFromRoot(std::vector<InventoryItem>& out, uint64_t root) const
{
    out.clear();
    if (root == 0) {
        LOGW("[Inventory] 根节点为空");
        return false;
    }
    PreOrder(root, out);
    return true;
}

bool InventoryTraverser::Traverse(std::vector<InventoryItem>& out, uint64_t moduleBase) const
{
    out.clear();

    // 背包链 -> 哨兵节点（末级解引用）
    const uint64_t sentinel = mem::PointerChain(
        InventoryChainOffsets(), kInventoryChainDepth, mem::TailOp::Deref)
        .Resolve(*m_mem, moduleBase);

    if (sentinel == 0) {
        LOGW("[Inventory] 背包链解析失败（基址 0x%llX）", (unsigned long long)moduleBase);
        return false;
    }

    // 哨兵节点的 parent 字段就是根
    const uint64_t root = m_mem->Follow(sentinel + kNodeParentOff);
    if (root == 0) {
        LOGW("[Inventory] 哨兵 0x%llX 的 parent 为空", (unsigned long long)sentinel);
        return false;
    }

    PreOrder(root, out);
    return true;
}

} // namespace poe
