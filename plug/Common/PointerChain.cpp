#include "PointerChain.h"
#include "Logger.h"

namespace mem {

uint64_t PointerChain::Resolve(const GameMemory& mem, uint64_t moduleBase) const
{
    if (m_offsets.empty() || !mem.IsPlausible(moduleBase)) {
        return 0;
    }
    return mem.FollowChain(moduleBase, m_offsets, m_tail == TailOp::Deref);
}

uint64_t PointerChain::ResolveWithLog(const GameMemory& mem, uint64_t moduleBase, const char* tag) const
{
    const char* name = (tag && *tag) ? tag : "chain";

    if (m_offsets.empty()) {
        LOGW("[%s] 链为空", name);
        return 0;
    }
    if (!mem.IsPlausible(moduleBase)) {
        LOGW("[%s] 模块基址 0x%llX 不合法", name, (unsigned long long)moduleBase);
        return 0;
    }

    LOGI("[%s] moduleBase=0x%llX depth=%d tail=%s",
        name, (unsigned long long)moduleBase, Depth(),
        m_tail == TailOp::Deref ? "Deref" : "Add");

    uint64_t addr = moduleBase;
    for (int i = 0; i < Depth() - 1; ++i) {
        const uint64_t slot = addr + m_offsets[(size_t)i];
        addr = mem.Follow(slot);
        LOGI("[%s] lvl%-2d *(+0x%llX) = 0x%llX", name, i + 1,
            (unsigned long long)m_offsets[(size_t)i], (unsigned long long)addr);
        if (addr == 0) {
            LOGE("[%s] 链在第 %d 级断开", name, i + 1);
            return 0;
        }
    }

    const uint64_t lastSlot = addr + m_offsets[(size_t)(Depth() - 1)];
    if (m_tail == TailOp::Deref) {
        const uint64_t v = mem.Follow(lastSlot);
        LOGI("[%s] lvl%-2d *(+0x%llX) = 0x%llX  <- result",
            name, Depth(), (unsigned long long)m_offsets[(size_t)(Depth() - 1)],
            (unsigned long long)v);
        return v;
    }

    LOGI("[%s] lvl%-2d  (+0x%llX) = 0x%llX  <- result",
        name, Depth(), (unsigned long long)m_offsets[(size_t)(Depth() - 1)],
        (unsigned long long)lastSlot);
    return mem.IsPlausible(lastSlot) ? lastSlot : 0;
}

} // namespace mem
