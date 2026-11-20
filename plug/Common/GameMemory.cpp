#include "GameMemory.h"
#include "Logger.h"

namespace mem {
// 游戏 std::string / std::wstring 布局（实测，见 GameOffsets.h 的 kStdStringDataOff）：
//   +0x00  数据（SSO 时数据就在对象内部；长字符串时这里是堆指针）
//   +0x18  maxLen / 容量；> 7 表示数据在堆上，需要再解一次指针
// 注意：调用方传进来的地址是"字符串对象起始地址"，不是数据地址。
// Base 物品名 info+0x30 是字符串对象；Mods 名称行+0x30 则是 UTF-16Z 指针，须用 ReadText。
//
// ★ 偏移一律从 GameOffsets.h 取，这里不再单独写一份。
//   历史上本文件与 GameOffsets.h 各有一份 kStdStringDataOff，一个 +0x00
//   一个 +0x10，谁对谁错只能靠翻注释；现在只有 GameOffsets.h 一处。
constexpr uint64_t kStdStringDataOff   = ::kStdStringDataOff;
constexpr uint64_t kStdStringMaxLenOff = ::kStdStringMaxLenOff;
constexpr uint32_t kStdStringSsoLimit  = ::kStdStringSsoLimit;

GameMemory::GameMemory(const ola::OlaDriver& driver)
    : m_driver(driver)
{
}

bool GameMemory::ReadInto(uint64_t addr, void* out, size_t size) const
{
    if (!IsPlausible(addr) || out == nullptr || size == 0) {
        return false;
    }
    return m_driver.ReadRaw(addr, out, size);
}

uint64_t GameMemory::Follow(uint64_t slot) const
{
    if (!IsPlausible(slot)) {
        return 0;
    }
    uint64_t v = 0;
    if (!m_driver.ReadU64(slot, v)) {
        return 0;
    }
    return IsPlausible(v) ? v : 0;
}

uint64_t GameMemory::FollowChain(uint64_t base, const uint64_t* offsets, int count, bool tailIsDeref) const
{
    if (offsets == nullptr || count <= 0 || !IsPlausible(base)) {
        return 0;
    }

    uint64_t addr = base;
    for (int i = 0; i < count - 1; ++i) {
        addr = Follow(addr + offsets[i]);
        if (addr == 0) {
            return 0;
        }
    }

    const uint64_t lastSlot = addr + offsets[count - 1];
    return tailIsDeref ? Follow(lastSlot) : (IsPlausible(lastSlot) ? lastSlot : 0);
}

uint64_t GameMemory::FollowChain(uint64_t base, const std::vector<uint64_t>& offsets, bool tailIsDeref) const
{
    return FollowChain(base, offsets.data(), (int)offsets.size(), tailIsDeref);
}

std::string GameMemory::ReadStdString(uint64_t strObjAddr, TextKind kind) const
{
    if (!IsPlausible(strObjAddr)) {
        return std::string();
    }

    uint64_t dataAddr = strObjAddr + kStdStringDataOff;
    const uint32_t maxLen = Read<uint32_t>(strObjAddr + kStdStringMaxLenOff);

    if (maxLen > kStdStringSsoLimit) {
        // 长字符串：对象里存的是堆指针，再解一次
        dataAddr = Follow(dataAddr);
        if (dataAddr == 0) {
            return std::string();
        }
    }
    return ReadText(dataAddr, kind);
}

std::string GameMemory::ReadText(uint64_t dataAddr, TextKind kind) const
{
    if (!IsPlausible(dataAddr)) {
        return std::string();
    }
    return (kind == TextKind::Ansi)
        ? m_driver.ReadAnsi(dataAddr)
        : m_driver.ReadUtf16(dataAddr);
}

bool GameMemory::ReadBlob(uint64_t addr, size_t size, std::vector<uint8_t>& out) const
{
    out.clear();
    if (!IsPlausible(addr) || size == 0) {
        return false;
    }
    return m_driver.ReadBlob(addr, size, out);
}

uint64_t GameMemory::ModuleBase(const std::string& moduleName) const
{
    return m_driver.ModuleBase(moduleName);
}

} // namespace mem
