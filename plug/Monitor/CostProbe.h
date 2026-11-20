#pragma once
// ============================================================
// CostProbe.h —— 把 CostCall 这个"危险原语"包成安全的东西
//
// 为什么需要它：CostCall 是从汇编反推出的函数指针，
//   · 签名靠猜（rcx=map, rdx=&{x,y}, r8b=layer, 返回 al）
//   · 调用错一格就会破坏调用者的栈帧（历史上 CostCall_Offset 偏过 0xD）
//   · 参数越界不报错，静默返回垃圾
//   · 逐点调用几百万次会把游戏主线程饿死
//
// 这一个类把上面四件事全兜住：
//   · 参数全部钳制（越界返回 0，绝不让游戏函数见到非法值）
//   · layer 上限从汇编实证（cmp r8b,4）写死为 4，且可通过 MaxUsableLayer 反映真实可用层
//   · 批量 Capture 里每 N 行 Sleep(0) 让出时间片
//
// 外面的代码拿不到裸函数指针，只能走这个接口。
//
// 用法（Monitor 侧，进程内）：
//     std::string err;
//     auto probe = mapscan::MakeCostProbe(err);
//     if (!probe) { LOGI("失败: %s", err.c_str()); }
//     mapdata::CostBuffer buf;
//     probe->CaptureArray(buf, 0);     // 快速：整块读
//     // 或
//     probe->CaptureByCall(buf, 0);    // 慢：逐点调 CostCall
// ============================================================
#include <cstdint>
#include <memory>
#include <string>

#include "../Common/MapFrame.h"
#include "../Common/MapGeometry.h"

namespace mapscan {

// CostCall 的函数指针类型（依据 GameOffsets.h 当前 CostCall_Offset 对应指令反推）
//
//   rcx = 地图对象
//   rdx = 指向 { int x; int y; } 的指针
//   r8b = layer（函数内 cmp r8b, 4 检查上限）
//   返回 al（代价值 0~255）
using CostCallFn = uint8_t (*)(uint64_t map, uint64_t posPtr, uint64_t layer);

// 探针接口：单点 / 批量两种采集方式，参数一律被 clamp
class ICostProbe {
public:
    virtual ~ICostProbe() = default;

    // ---- 元信息 ----
    virtual const mapdata::Geometry& Geo() const = 0;

    // Resolve the live chain again; cached objects and dimensions must still match.
    virtual bool IsCurrent(std::string& err) const = 0;

    // 汇编里 `cmp r8b, 4` → 理论上 layer 0~4；
    // 但进程内实测 layer 4 的 vector 为空，所以"可用层"是 0~3。
    virtual uint8_t MaxLayer() const = 0;
    virtual uint8_t UsableLayerCount() const = 0;

    // ---- 单点：越界/异常一律返回 0，绝不崩 ----
    virtual mapdata::Cost Cost(int x, int y, uint8_t layer) const = 0;

    // ---- 批量：整块读代价数组（生产路径唯一入口）----
    // 直接读 map+0xD0+layer*0x18 的 vector，不碰游戏线程。
    // 实测：与逐点 CostCall 结果 8000 抽样点 100% 一致，单次采集 0 ms。
    virtual bool CaptureArray(mapdata::CostBuffer& out, uint8_t layer) const = 0;

    // ---- 诊断 ----
    virtual uint64_t MapAddress()  const = 0;
    virtual uint64_t CostCallAddr() const = 0;

    // ★ 逐点调 CostCall 不在这里。
    //   它是"验证手段"而不是"生产路径"，见 CostProbeDiag::CrossCheck()。
    //   理由：整块读看起来成功、其实读错（偏移挪了但长度没变）时，
    //   只有逐点调能发现——但这种事不该发生在每次正常采集里。
};

// 工厂：成功返回探针；失败返回 nullptr 并在 err 里写人话。
// 绝不会返回"半初始化"的对象。
std::unique_ptr<ICostProbe> MakeCostProbe(std::string& err);

// ============================================================
// 诊断工具：只在"想验证"的时候用，生产路径永远不碰
// ============================================================
namespace diag {

// 逐点调用 CostCall 采集整张图（慢，会占用游戏线程几秒）。
// 仅用于验证/对拍，不要放进正常的 MT_MapReq 处理流程。
bool CaptureByCall(const ICostProbe& probe,
                   mapdata::CostBuffer& out, uint8_t layer);

// 抽样对拍：随机取 samples 个点，比较"整块读"与"逐点调"的一致性。
// 返回一致率 0.0 ~ 1.0（1.0 = 完全一致）。
// 一致率 < 0.95 通常意味着偏移已随游戏更新变化。
double CrossCheck(const ICostProbe& probe, uint8_t layer, int samples);

} // namespace diag

} // namespace mapscan
