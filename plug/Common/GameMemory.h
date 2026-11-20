#pragma once
// ============================================================
// GameMemory.h —— 全项目唯一"能摸到游戏内存的手"
//
// 它收掉的重复（改动前实测数字）：
//   - 地址合法性校验（0xFFFFFFF / 0x7ffffffffff）：28 处 -> 1 处
//   - 手写 kIntTypeU8/U32/U64 选长度码：44 处 -> 0 处
//   - SSO 字符串判断（maxLen > 7 才解堆指针）：5 处 -> 1 处
//   - 链遍历：3 处手写 -> 归 PointerChain
//
// 规矩：
//   - 上层（组件 / 实体 / 遍历器）只能通过本类读内存
//   - 上层不准再出现裸地址范围比较
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <string>
#include <vector>

#include "OlaDriver.h"
#include "GameOffsets.h"

namespace mem {

// 字符串编码
using TextKind = ola::TextKind;

class GameMemory {
public:
    explicit GameMemory(const ola::OlaDriver& driver);

    // ---- 整数读取：模板推导类型，不再手选长度码 ----
    template <class T>
    T Read(uint64_t addr) const
    {
        T v{};
        ReadInto(addr, &v, sizeof(T));
        return v;
    }

    bool ReadInto(uint64_t addr, void* out, size_t size) const;

    // ---- 指针解引用：内置地址校验 ----
    // 地址不合法、或读出的值不合法，一律返回 0（调用方只需判 0）
    uint64_t Follow(uint64_t slot) const;

    // 沿链走：每级解引用，最后一级按 tailOp 处理
    uint64_t FollowChain(uint64_t base, const uint64_t* offsets, int count, bool tailIsDeref) const;
    uint64_t FollowChain(uint64_t base, const std::vector<uint64_t>& offsets, bool tailIsDeref) const;

    // ---- 字符串 ----
    // 读游戏里的 std::string / std::wstring 风格对象：
    // 对象 +0 是数据（SSO 时内联），+0x10 是长度，+0x18 是 maxLen（>7 表示堆指针）
    std::string ReadStdString(uint64_t strObjAddr, TextKind kind = TextKind::Utf16) const;

    // 直接读字符串数据（已知数据地址，不做 SSO 判定）
    std::string ReadText(uint64_t dataAddr, TextKind kind = TextKind::Utf16) const;

    // ---- 大块数据（代价表等）----
    bool ReadBlob(uint64_t addr, size_t size, std::vector<uint8_t>& out) const;

    // ---- 基础信息 ----
    bool IsPlausible(uint64_t addr) const { return IsPlausibleAddress(addr); }
    uint64_t ModuleBase(const std::string& moduleName) const;
    const ola::OlaDriver& Driver() const { return m_driver; }

private:
    const ola::OlaDriver& m_driver;
};

} // namespace mem
