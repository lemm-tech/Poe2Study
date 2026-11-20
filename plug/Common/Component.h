#pragma once
// ============================================================
// Component.h —— 组件基类 + 全部组件子类 + 组件工厂（单文件集中）
//
// 为什么集中在一个文件：这些组件是同源同构的兄弟——构造签名一致、
// 都是纯读取器、都因为同一个原因（游戏更新改偏移）而改变。
// 拆成 8 个文件会让"新增组件"从 3 步 1 文件变成 5 步跨 3 文件。
// 详见方案讨论。文件冲上 1500 行或某组件长出独立复杂度时再拆。
//
// 组件基类只有一个职责：持有内存访问器 + 组件地址。
// 它不含任何实体逻辑，所以实体和组件是彻底解耦的。
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>

#include "GameMemory.h"
#include "../Common/GameOffsets.h"

namespace poe {

class Component;
using ComponentPtr = std::shared_ptr<Component>;

// ============================================================
// 组件基类
// ============================================================
class Component {
protected:
    const mem::GameMemory* m_mem;
    uint64_t m_ptr;

public:
    Component(const mem::GameMemory& mem, uint64_t ptr)
        : m_mem(&mem), m_ptr(ptr) {}

    virtual ~Component() = default;

    uint64_t Ptr() const { return m_ptr; }
    bool IsValid() const { return m_mem->IsPlausible(m_ptr); }
};


// ============================================================
// 通用组件（包围 + 背包都用）
// ============================================================

// Render：实体显示名
class RenderComponent : public Component {
public:
    using Component::Component;
    std::string GetRenderName() const;
};

// Positioned：坐标（+0x444 x、+0x448 y，各一个 int32，值就是世界像素）
struct EntityPosition {
    int32_t x = 0;
    int32_t y = 0;
};

class PositionComponent : public Component {
public:
    using Component::Component;
    EntityPosition GetPosition() const;
};

// Life：血量
class LifeComponent : public Component {
public:
    using Component::Component;
    uint32_t GetHp() const;
};

// Base：物品名
class BaseComponent : public Component {
public:
    using Component::Component;
    std::string GetItemName() const;
};


// ============================================================
// 周围遍历专用组件
// ============================================================

// Player：玩家名
class PlayerComponent : public Component {
public:
    using Component::Component;
    std::string GetPlayerName() const;
};

// WorldItem：指向掉落物物品实体
class WorldItemComponent : public Component {
public:
    using Component::Component;
    uint64_t GetItemEntity() const;
};


// ============================================================
// 背包专用组件
// ============================================================

// Stack：可堆叠数量
class StackComponent : public Component {
public:
    using Component::Component;
    uint32_t GetCount() const;
};

// Mods：前缀 / 品级
class ModsComponent : public Component {
public:
    using Component::Component;
    std::string GetPrefixName() const;   // 前半 + 后半（中间不加空格）
    int GetGrade() const;                // 0白 1蓝 2金 3暗金，无效返回 -1
};


// ============================================================
// 组件工厂：全局唯一注册表
// ============================================================
// 历史坑：原来背包侧和周围侧各有一份 static map，同一个 "Base"
// 被注册成两种不同东西，导致背包侧 dynamic_pointer_cast 拿不到。
// 现在只有这一张表。
namespace component_factory {

using Ctor = std::function<ComponentPtr(const mem::GameMemory&, uint64_t)>;

// 取注册表（function-local static，规避静态初始化顺序问题）
std::unordered_map<std::string, Ctor>& Table();

// 一次性注册全部组件（幂等；首次解析实体组件表时自动调用）
void RegisterAll();

// 按组件名创建；未注册返回 nullptr
ComponentPtr Create(const std::string& name, const mem::GameMemory& mem, uint64_t ptr);

} // namespace component_factory

} // namespace poe
