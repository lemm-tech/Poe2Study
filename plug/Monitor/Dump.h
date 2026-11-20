#pragma once
// ============================================================
// Dump —— Monitor 侧的代价表落盘（诊断用，不经过管道）
//
// 为什么放在 Monitor 里而不是 Plug：
//   Plug 拿到的图是"经过 nibble 打包 -> 管道 -> 解包"的产物，
//   如果这条链路有问题（打包反了、行错位），Plug 出的图也是错的，
//   但看不出错在哪。Monitor 在游戏进程内直接读数组、直接画，
//   两份图一对比就能定位"是读错了还是传错了"。
//
// 三个着色策略与 Plug 侧 MapShowcase 保持一致（同一个视觉语言）：
//   raw       0=黑, 1..4 逐档变灰, 0xFF=白
//   grade     1~4 档分别染色深灰/灰/浅灰/白，一眼看出减速带
//   walkable  只有 0 和 255（纯通行图，最像"能不能走"）
//
// 本文件纯头文件 + inline，只有 Monitor 用（依赖 CostBuffer）。
// ============================================================
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "../Common/MapFrame.h"

#include "../Plug/BmpWriter.h"

namespace dump {

enum class Style : uint8_t {
    Raw      = 0,
    Grade    = 1,
    Walkable = 2,
};

inline uint8_t ToGray(mapdata::Cost c, Style s)
{
    switch (s) {
    case Style::Walkable:
        return (c != 0) ? 255 : 0;

    case Style::Grade:
        if (c == 0) return 0;
        if (c == kCostSpecial) return 255;
        // 1..4 档：越难走越暗（1 最亮，4 最暗但非黑）
        switch (c) {
        case 1: return 210;
        case 2: return 160;
        case 3: return 110;
        case 4: return 70;
        default: return 40;
        }

    case Style::Raw:
    default:
        if (c == 0) return 0;
        if (c == kCostSpecial) return 255;
        return (uint8_t)(40 + c * 40);
    }
}

inline const char* StyleName(Style s)
{
    switch (s) {
    case Style::Grade:    return "grade";
    case Style::Walkable: return "walkable";
    default:              return "raw";
    }
}

// 在这张灰度图上画一个 5x5 的方块（标记玩家/目标）
inline void Mark(std::vector<uint8_t>& gray, uint32_t W, uint32_t H,
                 int32_t x, int32_t y, uint8_t v)
{
    if (x < 0 || y < 0 || (uint32_t)x >= W || (uint32_t)y >= H) return;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int nx = (int)x + dx;
            const int ny = (int)y + dy;
            if (nx < 0 || ny < 0 || (uint32_t)nx >= W || (uint32_t)ny >= H) continue;
            gray[(size_t)ny * W + (size_t)nx] = v;
        }
    }
}

// 把路径点直接画成深色，让"路径是否贴墙"肉眼可见。
// 只依赖 {int32 x; int32 y;} 这个布局（NavBatchChunk / Waypoint 都是它）。
template <typename Pt>
inline void DrawPath(std::vector<uint8_t>& gray, uint32_t W, uint32_t H,
                     const std::vector<Pt>& pts, uint8_t v)
{
    for (const Pt& p : pts) {
        if (p.x < 0 || p.y < 0 || (uint32_t)p.x >= W || (uint32_t)p.y >= H) continue;
        gray[(size_t)p.y * W + (size_t)p.x] = v;
    }
}

// 把 CostBuffer 存成 BMP。返回 false = 数据为空 / 写文件失败。
inline bool SaveCostBuffer(const mapdata::CostBuffer& cost,
                           const std::string& path,
                           Style style,
                           int32_t markX = -1, int32_t markY = -1)
{
    if (cost.Empty() || path.empty()) {
        return false;
    }
    const mapdata::Geometry& geo = cost.Geo();
    const uint32_t W = geo.WorldW();
    const uint32_t H = geo.WorldH();
    if (W == 0 || H == 0) {
        return false;
    }

    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            gray[(size_t)y * W + x] = ToGray(cost.At(x, y), style);
        }
    }

    if (markX >= 0 && markY >= 0) {
        Mark(gray, W, H, markX, markY, 255);   // 亮白点 = 标记
    }

    return bmp::WriteGray8(path, gray.data(), W, H);
}

} // namespace dump
