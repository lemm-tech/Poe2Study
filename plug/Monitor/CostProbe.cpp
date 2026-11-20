#include "CostProbe.h"
#include "MonitorConfig.h"
#include "Log.h"

#include <windows.h>
#include <cstdio>
#include <vector>

#include "../Common/GameOffsets.h"
#include "../Common/GameBuild.h"

namespace mapscan {
namespace {

// ------------------------------------------------------------
// 进程内安全读取：所有裸指针解引用都必须经过这里
//
// 原来 MapScanner.cpp 有 8 处 *(UINT64*)slot 直接解引用，
// 一旦地址失效就是访问违规，把游戏带走。
// 这里统一加地址校验 + SEH 兜底。
// ------------------------------------------------------------
class SafeMem {
public:
    // 读 u64，失败（地址非法/访问违例）返回 0
    static uint64_t U64(uint64_t addr)
    {
        if (!IsPlausibleAddress(addr)) return 0;
        __try {
            return *(const uint64_t*)addr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static uint32_t U32(uint64_t addr)
    {
        if (!IsPlausibleAddress(addr)) return 0;
        __try {
            return *(const uint32_t*)addr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
};

// ------------------------------------------------------------
// 解链：全工程唯一实现
//   第一级：*(moduleBase + BaseOffset)
//   中间 8 级：每级 + kMapObjectChain[i] 后解引用
//   末级：只做加法（TailOp::Add 的语义）
// ------------------------------------------------------------
uint64_t ResolveMapObject(uint64_t moduleBase, std::string* err)
{
    auto fail = [err](const char* msg) {
        if (err) *err = msg;
        return 0;
    };

    if (!IsPlausibleAddress(moduleBase)) {
        return fail("模块基址不合法");
    }

    uint64_t addr = SafeMem::U64(moduleBase + BaseOffset);
    if (!IsPlausibleAddress(addr)) {
        return fail("地图链第 0 级解引用失败（BaseOffset 可能已随游戏更新变化）");
    }

    for (int i = 0; i < kMapObjectChainDepth - 1; ++i) {
        const uint64_t slot = addr + kMapObjectChain[i];
        const uint64_t next = SafeMem::U64(slot);
        if (!IsPlausibleAddress(next)) {
            char buf[128];
            sprintf_s(buf, "地图链第 %d 级断开（+0x%llX）",
                      i + 1, (unsigned long long)kMapObjectChain[i]);
            return fail(buf);
        }
        addr = next;
    }

    const uint64_t map = addr + kMapObjectChain[kMapObjectChainDepth - 1];
    if (!IsPlausibleAddress(map)) {
        return fail("地图对象地址不合法（末级加法越界）");
    }
    return map;
}

// ------------------------------------------------------------
// 实现
// ------------------------------------------------------------
class CostCallProbe final : public ICostProbe {
public:
    CostCallProbe(uint64_t moduleBase, uint64_t map, CostCallFn fn, const mapdata::Geometry& geo)
        : m_moduleBase(moduleBase), m_map(map), m_fn(fn), m_geo(geo)
    {
    }

    const mapdata::Geometry& Geo() const override { return m_geo; }

    bool IsCurrent(std::string& err) const override
    {
        err.clear();
        const uint64_t currentMap = ResolveMapObject(m_moduleBase, &err);
        if (!currentMap) return false;
        if (currentMap != m_map) {
            char buf[160];
            sprintf_s(buf, "地图对象已切换：缓存 0x%llX，当前 0x%llX",
                      (unsigned long long)m_map, (unsigned long long)currentMap);
            err = buf;
            return false;
        }
        const uint32_t gw = SafeMem::U32(currentMap + MAP_OFF_GRID_W);
        const uint32_t gh = SafeMem::U32(currentMap + MAP_OFF_GRID_H);
        const uint32_t stride = SafeMem::U32(currentMap + MAP_OFF_ROW_STRIDE);
        if (gw != m_geo.Grid().w || gh != m_geo.Grid().h || stride != m_geo.RowStride()) {
            char buf[192];
            sprintf_s(buf, "地图布局已变化：缓存网格 %ux%u stride=%u，当前 %ux%u stride=%u",
                      m_geo.Grid().w, m_geo.Grid().h, m_geo.RowStride(), gw, gh, stride);
            err = buf;
            return false;
        }
        return true;
    }

    // 游戏函数虽允许 layer==4，但 +0x130 已是行跨度字段；只开放验证过的四层。
    uint8_t MaxLayer() const override { return MAP_LAYER_COUNT - 1; }
    uint8_t UsableLayerCount() const override { return MAP_LAYER_COUNT; }

    uint64_t MapAddress()  const override { return m_map; }
    uint64_t CostCallAddr() const override { return (uint64_t)m_fn; }

    // ---- 单点：所有防御都在这里，一次写对 ----
    uint8_t Cost(int x, int y, uint8_t layer) const override
    {
        if (!m_fn || m_map == 0) return 0;
        if (layer > MaxLayer()) return 0;
        if (!m_geo.Contains(x, y)) return 0;

        struct Pos { int x; int y; };
        Pos pos{ x, y };

        __try {
            return m_fn(m_map, (uint64_t)&pos, (uint64_t)layer);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // CostCall 内部崩了（RVA 错/游戏状态异常）：当作 0，别把游戏带走
            return 0;
        }
    }

    // ---- 整块读（快，生产路径唯一入口）----
    //
    // map 对象里 +0xD0 处是【每层一个 vector，共 4 槽，槽长 0x18】的数组：
    //   第 layer 层 begin = *(map + 0xD0 + layer*0x18)
    //   第 layer 层 end   = *(map + 0xD8 + layer*0x18)
    // 这正是 CostCall 内部做的事（见当前 CostCall_Offset 对应实现的指令序列），我们只是跳过它。
    bool CaptureArray(mapdata::CostBuffer& out, uint8_t layer) const override
    {
        out.Clear();
        if (layer >= MAP_LAYER_COUNT) return false;
        if (!CheckCurrent()) return false;

        const uint64_t base = m_map + MAP_OFF_ARY_BEGIN
                            + (uint64_t)layer * MAP_LAYER_STRIDE;

        const uint64_t begin = SafeMem::U64(base);
        const uint64_t end   = SafeMem::U64(base + (MAP_OFF_ARY_END - MAP_OFF_ARY_BEGIN));
        const uint64_t capacity = SafeMem::U64(base + 2 * (MAP_OFF_ARY_END - MAP_OFF_ARY_BEGIN));

        if (!IsPlausibleAddress(begin) || !IsPlausibleAddress(end) ||
            !IsPlausibleAddress(capacity) || end <= begin || capacity < end) {
            LOGI("[CostProbe] layer %u 的代价数组为空或边界无效", (unsigned)layer);
            return false;
        }

        const uint64_t n = end - begin;
        if (n != m_geo.PackedBytes()) {
            LOGI("[CostProbe] layer %u 数组长度 %llu，期望 %llu，拒绝采集并要求重建",
                 (unsigned)layer, (unsigned long long)n,
                 (unsigned long long)m_geo.PackedBytes());
            return false;
        }

        out.Resize(m_geo);

        const uint8_t* ary = (const uint8_t*)begin;
        const uint32_t W = m_geo.WorldW();
        const uint32_t H = m_geo.WorldH();
        const uint32_t stride = m_geo.RowStride();

        __try {
            for (uint32_t y = 0; y < H; ++y) {
                const uint8_t* row = ary + (size_t)y * stride;
                mapdata::Cost* dst = out.Raw().data() + (size_t)y * W;

                for (uint32_t x = 0; x < W; ++x) {
                    const uint8_t b = row[x >> 1];
                    // 游戏原始数组：x 偶数取低 4 位，奇数取高 4 位。
                    const uint8_t nib = (x & 1) ? (uint8_t)(b >> 4)
                                                : (uint8_t)(b & 0x0F);
                    dst[x] = NibbleToCost(nib);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOGI("[CostProbe] 读取 layer %u 数组时发生访问违规", (unsigned)layer);
            out.Clear();
            return false;
        }
        if (SafeMem::U64(base) != begin ||
            SafeMem::U64(base + (MAP_OFF_ARY_END - MAP_OFF_ARY_BEGIN)) != end ||
            SafeMem::U64(base + 2 * (MAP_OFF_ARY_END - MAP_OFF_ARY_BEGIN)) != capacity ||
            !CheckCurrent()) {
            LOGI("[CostProbe] layer %u 读取期间地图或数组发生变化，丢弃本次数据", (unsigned)layer);
            out.Clear();
            return false;
        }
        return true;
    }

private:
    bool CheckCurrent() const
    {
        std::string err;
        if (IsCurrent(err)) return true;
        LOGI("[CostProbe] %s", err.c_str());
        return false;
    }

    uint64_t              m_moduleBase;
    uint64_t              m_map;
    CostCallFn            m_fn;
    mapdata::Geometry     m_geo;
};

} // namespace

// ------------------------------------------------------------
// 工厂
// ------------------------------------------------------------
std::unique_ptr<ICostProbe> MakeCostProbe(std::string& err)
{
    err.clear();

    // 模块名从配置取（原来硬编码 "PathOfExile.exe"，架构守卫 A9 会拦截）
    mcfg::MonitorConfig cfg;
    mcfg::LoadFromFile(cfg);

    HMODULE hMod = GetModuleHandleA(cfg.gameModuleName.c_str());
    if (!hMod) {
        err = "取不到模块 " + cfg.gameModuleName + " 的句柄（游戏未运行或 [game] moduleName 配置有误）";
        return nullptr;
    }
    const uint64_t moduleBase = (uint64_t)hMod;
    if (!gamebuild::VerifyLocal(moduleBase, err)) return nullptr;

    const uint64_t map = ResolveMapObject(moduleBase, &err);
    if (map == 0) {
        if (err.empty()) err = "地图对象定位失败";
        return nullptr;
    }

    // 网格尺寸
    const uint32_t gw = SafeMem::U32(map + MAP_OFF_GRID_W);
    const uint32_t gh = SafeMem::U32(map + MAP_OFF_GRID_H);

    const mapdata::Geometry geo = mapdata::Geometry::FromGrid(gw, gh);
    if (!geo.Plausible()) {
        char buf[128];
        sprintf_s(buf, "网格尺寸不合理 %u x %u（基址可能已漂移）", gw, gh);
        err = buf;
        return nullptr;
    }

    if (SafeMem::U32(map + MAP_OFF_ROW_STRIDE) != geo.RowStride()) {
        err = "地图行跨度与网格尺寸不一致（切图中或偏移已变化）";
        return nullptr;
    }

    const CostCallFn fn = (CostCallFn)(moduleBase + CostCall_Offset);

    LOGI("[CostProbe] moduleBase=0x%llX map=0x%llX grid=%u x %u world=%u x %u",
         (unsigned long long)moduleBase, (unsigned long long)map,
         gw, gh, geo.WorldW(), geo.WorldH());
    LOGI("[CostProbe] CostCall=0x%llX (RVA 0x%X)",
         (unsigned long long)(uint64_t)fn, (unsigned)CostCall_Offset);

    auto probe = std::make_unique<CostCallProbe>(moduleBase, map, fn, geo);
    if (!probe->IsCurrent(err)) return nullptr;
    return probe;
}

// ============================================================
// 诊断工具：逐点调用 CostCall，只用于验证，不进生产路径
//
// 为什么放在这里而不是 ICostProbe：
//   正常的 MT_MapReq 处理只该走 CaptureArray（0 ms）。
//   逐点调用会占用游戏线程几秒，属于"验证代价"，不该是常规开销。
//   但它是唯一能发现「整块读看起来成功、其实读错」的手段：
//     · 0xD0 偏移挪了 8 字节但长度恰好一样
//     · nibble 的高低半字节规则反过来（图上错位一格）
//   所以保留能力，但挪出生产接口。
// ============================================================
namespace diag {

bool CaptureByCall(const ICostProbe& probe,
                   mapdata::CostBuffer& out, uint8_t layer)
{
    out.Clear();
    const mapdata::Geometry& geo = probe.Geo();
    if (!geo.Plausible()) return false;
    if (layer > probe.MaxLayer()) return false;
    std::string err;
    if (!probe.IsCurrent(err)) {
        LOGI("[diag] 逐点采集取消：%s", err.c_str());
        return false;
    }

    out.Resize(geo);

    const uint32_t W = geo.WorldW();
    const uint32_t H = geo.WorldH();

    const ULONGLONG t0 = GetTickCount64();

    for (uint32_t y = 0; y < H; ++y) {
        if ((y & 0x1F) == 0 && !probe.IsCurrent(err)) {
            LOGI("[diag] 逐点采集期间地图变化：%s", err.c_str());
            out.Clear();
            return false;
        }
        for (uint32_t x = 0; x < W; ++x) {
            out.At(x, y) = probe.Cost((int)x, (int)y, layer);
        }
        // 每 32 行让出一次时间片。
        // 原来一口气 270 多万次调用会把游戏主线程饿死（"卡几秒"）。
        if ((y & 0x1F) == 0) {
            Sleep(0);
        }
    }

    if (!probe.IsCurrent(err)) {
        LOGI("[diag] 逐点采集完成时地图变化：%s", err.c_str());
        out.Clear();
        return false;
    }

    LOGI("[diag] 逐点采集完成：%llu ms（%u 点）",
         (unsigned long long)(GetTickCount64() - t0), W * H);
    return true;
}

double CrossCheck(const ICostProbe& probe, uint8_t layer, int samples)
{
    const mapdata::Geometry& geo = probe.Geo();
    if (!geo.Plausible()) return 0.0;
    if (layer > probe.MaxLayer()) return 0.0;

    // 整块读一次（快）
    mapdata::CostBuffer whole;
    if (!probe.CaptureArray(whole, layer)) {
        LOGI("[diag] CrossCheck: 整块读失败，无法对拍");
        return 0.0;
    }

    const uint32_t W = geo.WorldW();
    const uint32_t H = geo.WorldH();
    if (samples <= 0) samples = 200;

    const ULONGLONG t0 = GetTickCount64();

    int match = 0;
    int mismatch = 0;
    int firstBadX = -1, firstBadY = -1;

    // 质数步进，保证点位均匀散布（避免规律性采样撞上同一类地形）
    for (int i = 0; i < samples; ++i) {
        const uint32_t x = (uint32_t)(((uint64_t)i * 7919u) % W);
        const uint32_t y = (uint32_t)(((uint64_t)i * 104729u) % H);

        const mapdata::Cost viaCall  = probe.Cost((int)x, (int)y, layer);
        const mapdata::Cost viaArray = whole.At(x, y);

        if (viaCall == viaArray) {
            ++match;
        }
        else {
            ++mismatch;
            if (firstBadX < 0) { firstBadX = (int)x; firstBadY = (int)y; }
        }
    }

    const double rate = (match + mismatch)
        ? ((double)match / (double)(match + mismatch))
        : 0.0;

    std::string err;
    if (!probe.IsCurrent(err)) {
        LOGI("[diag] CrossCheck 期间地图变化：%s", err.c_str());
        return 0.0;
    }

    LOGI("[diag] CrossCheck(layer=%u, %d 抽样): 一致 %d / 不一致 %d = %.2f%%  (%llu ms)",
         (unsigned)layer, samples, match, mismatch, rate * 100.0,
         (unsigned long long)(GetTickCount64() - t0));

    if (firstBadX >= 0) {
        LOGI("[diag] 首个不一致点 (%d, %d): Call=0x%02X Array=0x%02X",
             firstBadX, firstBadY,
             probe.Cost(firstBadX, firstBadY, layer),
             whole.At((uint32_t)firstBadX, (uint32_t)firstBadY));
    }

    return rate;
}

} // namespace diag

} // namespace mapscan
