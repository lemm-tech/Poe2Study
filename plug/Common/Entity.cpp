#include "Entity.h"
#include "Logger.h"

namespace poe {

Entity::Entity(const mem::GameMemory& mem, uint64_t ptr)
    : m_mem(&mem), m_ptr(ptr)
{
}

void Entity::EnsureParsed() const
{
    if (m_parsed) {
        return;
    }
    m_parsed = true;   // 只尝试一次，失败也不反复重试（避免每个 Get 都烧一堆远程读）

    if (!m_mem->IsPlausible(m_ptr)) {
        return;
    }

    // 组件信息对数组：header+0x28 为首地址，header+0x30 为尾地址。
    uint64_t pairsHeader = m_mem->Follow(m_ptr + kEntityPairsOff0);
    if (pairsHeader == 0) {
        return;
    }
    pairsHeader = m_mem->Follow(pairsHeader + kEntityPairsOff1);
    if (pairsHeader == 0) {
        return;
    }
    const uint64_t pairsAddress = m_mem->Follow(pairsHeader + kEntityPairsOff2);
    if (pairsAddress == 0) {
        return;
    }
    const uint64_t pairsEnd = m_mem->Read<uint64_t>(pairsHeader + kEntityPairsEndOff);
    if (pairsEnd <= pairsAddress) {
        LOGW("[Entity] 0x%llX 组件信息对数组范围非法（begin=0x%llX end=0x%llX），跳过解析",
            (unsigned long long)m_ptr,
            (unsigned long long)pairsAddress,
            (unsigned long long)pairsEnd);
        return;
    }
    const uint64_t pairBytes = pairsEnd - pairsAddress;
    if (pairBytes % kComponentPairSize != 0) {
        LOGW("[Entity] 0x%llX 组件信息对数组长度非法（%llu 字节，非 0x%llX 的整数倍），跳过解析",
            (unsigned long long)m_ptr,
            (unsigned long long)pairBytes,
            (unsigned long long)kComponentPairSize);
        return;
    }
    const uint64_t pairCount = pairBytes / kComponentPairSize;

    // 组件对象指针数组：[entity+0x10] .. [entity+0x18]
    const uint64_t components = m_mem->Follow(m_ptr + kEntityComponentsOff);
    if (components == 0) {
        return;
    }
    const uint64_t componentsEnd = m_mem->Read<uint64_t>(m_ptr + kEntityComponentsEndOff);
    if (componentsEnd <= components) {
        return;
    }

    const uint64_t componentBytes = componentsEnd - components;
    if (componentBytes % kComponentPtrSize != 0) {
        LOGW("[Entity] 0x%llX 组件对象指针数组长度非法（%llu 字节，非 0x%llX 的整数倍），跳过解析",
            (unsigned long long)m_ptr,
            (unsigned long long)componentBytes,
            (unsigned long long)kComponentPtrSize);
        return;
    }
    uint64_t count = componentBytes / kComponentPtrSize;
    if (pairCount != count) {
        LOGW("[Entity] 0x%llX 组件数量不一致（信息对=%llu，对象指针=%llu），跳过解析",
            (unsigned long long)m_ptr,
            (unsigned long long)pairCount,
            (unsigned long long)count);
        return;
    }

    const bool clamped = (count > kMaxComponentCount);
    if (clamped) {
        // 数据错位的异常实体会算出天文数字，卡上限防止长时间空转
        count = kMaxComponentCount;
    }

    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t pairSlot = pairsAddress + i * kComponentPairSize;

        // 组件名指针
        const uint64_t pname = m_mem->Follow(pairSlot);
        if (pname == 0) {
            break;
        }
        const std::string aname = m_mem->ReadText(pname, mem::TextKind::Ansi);

        // 组件索引 = 组件对象数组下标
        const uint32_t idx = m_mem->Read<uint32_t>(pairSlot + kComponentPairIndexOff);

        const uint64_t pComponent = m_mem->Follow(components + (uint64_t)idx * kComponentPtrSize);
        if (pComponent == 0) {
            continue;
        }

        ComponentPtr comp = component_factory::Create(aname, *m_mem, pComponent);
        if (!comp) {
            // 没注册的组件直接跳过；诊断时可用 ComponentNames() 看漏了什么
            continue;
        }
        m_components[aname] = comp;
    }

    if (clamped) {
        LOGW("[Entity] 0x%llX 组件数量异常（已截断到 %llu），疑似组件表地址错位",
            (unsigned long long)m_ptr, (unsigned long long)kMaxComponentCount);
    }
}

ComponentPtr Entity::Get(const std::string& name) const
{
    EnsureParsed();
    auto it = m_components.find(name);
    return (it == m_components.end()) ? nullptr : it->second;
}

const std::string& Entity::MetadataPath() const
{
    if (m_metadataLoaded) {
        return m_metadataPath;
    }
    m_metadataLoaded = true;

    // 元数据路径：entity+0x8 -> +0x8，UTF-16 std::string
    const uint64_t pStrHolder = m_mem->Follow(m_ptr + kMetadataPathOff0);
    if (pStrHolder == 0) {
        return m_metadataPath;
    }
    m_metadataPath = m_mem->ReadStdString(pStrHolder + kMetadataPathOff1, mem::TextKind::Utf16);
    return m_metadataPath;
}

bool Entity::HasComponents() const
{
    EnsureParsed();
    return !m_components.empty();
}

std::vector<std::string> Entity::ComponentNames() const
{
    EnsureParsed();
    std::vector<std::string> names;
    names.reserve(m_components.size());
    for (const auto& kv : m_components) {
        names.push_back(kv.first);
    }
    return names;
}

} // namespace poe
