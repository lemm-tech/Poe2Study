#pragma once
// ============================================================
// Entity.h —— 通用实体：一个地址 + 它的组件表
//
// 设计要点：
//   1. 只干三件事：解析组件表、按名/按类型取组件、读元数据路径。
//      不含任何"我是背包物品"或"我是怪物"的业务判断 —— 那是上层的事。
//   2. 惰性解析：真正要组件时才去读组件表，之后全部命中缓存。
//      历史上构造函数立刻把整张表读完，周围遍历一次扫几百实体时
//      会白读大量用不到的组件。
//   3. 组件表解析只有这一份实现。
//      原来是 InventoryEntity 和 Entity 各抄一遍（逐字雷同），
//      修一个 bug 要改两处，漏改一半就是"一边好一边坏"。
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

#include "Component.h"
#include "GameMemory.h"

namespace poe {

class Entity {
public:
    Entity(const mem::GameMemory& mem, uint64_t ptr);

    // 按组件名取；不存在或未注册返回 nullptr。首次调用触发组件表解析
    ComponentPtr Get(const std::string& name) const;

    // 按类型取（内部 dynamic_pointer_cast），高频路径可直接用 Get() + 一次 cast
    template <class T>
    std::shared_ptr<T> GetAs(const std::string& name) const
    {
        return std::dynamic_pointer_cast<T>(Get(name));
    }

    // 元数据路径（UTF-16，惰性 + 缓存）
    const std::string& MetadataPath() const;

    uint64_t Ptr() const { return m_ptr; }
    bool IsValid() const { return m_mem->IsPlausible(m_ptr); }

    // 组件表是否已成功解析（调试/诊断用）
    bool HasComponents() const;

    // 已解析出的组件名列表（诊断未注册组件用）
    std::vector<std::string> ComponentNames() const;

private:
    void EnsureParsed() const;

    const mem::GameMemory* m_mem;
    uint64_t m_ptr;

    mutable bool m_parsed = false;
    mutable std::unordered_map<std::string, ComponentPtr> m_components;

    mutable bool m_metadataLoaded = false;
    mutable std::string m_metadataPath;
};

} // namespace poe
