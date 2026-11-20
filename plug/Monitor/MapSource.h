#pragma once
// ============================================================
// MapSource.h —— 取数策略接口
//
// 为什么需要这一层：CostProbe 已经有了"怎么安全地读数据"，
// 但"用哪条路读、读失败了怎么办"还硬编码在 dllmain.cpp 里。
//
// 现实问题：
//   CaptureArray 只校验长度（end-begin == 预期字节数）。
//   如果游戏更新导致 0xD0 偏移挪了但长度恰好一样，
//   它会读到另一个同样大的数组 → 出图成功但内容是垃圾。
//
// IMapSource 用 Prepare() 解决这件事：每个策略在启用前必须
// 自证"我真的能读对"，读不对就被跳过、换下一个。
//
// 两个实现：
//   ArraySource  —— 整块读（0 ms，生产首选）
//   CallSource   —— 逐点调 CostCall（慢但最直接，整块读出问题时的兜底）
//
// 用法：
//     std::string err;
//     auto src = mapscan::MakeBestSource(err);
//     if (!src) { LOGI("全部策略失败: %s", err.c_str()); }
//     src->Capture(buf, 0);
// ============================================================
#include <cstdint>
#include <memory>
#include <string>

#include "../Common/MapFrame.h"
#include "../Common/MapGeometry.h"

namespace mapscan {

class IMapSource {
public:
    virtual ~IMapSource() = default;

    // 策略名（日志/诊断用）
    virtual const char* Name() const = 0;

    // 准备并自检。返回 false = 这条策略在当前环境下不可用，
    // err 里写清楚原因（供日志/MakeBestSource 决定是否换下一条）。
    virtual bool Prepare(std::string& err) = 0;

    // 采集一整张图。Prepare 成功后调用。
    virtual bool Capture(mapdata::CostBuffer& out, uint8_t layer) = 0;

    // 几何（Prepare 成功后有效）
    virtual const mapdata::Geometry& Geo() const = 0;

    // 可用层数（0 ~ N-1）
    virtual uint8_t UsableLayerCount() const = 0;

    // 诊断信息（脚本/日志用）
    virtual std::string Describe() const = 0;
};

// ---- 具体策略 ----

// 整块读：直接读 map+0xD0+layer*0x18 的 vector，不碰游戏线程。
// 实测 0 ms，与逐点 CostCall 8000 抽样点 100% 一致。
std::unique_ptr<IMapSource> MakeArraySource();

// 逐点调 CostCall：慢（几秒，会占用游戏线程），但直接反映游戏真实行为。
// 主要在 ArraySource 自检不通过时兜底。
std::unique_ptr<IMapSource> MakeCallSource();

// 按优先级尝试全部策略，返回第一个 Prepare() 成功的。
// 每次尝试的结果都写日志，失败时 err 里是最后一条失败原因。
std::unique_ptr<IMapSource> MakeBestSource(std::string& err);

// 可选策略名列表（给 config 用）
const char* const* AvailableSourceNames();

} // namespace mapscan
