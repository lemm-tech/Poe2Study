#pragma once
// ============================================================
// ActionWalker.h —— 走路 call 的安全封装（进程内，Monitor 侧专用）
//
// ★ 参数语义（以 GameOffsets.h 上方的三组进程内消融实测为准）：
//
//   走路 call 是游戏动作系统 vtable 里的一个槽（[[动作系统对象] + 0x198]），
//   它是 Microsoft x64 成员函数：rcx 既是 this，又是第一个参数。
//
//     rcx = 动作系统对象
//     rdx = 0x400  （**常数**，不是指针！）
//     r8  = 打包坐标 (y << 32) | x（高位 y、低位 x）
//     r9  = 0
//     [rsp+0x20] = 0, [rsp+0x28] = 0x400
//
//   证据（walkasm_probe.dll，起始 (241,891) → 目标 (361,891)）：
//     用例 A：rdx=0x400, r8=(y<<32)|x  -> 3 秒走 121 像素，精确到达 (361,890) ✅
//     用例 B：rdx=&pos,  r8=&pos       -> 位移 0                            ❌
//     用例 C：rdx=&pos,  r8=打包值      -> 位移 0                            ❌
//
//   ★ 本文件早期版本写的"rdx/r8 都传坐标指针"是被实测推翻的旧结论，
//     实现早就改回常数 + 打包值，注释却一直没跟着改。
//     现在注释与 ActionWalker.cpp / GameOffsets.h 三处一致。
//
//   ★ 最容易踩的坑：手写汇编里的 `mov rdx,400` 是十六进制常数 0x400，
//     不是"把某个地址放进 rdx"。曾经把它读成指针语义，结果是走路 call
//     永远返回成功、日志全绿、角色纹丝不动 —— 不崩溃、不报错、就是不干活。
//
//   4 种参数组合仍保留在 Ablation 里，用于将来游戏更新后重新验证。
//
// 这个类把"危险原语"四件事全兜住（与 CostProbe 同一套思路）：
//   1. 参数钳制：坐标越界直接拒绝，绝不让游戏函数见到非法值
//   2. 入口校验：每次调用前确认 动作系统对象的 vtable 指针 与
//      [vtable+0x198] 仍是可执行地址
//      （切图 / 重登会换动作系统对象，缓存的对象会失效）
//   3. SEH 兜底：调用过程中若崩，吞掉异常并返回失败，不带走游戏
//   4. 节奏控制：两次调用之间强制间隔，避免把游戏主线程压死
//
// 外面的代码拿不到裸函数指针，只能走这个接口。
// ============================================================
#include <cstdint>
#include <string>

#include <windows.h>

#include "../Common/GameOffsets.h"

namespace navwalk {

// 调用结果
// 注意：枚举值不能叫 Ok / Exception —— Windows 头文件里 Exception 是
// SEH 宏，会在这里被展开成语法错误；Ok 也容易被第三方头文件污染。
enum class WalkResult : uint8_t {
    Success = 0,        // 调用完成
    NoActionSystem,     // 动作系统对象为空 / 解链失败
    BadVtable,          // 动作系统对象的 vtable 指针不可读 / 不合法
    BadEntry,           // [vtable+0x198] 不是合法可执行地址
    OutOfBounds,        // 目标坐标越界（被钳制拒绝）
    Crashed,            // 调用过程中抛异常（已 SEH 兜底）
    RateLimited,        // 被限速/熔断拒绝（没真正调用）
};

const char* WalkResultName(WalkResult r);

// 坐标传参方式（消融实验用）
//
// 名字与 ActionWalker.cpp 的实际赋值一一对应（2026 实测后的语义）：
//   生产路径永远是 Both = rdx 给常数 0x400、r8 给打包坐标 (y<<32)|x。
//   其余三种是把参数故意改成别的组合做对照，验证"游戏确实只读 r8"。
enum class Ablation : uint8_t {
    Both   = 0,   // rdx = 0x400, r8 = (y<<32)|x   ★ 生产路径
    RdxOnly,      // rdx = 0x400, r8 = 0           对照：不给 r8
    R8Only,       // rdx = 0,     r8 = (y<<32)|x    对照：不给 rdx
    PackedR8,     // rdx = &packed, r8 = (y<<32)|x  对照：把 rdx 故意塞成指针
};

const char* AblationName(Ablation a);

// 世界坐标范围校验（防把角色推出地图）
struct WorldBounds {
    int32_t maxX = 0;
    int32_t maxY = 0;

    bool Valid() const { return maxX > 0 && maxY > 0; }
    bool Contains(int32_t x, int32_t y) const
    {
        return x >= 0 && y >= 0 && x < maxX && y < maxY;
    }
};

// 走路器
class ActionWalker {
public:
    ActionWalker() = default;
    ~ActionWalker();

    ActionWalker(const ActionWalker&) = delete;
    ActionWalker& operator=(const ActionWalker&) = delete;

    // 绑定模块基址（进程内就是 GetModuleHandle 的返回值）。
    // 会立刻分配可执行内存并拷入调用桩。
    bool Initialize(uint64_t moduleBase, std::string& err);

    // 重新解链动作系统对象（切图 / 重登后必须重做）
    WalkResult RefreshActionSystem(uint64_t* outObj = nullptr);

    // 设置世界坐标上界（由地图几何决定），越界的目标点会被拒绝
    void SetWorldBounds(const WorldBounds& b) { m_bounds = b; }

    // 下发一个移动目标（世界像素坐标）
    WalkResult WalkTo(int32_t x, int32_t y);

