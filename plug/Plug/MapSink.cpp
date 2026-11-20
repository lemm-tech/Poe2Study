#include "MapSink.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "../Common/Logger.h"

namespace mapsink {

// ============================================================
// IMapSink 公共实现
// ============================================================
bool IMapSink::FeedFrame(const mapdata::MapFrame& frame)
{
    std::string err;
    if (!Begin(frame.GetMeta(), err)) {
        LOGW("[Sink] Begin 失败: %s", err.c_str());
        return false;
    }

    const uint32_t H = frame.GetMeta().height;
    const uint32_t stride = frame.GetMeta().rowStride;

    for (uint32_t y = 0; y < H; ++y) {
        const mapdata::MapFrame::RowSlice s = frame.SliceRow(y, 0, stride);
        if (!s.data || s.len == 0) continue;
        if (!Feed(s)) return false;
    }
    return true;
}

// ============================================================
// PipeSink
// ============================================================
bool PipeSink::Begin(const mapdata::MapFrame::Meta& meta, std::string& err)
{
    err.clear();

    if (meta.width == 0 || meta.height == 0) {
        err = "MapMeta 尺寸为 0";
        return false;
    }
    if (meta.width > 0x4000 || meta.height > 0x4000) {
        err = "MapMeta 尺寸过大（疑似数据错位）";
        return false;
    }

    // rowStride 必须等于 (width+1)/2，否则说明发送端/接收端版本不一致
    const uint32_t expect = (meta.width + 1) / 2;
    if (meta.rowStride != expect) {
        char buf[128];
        sprintf_s(buf, "rowStride 不一致：收到 %u，期望 %u",
                  meta.rowStride, expect);
        err = buf;
        return false;
    }

    m_meta = meta;
    m_cost.Resize(mapdata::Geometry::FromGrid(
        meta.width / CELL_SIZE, meta.height / CELL_SIZE));

    m_rowRecv.assign(meta.height, 0);
    m_rowsDone = 0;
    m_complete = false;
    return true;
}

bool PipeSink::Feed(const mapdata::MapFrame::RowSlice& slice)
{
    if (m_complete) return true;
    if (!slice.data || slice.len == 0) return true;
    if (slice.y >= m_meta.height) return false;
    if (m_cost.Empty()) return false;

    // 解包这一片（唯一实现，走 MapFrame::DecodeRow）
    mapdata::MapFrame tmp;
    tmp.m_meta = m_meta;
    tmp.DecodeRow(slice.y, slice.off, slice.data, slice.len, m_cost);

    // ★ O(1) 完成判定：
    //   只有"这一行从没收满变成收满"时才 ++，不扫全表。
    const uint32_t endOff = slice.off + slice.len;
    const uint32_t prev = m_rowRecv[slice.y];
    if (endOff > prev) {
        m_rowRecv[slice.y] = endOff;
        if (prev < m_meta.rowStride && endOff >= m_meta.rowStride) {
            ++m_rowsDone;
            if (m_rowsDone >= m_meta.height) {
                m_complete = true;
            }
        }
    }
    return true;
}

// ============================================================
// ReplaySink
// ============================================================
bool ReplaySink::Begin(const mapdata::MapFrame::Meta& meta, std::string& err)
{
    err = "ReplaySink 不支持 Begin（用 LoadFile）";
    (void)meta;
    return false;
}

bool ReplaySink::Feed(const mapdata::MapFrame::RowSlice&)
{
    return false;
}

bool ReplaySink::LoadFile(const std::string& path,
                          uint32_t gridW, uint32_t gridH,
                          uint8_t layer, std::string& err)
{
    err.clear();

    const mapdata::Geometry geo = mapdata::Geometry::FromGrid(gridW, gridH);
    if (!geo.Plausible()) {
        err = "网格尺寸非法";
        return false;
    }

    // 读整个文件
    std::vector<uint8_t> data;
    {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) {
            err = "打不开文件: " + path;
            return false;
        }
        fseek(fp, 0, SEEK_END);
        const long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (len <= 0) {
            fclose(fp);
            err = "文件为空";
            return false;
        }
        data.resize((size_t)len);
        const size_t got = fread(data.data(), 1, data.size(), fp);
        fclose(fp);
        if (got != data.size()) {
            err = "读取不完整";
            return false;
        }
    }

    const uint64_t packedBytes = geo.PackedBytes();
    const uint64_t pixelCount  = geo.PixelCount();

    // 元信息在这儿一次定好：三种格式共用同一份几何，
    // 只有 rowStride 会因格式不同而不同。
    m_meta.width     = geo.WorldW();
    m_meta.height    = geo.WorldH();
    m_meta.rowStride = geo.RowStride();
    m_meta.layer     = layer;

    // 紧凑 nibble 流（每点半个字节、无行对齐）的总字节数
    const uint64_t compactNibble = (pixelCount + 1) / 2;

    // 诊断：把判定用到的所有尺寸打出来。回放对不上网格时，
    // 这行日志直接给出"差多少"，比反复试参数快得多。
    LOGI("[Replay] 尺寸: file=%zu  pixelCount=%llu  packedBytes=%llu  "
         "compactNibble=%llu  world=%ux%u  rowStride=%u",
         data.size(),
         (unsigned long long)pixelCount,
         (unsigned long long)packedBytes,
         (unsigned long long)compactNibble,
         geo.WorldW(), geo.WorldH(), geo.RowStride());

