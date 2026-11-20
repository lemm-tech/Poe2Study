#pragma once
// ============================================================
// MapFrame.h —— 地图代价表：内存格式(CostBuffer) 与 线上格式(MapFrame)
//
// 收掉的重复：
//   原来 nibble 的打包/解包散在 3+ 处：
//     Monitor\dllmain.cpp   SendMapOverPipe  手动 (hi<<4)|lo
//     Plug\MapRenderer.cpp  OnMapRow         NibbleToCost 两次
//     tools\mapclient_test.cpp              又一遍
//   现在只有 CostBuffer::ToFrame / MapFrame::ToCost 两个入口。
//
// 关键语义（CostCall 汇编实证，不要凭直觉改）：
//   · x 偶数 -> 高 4 位；x 奇数 -> 低 4 位（cmovne 选 value>>4）
//   · nibble 5  <=>  CostCall 返回 0xFF（最高可通行档的特殊标记）
//     互转统一走 GameOffsets.h 的 CostToNibble / NibbleToCost
//
// 本文件纯头文件，无 .cpp，Plug 与 Monitor 共用。
// ============================================================
#include <cstdint>
#include <vector>

#include "GameOffsets.h"
#include "MapGeometry.h"

namespace mapdata {

// 每个世界点的代价值（0~255，其中 0xFF 是特殊标记）
using Cost = uint8_t;

class CostBuffer;   // 前向声明

// ------------------------------------------------------------
// MapFrame —— 线上格式：nibble 打包，长度 = RowStride * WorldH
// ------------------------------------------------------------
class MapFrame {
public:
    // 元信息（管道 MT_MapMeta 直接照抄这几个字段）
    struct Meta {
        uint32_t width      = 0;   // 世界像素宽（不是网格）
        uint32_t height     = 0;   // 世界像素高
        uint32_t rowStride  = 0;   // 每行打包字节数
        uint8_t  layer      = 0;
    };

    // 一行切片（供管道分块：一行的数据可能跨多个包）
    struct RowSlice {
        uint32_t       y    = 0;   // 行号
        uint32_t       off  = 0;   // 本片在这一行内的字节偏移
        const uint8_t* data = nullptr;
        uint32_t       len  = 0;
    };

    MapFrame() = default;

    Meta              m_meta;
    std::vector<uint8_t> m_packed;    // nibble 打包后的字节

    bool   Empty()      const { return m_packed.empty(); }
    size_t PackedSize() const { return m_packed.size(); }
    const  Meta& GetMeta() const { return m_meta; }

    Geometry Geo() const
    {
        return Geometry::FromGrid(
            m_meta.width  / CELL_SIZE,
            m_meta.height / CELL_SIZE);
    }

    // 准备一行切片（调用方给 offset 和 chunk，剩余部分自行循环）
    RowSlice SliceRow(uint32_t y, uint32_t off, uint32_t chunk) const
    {
        RowSlice s;
        s.y  = y;
        s.off = off;
        const size_t base = (size_t)y * m_meta.rowStride + off;
        if (base >= m_packed.size()) return s;
        const size_t remain = m_packed.size() - base;
        s.len  = (uint32_t)((chunk < remain) ? chunk : remain);
        s.data = m_packed.data() + base;
        return s;
    }

    // ---- 解包：声明在这里，实现放到 CostBuffer 定义之后 ----
    // （CostBuffer 用到本类，本类的方法又要用 CostBuffer 的成员，
    //   所以两边都得先有完整定义；用"类内声明 + 类外 inline 实现"打破循环）
    bool ToCost(CostBuffer& out) const;

    // 单行解包（接收端边收边解，不必等全部收齐）
    void DecodeRow(uint32_t y, uint32_t off, const uint8_t* src, uint32_t len,
                   CostBuffer& out) const;

private:
    void DecodeInto(CostBuffer& out) const;
};

// ------------------------------------------------------------
// CostBuffer —— 内存格式：每点一个字节，行优先，索引 = y*WorldW + x
// ------------------------------------------------------------
class CostBuffer {
public:
    CostBuffer() = default;
    explicit CostBuffer(const Geometry& geo) { Resize(geo); }

    void Resize(const Geometry& geo)
    {
        m_geo = geo;
        m_cost.assign((size_t)geo.PixelCount(), 0);
    }

    void Clear()
    {
        m_geo = Geometry();
        m_cost.clear();
    }

