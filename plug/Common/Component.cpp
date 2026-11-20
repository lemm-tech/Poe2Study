#include "Component.h"

namespace poe {

// ============================================================
// 通用组件实现
// ============================================================

std::string RenderComponent::GetRenderName() const
{
    return m_mem->ReadStdString(m_ptr + kRender_NameOff);
}

// ============================================================
// 实体坐标（玩家与周围遍历共用同一对偏移）
//
//   读取 +0x444 / +0x448，各是一个 int32，值就是世界像素，
//   不需要除以任何比例。偏移说明见 GameOffsets.h。
//
//   历史：0x490 那对是打包的 float 对（要 /10.88），在周围实体上会
//   读到 0 或明显偏离实际位置，已全面弃用。
// ============================================================
EntityPosition PositionComponent::GetPosition() const
{
    EntityPosition pos;
    pos.x = m_mem->Read<int32_t>(m_ptr + kPositioned_EntXOff);
    pos.y = m_mem->Read<int32_t>(m_ptr + kPositioned_EntYOff);
    return pos;
}

uint32_t LifeComponent::GetHp() const
{
    return m_mem->Read<uint32_t>(m_ptr + kLife_HpOff);
}

std::string BaseComponent::GetItemName() const
{
    const uint64_t info = m_mem->Follow(m_ptr + kBase_ItemInfoOff);
    if (info == 0) {
        return std::string();
    }
    return m_mem->ReadStdString(info + kBase_NameOff);
}

// ============================================================
// 周围专用组件实现
// ============================================================

std::string PlayerComponent::GetPlayerName() const
{
    return m_mem->ReadStdString(m_ptr + kPlayer_NameOff);
}

uint64_t WorldItemComponent::GetItemEntity() const
{
    return m_mem->Follow(m_ptr + kWorldItem_EntityOff);
}

// ============================================================
// 背包专用组件实现
// ============================================================

uint32_t StackComponent::GetCount() const
{
    const uint8_t count = m_mem->Read<uint8_t>(m_ptr + kStack_CountOff);
    return count > 0 ? (uint32_t)count : 1u;
}

std::string ModsComponent::GetPrefixName() const
{
    const uint64_t header = m_ptr + kMods_VectorOff;
    uint64_t bounds[3]{};
    if (!m_mem->ReadInto(header, bounds, sizeof(bounds))) return {};
    const auto begin = bounds[0], end = bounds[1], capacity = bounds[2];
    if (!m_mem->IsPlausible(begin) || end < begin || capacity < end ||
        (end - begin) % kMods_PrefixEntryStride || (capacity - begin) % kMods_PrefixEntryStride ||
        end - begin > kMods_MaxNameParts * kMods_PrefixEntryStride ||
        capacity - begin > 64 * kMods_PrefixEntryStride) return {};
    std::string name;
    for (auto entry = begin; entry < end; entry += kMods_PrefixEntryStride) {
        const auto record = m_mem->Follow(entry);
        if (!record) return {};
        const auto text = m_mem->Follow(record + kMods_PrefixNameOff);
        if (!text) return {};
        const auto part = m_mem->ReadText(text);
        if (part.empty()) return {};
        name += part;
    }
    uint64_t current[3]{};
    if (!m_mem->ReadInto(header, current, sizeof(current)) ||
        current[0] != begin || current[1] != end || current[2] != capacity) return {};
    return name;
}

int ModsComponent::GetGrade() const
{
    const uint8_t grade = m_mem->Read<uint8_t>(m_ptr + kMods_GradeOff);
    return (grade <= 3) ? (int)grade : -1;   // -1 == NONE
}

// ============================================================
// 组件工厂
// ============================================================

namespace component_factory {

std::unordered_map<std::string, Ctor>& Table()
{
    static std::unordered_map<std::string, Ctor> table;
    return table;
}

void RegisterAll()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    auto& t = Table();
    // ---- 通用 ----
    t["Render"]     = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<RenderComponent>(m, p); };
    t["Positioned"] = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<PositionComponent>(m, p); };
    t["Life"]       = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<LifeComponent>(m, p); };
    t["Base"]       = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<BaseComponent>(m, p); };
    // ---- 周围专用 ----
    t["Player"]     = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<PlayerComponent>(m, p); };
    t["WorldItem"]  = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<WorldItemComponent>(m, p); };
    // ---- 背包专用 ----
    t["Stack"]      = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<StackComponent>(m, p); };
    t["Mods"]       = [](const mem::GameMemory& m, uint64_t p) { return std::make_shared<ModsComponent>(m, p); };
}

ComponentPtr Create(const std::string& name, const mem::GameMemory& mem, uint64_t ptr)
{
    RegisterAll();
    auto& t = Table();
    auto it = t.find(name);
    if (it == t.end()) {
        return nullptr;
    }
    return it->second(mem, ptr);
}

} // namespace component_factory

} // namespace poe