    // ============================================================
    // 判定顺序（★ 顺序本身就是语义，别随手调）
    //
    // 先从"信息量最大、约束最强"的格式往下判，最后才轮到能容纳一切的
    // 宽松格式：
    //
    //   A. nibble-packed    长度 == RowStride * WorldH      （按行跨度切行）
    //   B. compact-nibble   长度 == ceil(WorldW*WorldH/2)   （无行对齐，按点序）
    //   C. byte-per-point   长度 == WorldW*WorldH           （每点一个字节）
    //
    // ★ A 必须排在 B 前面：WorldW 为偶数时 RowStride*WorldH 恰好等于
    //   WorldW*WorldH/2，也就是和 B 的判据撞值。先判 A，带行跨度的流
    //   才会走"按行切"的正确解码路径。
    //
    // ★ 这里原来把"C"写在最前面，条件又是
    //   data.size()==compactNibble || data.size()==pixelCount 的超集，
    //   后面那支独立的格式 B 条件同为 data.size()==pixelCount ——
    //   于是 B 永远不可达（死代码，编译器不报警）。
    //   现在拆成 A / B / C 三支互不重叠的判据，B 恢复可达。
    // ============================================================

    // ---------- 格式 A：带行跨度的 nibble 打包流 ----------
    if (data.size() == packedBytes) {
        m_format = "nibble-packed";

        m_cost.Resize(geo);
        for (uint32_t y = 0; y < m_meta.height; ++y) {
            const uint8_t* row = data.data() + (size_t)y * m_meta.rowStride;
            mapdata::Cost* dst = m_cost.Raw().data() + (size_t)y * m_meta.width;

            for (uint32_t x = 0; x < m_meta.width; ++x) {
                const uint8_t b = row[x >> 1];
                const uint8_t nib = (x & 1) ? (uint8_t)(b >> 4)
                                            : (uint8_t)(b & 0x0F);
                dst[x] = NibbleToCost(nib);
            }
        }
        m_rowsDone = m_meta.height;
        m_complete = true;

        LOGI("[Replay] 格式=nibble-packed  %s  %u x %u  %zu 字节",
             path.c_str(), m_meta.width, m_meta.height, data.size());
        return true;
    }

    // ---------- 格式 C1：紧凑 nibble 流（无行对齐）----------
    //
    // 为什么需要这条：一部分历史 .bin 是"按点序连续排的 nibble"，
    // 总长 = ceil(WorldW*WorldH/2)，行首没有对齐，所以不能按
    // rowStride 切行，只能按全局点序 idx = y*W + x 取。
    if (data.size() == compactNibble) {
        m_format = "compact-nibble";

        // 行跨度按世界宽重新算，供上层显示/校验用
        m_meta.rowStride = geo.RowStride();

        m_cost.Resize(geo);
        const uint32_t W = m_meta.width;
        const uint32_t H = m_meta.height;

        for (uint32_t y = 0; y < H; ++y) {
            mapdata::Cost* dst = m_cost.Raw().data() + (size_t)y * W;
            const size_t base = (size_t)y * W;   // 连续 nibble 流：按点序排
            for (uint32_t x = 0; x < W; ++x) {
                const size_t idx = base + x;
                const uint8_t b = data[idx >> 1];
                const uint8_t nib = (idx & 1) ? (uint8_t)(b >> 4)
                                              : (uint8_t)(b & 0x0F);
                dst[x] = NibbleToCost(nib);
            }
        }

        m_rowsDone = m_meta.height;
        m_complete = true;

        LOGI("[Replay] 格式=%s  %s  %u x %u  %zu 字节",
             m_format, path.c_str(), m_meta.width, m_meta.height,
             data.size());
        return true;
    }

    // ---------- 格式 C2：逐点字节流 ----------
    if (data.size() == pixelCount) {
        m_format = "byte-per-point";

        // 实测 out_recievedata.bin 是 ProcessMapData.py 拆出来的 nibble，
        // 所以这里每个字节存的仍是 nibble 值，还要经 NibbleToCost 映射
        // （0x05 -> 0xFF），不能直接当代价值用。
        m_cost.Resize(geo);
        for (size_t i = 0; i < data.size(); ++i) {
            m_cost.Raw()[i] = NibbleToCost((uint8_t)(data[i] & 0x0F));
        }
        m_rowsDone = m_meta.height;
        m_complete = true;

        LOGI("[Replay] 格式=byte-per-point  %s  %u x %u  %zu 字节",
             path.c_str(), m_meta.width, m_meta.height, data.size());
        return true;
    }

    // ---------- 尺寸不符：三种格式都不匹配 ----------
    char buf[320];
    sprintf_s(buf,
        "文件大小 %zu 与三种格式都不符：\n"
        "        nibble-packed   需要 %llu 字节 (%u x %u 网格)\n"
        "        compact-nibble  需要 %llu 字节（无行对齐的连续 nibble）\n"
        "        byte-per-point  需要 %llu 字节\n"
        "        如果字节数正好是 2 倍关系，说明网格尺寸猜错了",
        data.size(),
        (unsigned long long)packedBytes, gridW, gridH,
        (unsigned long long)compactNibble,
        (unsigned long long)pixelCount);
    err = buf;
    return false;
}

} // namespace mapsink
