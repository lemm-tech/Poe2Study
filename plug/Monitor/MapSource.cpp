#include "MapSource.h"
#include "CostProbe.h"
#include "Log.h"

#include <windows.h>
#include <cstdio>
#include <memory>

namespace mapscan {
namespace {

// 自检参数：抽样点数、最低一致率
constexpr int   kSelfCheckSamples = 200;
constexpr double kSelfCheckMinRate = 0.95;

// ------------------------------------------------------------
// 整块读策略
// ------------------------------------------------------------
class ArraySource final : public IMapSource {
public:
    const char* Name() const override { return "array"; }

    bool Prepare(std::string& err) override
    {
        err.clear();

        m_probe = MakeCostProbe(err);
        if (!m_probe) return false;

        // ---- 自检 1：真的读一次，看有没有数据 ----
        mapdata::CostBuffer test;
        if (!m_probe->CaptureArray(test, 0)) {
            err = "整块读 layer 0 失败（数组为空或地址失效）";
            return false;
        }

        const size_t nonzero = test.CountNonZero();
        if (nonzero == 0) {
            // 全 0 可能有两种原因：地形没加载，或者 0xD0 偏移错了读到空数组。
            // 无论哪种，这条策略此刻都不能信。
            err = "整块读结果全为 0（地形未加载或 0xD0 偏移已变）";
            return false;
        }

        // ---- 自检 2：与逐点调对拍 ----
        // 这是发现"长度对但内容错"的唯一手段：
        // 偏移挪了 8 字节、长度恰好相同 → 只能靠对拍暴露。
        const double rate = diag::CrossCheck(*m_probe, 0, kSelfCheckSamples);
        if (rate < kSelfCheckMinRate) {
            char buf[160];
            sprintf_s(buf,
                "整块读与逐点调一致率仅 %.1f%%（<%.0f%%），"
                "0xD0 偏移或 nibble 规则可能已变",
                rate * 100.0, kSelfCheckMinRate * 100.0);
            err = buf;
            return false;
        }

        m_ready = true;

        char buf[192];
        sprintf_s(buf, "array: 网格 %u x %u, 非零点 %zu, 自检一致率 %.1f%%",
                  m_probe->Geo().Grid().w, m_probe->Geo().Grid().h,
                  nonzero, rate * 100.0);
        m_desc = buf;
        LOGI("[Source] %s", m_desc.c_str());
        return true;
    }

    bool Capture(mapdata::CostBuffer& out, uint8_t layer) override
    {
        if (!m_ready || !m_probe) return false;
        return m_probe->CaptureArray(out, layer);
    }

    const mapdata::Geometry& Geo() const override
    {
        static const mapdata::Geometry kEmpty;
        return m_probe ? m_probe->Geo() : kEmpty;
    }

    uint8_t UsableLayerCount() const override
    {
        return m_probe ? m_probe->UsableLayerCount() : 0;
    }

    std::string Describe() const override { return m_desc; }

private:
    std::unique_ptr<ICostProbe> m_probe;
    bool        m_ready = false;
    std::string m_desc;
};

// ------------------------------------------------------------
// 逐点调策略（兜底：ArraySource 自检不通时用）
// ------------------------------------------------------------
class CallSource final : public IMapSource {
public:
    const char* Name() const override { return "call"; }

    bool Prepare(std::string& err) override
    {
        err.clear();

        m_probe = MakeCostProbe(err);
        if (!m_probe) return false;

        // 自检：取中心点 + 四角，只要有任意非 0 就算这条路过关。
        // 逐点调不需要像整块读那样对拍——它本身就是基准。
        const mapdata::Geometry& geo = m_probe->Geo();
        const int cx = (int)geo.WorldW() / 2;
        const int cy = (int)geo.WorldH() / 2;

        const int probes[5][2] = {
            { cx, cy },
            { 1, 1 },
            { (int)geo.WorldW() - 2, 1 },
            { 1, (int)geo.WorldH() - 2 },
            { (int)geo.WorldW() - 2, (int)geo.WorldH() - 2 },
        };

        int nonzero = 0;
        for (const auto& p : probes) {
            if (m_probe->Cost(p[0], p[1], 0) != 0) ++nonzero;
        }

        if (nonzero == 0) {
            err = "逐点调 5 个采样点全为 0（CostCall 地址可能已失效）";
            return false;
        }

        char buf[160];
        sprintf_s(buf, "call: 网格 %u x %u, %d/5 采样点非零",
                  geo.Grid().w, geo.Grid().h, nonzero);
        m_desc = buf;
        LOGI("[Source] %s", m_desc.c_str());
        return true;
    }

    bool Capture(mapdata::CostBuffer& out, uint8_t layer) override
    {
        if (!m_probe) return false;
        // 逐点采集慢，日志里会打出耗时
        return diag::CaptureByCall(*m_probe, out, layer);
    }

    const mapdata::Geometry& Geo() const override
    {
        static const mapdata::Geometry kEmpty;
        return m_probe ? m_probe->Geo() : kEmpty;
    }

    uint8_t UsableLayerCount() const override
    {
        return m_probe ? m_probe->UsableLayerCount() : 0;
    }

    std::string Describe() const override { return m_desc; }

private:
    std::unique_ptr<ICostProbe> m_probe;
    std::string m_desc;
};

// 策略名表
const char* const kSourceNames[] = { "array", "call", nullptr };

} // namespace

std::unique_ptr<IMapSource> MakeArraySource()
{
    return std::make_unique<ArraySource>();
}

std::unique_ptr<IMapSource> MakeCallSource()
{
    return std::make_unique<CallSource>();
}

const char* const* AvailableSourceNames()
{
    return kSourceNames;
}

std::unique_ptr<IMapSource> MakeBestSource(std::string& err)
{
    err.clear();
    std::string lastErr;

    // 优先级：array（0 ms）→ call（慢但基准）
    struct Entry {
        const char*                    name;
        std::unique_ptr<IMapSource>  (*make)();
    };
    const Entry kOrder[] = {
        { "array", &MakeArraySource },
        { "call",  &MakeCallSource  },
    };

    for (const Entry& e : kOrder) {
        std::unique_ptr<IMapSource> src = e.make();

        std::string perr;
        if (src->Prepare(perr)) {
            LOGI("[Source] 采用 '%s'：%s", src->Name(), src->Describe().c_str());
            return src;
        }

        LOGI("[Source] '%s' 不可用: %s", e.name, perr.c_str());
        lastErr = std::string(e.name) + ": " + perr;
    }

    err = lastErr.empty() ? "没有可用策略" : lastErr;
    return nullptr;
}

} // namespace mapscan
