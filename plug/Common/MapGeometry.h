#pragma once
// ============================================================
// MapGeometry.h —— 地图几何换算的唯一实现
//
// 收掉的重复：
//   (W+1)/2 这个行跨度算式，原来在 Monitor\dllmain.cpp:53
//   和 Plug\MapRenderer.cpp:32 各写一遍。
//
// 依赖：只有 GameOffsets.h（拿 CELL_SIZE / 网格上限），无其它依赖。
// ============================================================
#include <cstdint>

#include "GameOffsets.h"

namespace mapdata {

// 网格尺寸（游戏内部单位，map+0x18 / map+0x20 的值）
struct GridSize {
    uint32_t w = 0;
    uint32_t h = 0;

    bool Same(const GridSize& o) const { return w == o.w && h == o.h; }
    bool Empty() const { return w == 0 || h == 0; }
};

// 网格 <-> 世界像素 换算
//
// 世界像素 = 网格数 * CELL_SIZE(0x17 = 23)
// 行跨度   = ceil(世界宽 / 2)     —— nibble 打包，每字节 2 点
//
// 网格上限 0x1000 来自 MapScanner 原有的合理性校验，现在收进这里。
class Geometry {
public:
    Geometry() = default;
    explicit Geometry(const GridSize& g) : m_grid(g) {}

    static Geometry FromGrid(uint32_t w, uint32_t h) { return Geometry(GridSize{ w, h }); }

    const GridSize& Grid() const { return m_grid; }

    uint32_t WorldW() const { return m_grid.w * CELL_SIZE; }
    uint32_t WorldH() const { return m_grid.h * CELL_SIZE; }
    uint64_t PixelCount() const { return (uint64_t)WorldW() * WorldH(); }

    // nibble 打包后每行的字节数（向上取整）
    uint32_t RowStride() const { return (WorldW() + 1) / 2; }

    // nibble 打包后的总字节数（= 游戏里代价数组的长度）
    uint64_t PackedBytes() const { return (uint64_t)RowStride() * WorldH(); }

    // 合理性：非 0 且不超过游戏实测上限
    bool Plausible() const
    {
        return m_grid.w > 0 && m_grid.h > 0
            && m_grid.w <= kMaxGridDimension
            && m_grid.h <= kMaxGridDimension;
    }

    // 点是否在世界范围内
    bool Contains(int x, int y) const
    {
        return x >= 0 && y >= 0
            && (uint32_t)x < WorldW()
            && (uint32_t)y < WorldH();
    }

    // 某个世界点在打包数组里的字节下标（不考虑层）
    //   idx = y * RowStride + x/2
    uint64_t PackedIndex(int x, int y) const
    {
        return (uint64_t)y * RowStride() + (uint64_t)(x / 2);
    }

    bool operator==(const Geometry& o) const { return m_grid.Same(o.m_grid); }

private:
    GridSize m_grid;
};

} // namespace mapdata
