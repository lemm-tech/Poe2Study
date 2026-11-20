#pragma once
// ============================================================
// PointerChain.h —— 把"指针链"变成一等公民
//
// 为什么要它：项目里两条链的末级语义本来就不同，但这个差异
// 一直只写在注释里，靠人记：
//     背包链：末级是"解引用"（落点就是哨兵节点）
//     地图链：末级是"只加法"（落点就是地图对象）
// 记反了就是崩溃或垃圾数据。
//
// 现在把它变成一个显式参数 TailOp，写错了编译器不管但代码里看得见。
//
// 顺带的红利：tools/probe_base.cpp 里手写的两份链遍历可以删掉，
// 探针与主程序从此走同一份逻辑（原来是两份手写，还出现过"链 A
// 跟链 B 自比自"的假阳性）。
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <vector>
#include <initializer_list>

#include "GameMemory.h"

namespace mem {

// 末级语义
enum class TailOp {
    Deref,   // 最后一跳要解引用（背包/周围树）
    Add      // 最后一跳只做加法（地图对象）
};

class PointerChain {
public:
    PointerChain() = default;

    PointerChain(std::initializer_list<uint64_t> offsets, TailOp tail)
        : m_offsets(offsets), m_tail(tail) {}

    PointerChain(const uint64_t* offsets, int count, TailOp tail)
        : m_offsets(offsets, offsets + (count > 0 ? count : 0)), m_tail(tail) {}

    // 解链：从模块基址出发。任何一级断了都返回 0
    uint64_t Resolve(const GameMemory& mem, uint64_t moduleBase) const;

    // 解链并写日志（探针/调试用）
    uint64_t ResolveWithLog(const GameMemory& mem, uint64_t moduleBase, const char* tag) const;

    const std::vector<uint64_t>& Offsets() const { return m_offsets; }
    TailOp Tail() const { return m_tail; }
    int Depth() const { return (int)m_offsets.size(); }
    bool Empty() const { return m_offsets.empty(); }

private:
    std::vector<uint64_t> m_offsets;
    TailOp m_tail = TailOp::Deref;
};

} // namespace mem
