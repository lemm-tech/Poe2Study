#pragma once
// ============================================================
// MapSink.h —— 地图数据接收端接口
//
// 为什么需要这一层：Plug\MapRenderer 一个类干了 5 件事——
//   收元信息 / 收行 / 解包 / 判完成 / 转灰度存盘。
//   其中"数据从哪来"（管道 / 本地 / 文件回放）和"怎么显示"（黑白/分级）
//   跟"怎么接收"毫无关系，却被焊在一起。
//
// 拆开之后：
//   IMapSink       —— 从某个来源收一张 MapFrame，产出 CostBuffer
//     PipeSink     —— 命名管道（生产路径）
//     ReplaySink   —— .bin 文件回放（离线调试，不开游戏）
//     LocalSink    —— 直接给 CostBuffer（单元测试用）
//   IMapShowcase   —— CostBuffer -> 灰度像素（见 MapShowcase.h）
//
// 关键性能修复：
//   MapRenderer 原来每收一个包就全扫一遍 height 行判完成（O(n²)）。
//   这里用"已收满行数"计数器，Complete() 是 O(1)。
// ============================================================
#include <cstdint>
#include <string>

#include "../Common/MapFrame.h"
#include "../Common/MapGeometry.h"

namespace mapsink {

class IMapSink {
public:
    virtual ~IMapSink() = default;

    // 开始一张新图。返回 false 表示元信息不可接受（尺寸非法等）
    virtual bool Begin(const mapdata::MapFrame::Meta& meta, std::string& err) = 0;

    // 喂一行数据（可能是一行的一部分）
    virtual bool Feed(const mapdata::MapFrame::RowSlice& slice) = 0;

    // 喂一整帧（便捷入口，内部按行拆给 Feed）
    virtual bool FeedFrame(const mapdata::MapFrame& frame);

    // 是否已收齐全部行。必须是 O(1)。
    virtual bool Complete() const = 0;

    // 收到多少行 / 总共多少行（进度显示用）
    virtual uint32_t RowsDone()   const = 0;
    virtual uint32_t RowsTotal()  const = 0;

    // 已还原的代价表（Complete() 之前也可读，未收到的地方为 0）
    virtual const mapdata::CostBuffer& Result() const = 0;

    // 来源名（日志用）
    virtual const char* Name() const = 0;
};

// ------------------------------------------------------------
// PipeSink —— 命名管道来源（生产路径）
//
// 用"已收满行数"计数器替代全表扫描：
//   每行第一次达到 rowStride 时 ++m_rowsDone，Complete() 只比较一个数。
//   原实现每包扫 1656 行 × 1656 包 = 274 万次比较，现在 O(1)。
// ------------------------------------------------------------
class PipeSink final : public IMapSink {
public:
    PipeSink() = default;

    const char* Name() const override { return "pipe"; }

    bool Begin(const mapdata::MapFrame::Meta& meta, std::string& err) override;
    bool Feed(const mapdata::MapFrame::RowSlice& slice) override;
    bool Complete() const override { return m_complete; }

    uint32_t RowsDone()  const override { return m_rowsDone; }
    uint32_t RowsTotal() const override { return m_meta.height; }

    const mapdata::CostBuffer& Result() const override { return m_cost; }

private:
    mapdata::MapFrame::Meta m_meta;
    mapdata::CostBuffer     m_cost;
    std::vector<uint32_t>   m_rowRecv;     // 每行已收到的最大结束偏移
    uint32_t                m_rowsDone = 0;
    bool                    m_complete = false;
};

// ------------------------------------------------------------
// ReplaySink —— 从 .bin 文件回放
//
// 支持的两种格式（自动识别）：
//   a) nibble 打包流   —— 长度 == RowStride * WorldH
//      你手里的 Plug\recievedata.bin 就是这种
//   b) 逐点字节流      —— 长度 == WorldW * WorldH
//      你手里的 Plug\out_recievedata.bin 是这种
//
// 之所以支持两种：ProcessMapData.py 干的就是把 a 拆成 b。
// 有了这个，改渲染/改着色都不用开游戏。
// ------------------------------------------------------------
class ReplaySink final : public IMapSink {
public:
    // 按给定尺寸从文件加载一整帧
    //   path     .bin 路径
    //   gridW/H  网格尺寸（通常从文件名或已知地图得知）
    //   layer    仅用于日志
    bool LoadFile(const std::string& path,
                  uint32_t gridW, uint32_t gridH,
                  uint8_t layer, std::string& err);

    const char* Name() const override { return "replay"; }

    bool Begin(const mapdata::MapFrame::Meta& meta, std::string& err) override;
    bool Feed(const mapdata::MapFrame::RowSlice& slice) override;
    bool Complete() const override { return m_complete; }

    uint32_t RowsDone()  const override { return m_rowsDone; }
    uint32_t RowsTotal() const override { return m_meta.height; }

    const mapdata::CostBuffer& Result() const override { return m_cost; }

    // 识别出来的格式（诊断用）
    const char* DetectedFormat() const { return m_format; }

private:
    mapdata::MapFrame::Meta m_meta;
    mapdata::CostBuffer     m_cost;
    uint32_t                m_rowsDone = 0;
    bool                    m_complete = false;
    const char*             m_format = "unknown";
};

} // namespace mapsink