    // ------------------------------------------------------------
    // 诊断：对「指定对象 + 任意 vtable 槽位 + 任意参数形态」调一次
    //
    // 为什么不走 m_actionSys / m_walkEntry：游戏更新后 kActionWalkVtblOff
    // 失效（解出来的入口不是走路函数，调用返回成功但角色不动），
    // 只能靠挨个试把正确槽位找回来。这个接口就是给扫描器用的。
    //
    //   obj     目标对象（必须是"首字段是 vtable"的对象）
    //   slotOff 槽位偏移，例如 0x198 / 0x1A0
    //   argMode 0=rdx:0x400 + r8 打包   1=rdx/r8 都传坐标指针
    //           2=rdx:0 + r8 打包       3=rdx 打包 + r8:0
    //
    // 会做两道校验再过手：目标槽位的函数指针必须在模块代码段内，
    // 且函数头前面是 CC 填充（确认是函数起点，不是函数中段）。
    // 调用过程 SEH 兜底，崩了也只返回 Crashed，不把游戏带走。
    // ------------------------------------------------------------
    WalkResult ProbeSlot(uint64_t obj, uint32_t slotOff,
                         int32_t x, int32_t y, uint8_t argMode);

    // 判断某地址是否像"函数起点"（模块代码段内 + 前面是 CC 填充）
    bool LooksLikeFuncStart(uint64_t addr) const;

    // 进程内安全读一个 qword（地址不合法或读崩都返回 0）。
    // 放在这里是为了让上层（dllmain）不用自己写裸指针解引用。
    static uint64_t SafeRead(uint64_t addr);

    // 坐标传参方式（默认 Both；只给消融测试用）
    void SetAblation(Ablation a) { m_ablation = a; }
    Ablation GetAblation() const { return m_ablation; }

    // ------------------------------------------------------------
    // ---- 熔断参数（全工程唯一定义处）----
    //
    // 实测教训：30ms 一次、3 秒打 149 次，游戏崩了（52GB 的 dmp 就是那两次）。
    // 所以除了时间间隔，再加两道硬闸：
    //   1. 每秒调用上限
    //   2. 连续调用总次数上限，超了就拒绝并报错
    //
    // ★ 这几个数字原来分散在 ActionWalker.h 的成员初值、
    //   NavDriver::Initialize()、以及 dllmain.cpp 的 MT_NavCfg 分支三处，
    //   而且取值不一致（15/40000 与 10/20000）。MT_NavCfg 那条分支
    //   会在运行期把 NavDriver 刚设好的值静默覆盖掉 —— 改参数等于白改。
    //   现在只有下面这组常量，谁要改都改这里，别的文件一律引用。
    static constexpr uint32_t kDefaultMinIntervalMs = 60;      // 两次调用最小间隔
    static constexpr uint32_t kDefaultMaxPerSecond   = 15;      // 每秒上限
    static constexpr uint32_t kDefaultMaxTotal       = 40000;   // 整局总次数上限

    // 两次调用之间的最小间隔（毫秒）
    void SetMinIntervalMs(uint32_t ms) { m_minIntervalMs = ms; }
    uint32_t MinIntervalMs() const { return m_minIntervalMs; }

    // 一并设置两道硬闸（perSecond 传 0 按 1 处理，避免把调用彻底焊死）
    void SetRateLimit(uint32_t perSecond, uint32_t totalMax)
    {
        m_maxPerSecond = perSecond ? perSecond : 1;
        m_maxTotal = totalMax;
    }

    // 恢复成上面那组默认值（运行期改完参数想还原时用）
    void SetDefaultRateLimit()
    {
        m_minIntervalMs = kDefaultMinIntervalMs;
        SetRateLimit(kDefaultMaxPerSecond, kDefaultMaxTotal);
    }

    uint32_t MaxPerSecond() const { return m_maxPerSecond; }
    uint32_t MaxTotal() const { return m_maxTotal; }

    // 当前这一秒内已调用多少次（诊断）
    uint32_t CallsThisSecond() const { return m_callsThisSecond; }

    // ---- 状态 ----
    bool       IsReady() const { return m_stub != nullptr; }
    uint64_t   ModuleBase() const { return m_moduleBase; }
    uint64_t   ActionSystem() const { return m_actionSys; }
    uint64_t   WalkEntry() const { return m_walkEntry; }
    uint64_t   CallCount() const { return m_calls; }
    WalkResult LastResult() const { return m_last; }

private:
    // 解链：[[[exe+0x4432108]+0x2E8+slot*8+0x18]+0x3E8]
    uint64_t ResolveActionSystem(uint32_t slot) const;

    // 校验入口：从动作系统对象取 vtable，再从 vtable 取 0x198 槽
    bool ResolveWalkEntry(uint64_t actionSys, uint64_t& entry) const;

    // 节流 + 限速。返回 false = 被熔断拒绝
    bool Throttle();

    uint64_t    m_moduleBase = 0;
    uint8_t*    m_stub = nullptr;        // 可执行内存里的调用桩
    uint64_t    m_actionSys = 0;
    uint64_t    m_walkEntry = 0;
    uint64_t    m_calls = 0;
    uint32_t    m_minIntervalMs = kDefaultMinIntervalMs;
    uint64_t    m_lastCallTick = 0;

    // 每秒限速窗口
    uint32_t    m_maxPerSecond = kDefaultMaxPerSecond;
    uint32_t    m_callsThisSecond = 0;
    uint64_t    m_secondWindowStart = 0;

    // 总次数硬上限
    uint32_t    m_maxTotal = kDefaultMaxTotal;

    Ablation    m_ablation = Ablation::Both;

    WorldBounds m_bounds;
    WalkResult  m_last = WalkResult::Success;
};

} // namespace navwalk