    const Geometry& Geo() const { return m_geo; }
    bool   Empty()    const { return m_cost.empty(); }
    size_t Size()     const { return m_cost.size(); }

    // 未做边界检查的快速访问（内部循环用）
    Cost&       At(uint32_t x, uint32_t y)       { return m_cost[(size_t)y * m_geo.WorldW() + x]; }
    Cost        At(uint32_t x, uint32_t y) const { return m_cost[(size_t)y * m_geo.WorldW() + x]; }

    // 带边界检查的访问（对外接口用）
    Cost Get(int x, int y) const
    {
        return m_geo.Contains(x, y) ? At((uint32_t)x, (uint32_t)y) : 0;
    }
    void Set(int x, int y, Cost v)
    {
        if (m_geo.Contains(x, y)) At((uint32_t)x, (uint32_t)y) = v;
    }

    const std::vector<Cost>& Raw() const { return m_cost; }
    std::vector<Cost>&       Raw()       { return m_cost; }

    // 统计（诊断用）
    size_t CountNonZero() const
    {
        size_t n = 0;
        for (Cost c : m_cost) if (c != 0) ++n;
        return n;
    }

private:
    Geometry           m_geo;
    std::vector<Cost>  m_cost;
};

// ------------------------------------------------------------
// MapFrame 的解包实现（必须在 CostBuffer 完整定义之后）
// ------------------------------------------------------------
inline void MapFrame::DecodeRow(uint32_t y, uint32_t off,
                                const uint8_t* src, uint32_t len,
                                CostBuffer& out) const
{
    if (!src || len == 0) return;
    if (y >= m_meta.height) return;
    if (out.Empty()) return;

    const uint32_t W = m_meta.width;
    Cost* dst = out.Raw().data() + (size_t)y * W;

    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t x0 = (off + i) * 2;
        if (x0 >= W) break;

        const uint8_t b = src[i];
        // x 偶数 → 高 4 位；x 奇数 → 低 4 位（与 CostCall 汇编一致）
        dst[x0] = NibbleToCost((uint8_t)((b >> 4) & 0x0F));
        if (x0 + 1 < W) {
            dst[x0 + 1] = NibbleToCost((uint8_t)(b & 0x0F));
        }
    }
}

inline void MapFrame::DecodeInto(CostBuffer& out) const
{
    const uint32_t H = m_meta.height;
    for (uint32_t y = 0; y < H; ++y) {
        DecodeRow(y, 0,
                  m_packed.data() + (size_t)y * m_meta.rowStride,
                  m_meta.rowStride, out);
    }
}

inline bool MapFrame::ToCost(CostBuffer& out) const
{
    if (m_meta.width == 0 || m_meta.height == 0) return false;
    if (m_meta.rowStride != (m_meta.width + 1) / 2) return false;

    const Geometry geo = Geo();
    if (!geo.Plausible()) return false;

    // 长度必须够（少一个字节都算没收到；多的忽略）
    if (m_packed.size() < geo.PackedBytes()) return false;

    out.Resize(geo);
    DecodeInto(out);
    return true;
}

// ------------------------------------------------------------
// CostBuffer -> MapFrame（唯一的打包入口）
// ------------------------------------------------------------
inline MapFrame ToFrame(const CostBuffer& cost, uint8_t layer)
{
    MapFrame f;
    const Geometry& geo = cost.Geo();

    f.m_meta.width     = geo.WorldW();
    f.m_meta.height    = geo.WorldH();
    f.m_meta.rowStride = geo.RowStride();
    f.m_meta.layer     = layer;

    const uint32_t W = geo.WorldW();
    const uint32_t H = geo.WorldH();
    f.m_packed.assign((size_t)f.m_meta.rowStride * H, 0);
    if (cost.Empty()) return f;

    for (uint32_t y = 0; y < H; ++y) {
        const Cost* src = cost.Raw().data() + (size_t)y * W;
        uint8_t*    dst = f.m_packed.data() + (size_t)y * f.m_meta.rowStride;

        for (uint32_t xb = 0; xb < f.m_meta.rowStride; ++xb) {
            const uint32_t x0 = xb * 2;
            const uint8_t hi = (x0     < W) ? CostToNibble(src[x0])     : 0;
            const uint8_t lo = (x0 + 1 < W) ? CostToNibble(src[x0 + 1]) : 0;
            dst[xb] = (uint8_t)(((hi & 0x0F) << 4) | (lo & 0x0F));
        }
    }
    return f;
}

} // namespace mapdata
