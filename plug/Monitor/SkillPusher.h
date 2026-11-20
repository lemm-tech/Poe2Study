#pragma once
// ============================================================
// SkillPusher.h —— 技能 call 的安全封装（进程内，Monitor 侧专用）
//
// 干什么：把「让角色放一次技能栏里的某个技能」变成一次函数调用。
//
// 与走路 call 的关系（两条独立通道，共用同一个 this）：
//   ActionWalker -> 走路 call，入口 = [[动作系统]+0x198]，随 vtable 变
//   SkillPusher  -> 技能 call，**固定 RVA 0x18C940** 的具名函数
//   两者 rcx 都是动作系统对象，都往同一个动作队列塞东西，
//   所以"同时推进会互相打断"这条约束依旧成立（由上层调度负责互斥）。
//
// ------------------------------------------------------------
// 调用形态（反汇编实证，见 GameOffsets.h 的"技能 call"一节）
// ------------------------------------------------------------
//     rcx = 动作系统对象
//     rdx = 地图对象
//     r8  = 0x400（左键 / 键盘 / 空格翻滚）  0x401（鼠标右键技能）
//     r9  = shared_ptr<skill>*（16 字节：+0x00 _Ptr、+0x08 _Rep）
//     [rsp+0x20] = 释放坐标指针（{int32 x; int32 y;} 世界像素）
//     [rsp+0x28] = 原反汇编里的 al（含义未定，诊断阶段照抄）
//     [rsp+0x30] = 原反汇编里的 r8d（含义未定，诊断阶段照抄 r8）
//
// ------------------------------------------------------------
// 为什么用"整段机器码"，而不是 C++ 函数指针
// ------------------------------------------------------------
//   Microsoft x64 只有 4 个整型参数寄存器（rcx/rdx/r8/r9）。技能 call
//   要在这 4 个之外再塞 3 个栈参数，而栈参数只能由**调用者**在
//   [rsp+0x20..0x30] 上准备好 —— C++ 函数指针无法表达这件事。
//
//   更麻烦的是：我们需要把 call 目标放进 r10、把参数块地址放进 r11，
//   这两个都是"任意寄存器"的诉求，C++ 层面根本无从指定。
//
//   所以做法是：手写一整段桩，它自己完成
//     · sub rsp, 0x48（shadow + 3 个栈参 + 对齐）
//     · 从 r11 指向的参数块搬 3 个 qword 到 [rsp+0x20..0x30]
//     · call r10
//     · add rsp, 0x48 / ret
//   r10 / r11 由桩的内部指令设置（入口地址和参数块地址以立即数形式
//   打在桩里），所以桩是"每次调用现拼、贴到 RWX 内存里跑一次"。
//
// ------------------------------------------------------------
// 两条腿走路
// ------------------------------------------------------------
//   1. ResolveSkillSharedPtr —— 静态链，只读，**没有任何副作用**
//        [[[[exe+0x45F15D8]+0x578]+0x330]+slot*8]+0x2D8/0x2E0
//      首选路径：只要槽位被游戏引用过，拿到的就是现成的有效 shared_ptr，
//      不需要替游戏做引用计数。
//
//   2. LazyLoadSlot —— 那段 `call [rax+0x3F0]`（诊断/兜底，默认关闭）
//        rcx = [动作系统+0x390] ; rax = [rcx] ; call [rax+0x3F0]
//      推断作用是"懒加载槽位对象 + 引用计数++"。它是**虚调用**，风险远高于
//      静态链（槽位对不上就是把任意函数当成员函数调），所以只在静态链
//      解不出有效 _Ptr 时由诊断代码显式打开，且调前必须过函数起点校验。
//
// ------------------------------------------------------------
// 危险原语的四件套兜底（与 ActionWalker / CostProbe 同一套思路）
// ------------------------------------------------------------
//   1. 参数钳制：坐标越界直接拒，绝不让游戏函数见到非法值
//   2. 入口校验：RVA 0x18C940 必须是模块映像内的函数起点
//   3. SEH 兜底：调用过程崩了吞掉异常，不带走游戏
//   4. 熔断：两次调用最小间隔 + 每秒上限 + 整局总次数上限
//
// 外面的代码拿不到裸函数指针，只能走这个接口。
// ============================================================
#include <cstdint>
#include <string>

#include <windows.h>

#include "../Common/GameOffsets.h"

namespace navskill {

// 调用结果
enum class SkillResult : uint8_t {
    Success = 0,        // 调用完成（返回值见 LastReturnValue）
    NoActionSystem,     // 动作系统对象为空 / 解链失败
    BadEntry,           // RVA 0x18C940 不是合法可执行地址 / 不是函数起点
    NoSkillPtr,         // 静态链解不出有效的 shared_ptr<skill>
    OutOfBounds,        // 释放坐标越界（被钳制拒绝）
    Crashed,            // 调用过程中抛异常（已 SEH 兜底）
    RateLimited,        // 被限速/熔断拒绝（没真正调用）
    PreflightFailed,    // hygiene bit2 打开时的深度校验没过（[[_Ptr]+0x520] 不像函数起点）
    CastBudgetExhausted,// 本次注入的 Cast 额度用完了（重新注入可重置）
};

const char* SkillResultName(SkillResult r);

// 静态链解出来的技能指针（纯数据，供诊断打印）
struct SkillPtrInfo {
    uint64_t containerL0 = 0;   // [exe+0x45F15D8]
    uint64_t containerL1 = 0;   // +0x578
    uint64_t containerL2 = 0;   // +0x330
    uint64_t slotEntry   = 0;   // +slot*8（这是 shared_ptr 的地址）
    uint64_t ptr         = 0;   // +0x2D8  _Ptr
    uint64_t rep         = 0;   // +0x2E0  _Rep
    uint32_t refCount    = 0;   // [_Rep + 0xC]（读不到就是 0）

    // _Ptr 与 _Rep 都像合法地址才算"可用"
    bool Valid() const;
};

// 一次调用的完整参数（诊断可覆盖任意字段）
struct SkillCallArgs {
    uint32_t skillSlot = kSkillSlotRightMouse;   // 技能栏下标（右键 = 2）
    uint64_t r8        = kSkillArgR8RightMouse;  // 0x400 / 0x401
    uint64_t stack28   = kSkillArgStack28;       // 原 al
    uint64_t stack30   = kSkillArgStack30Default;// 原 r8d
    int32_t  posX      = 0;                      // 释放坐标（世界像素）
    int32_t  posY      = 0;

    // ---- 消融开关（诊断用，生产恒为下面这组默认值）----
    // rdx 来源：0 = 地图对象本体（kMapObjectChain 末级 +0x8F8）
    //           1 = 用户实测那条链（末尾 +0x78，不含 +0x8F8）
    uint8_t  rdxMode   = 0;
    // r9 来源：0 = 静态链  1 = 先走懒加载虚调用再读静态链
    uint8_t  r9Mode    = 0;
    // 是否真的调用（true = 只解链 + 打日志，不动游戏）
    bool     dryRun    = true;

    // ---- 释放后自动停止（验证"停止施法"用）----
    //   0  = 不停止
    //   >0 = Cast 成功后睡眠这么多毫秒，再调用一次 0x211F0A0（停止发包 call）
    //
    //   为什么要在同一次 Cast 里做：
    //     · 停止 call 必须紧接着释放（中间不能有别的调用插进来）
    //     · 由调用方拆成两步的话，Plug->Monitor 一来一回就过了几百毫秒，
    //       动作可能已经播完了，测不出"打断"效果
    uint32_t stopAfterMs = 0;

    // ---- 释放坐标模式 ----
    //   0 = 用 posX/posY 原值（外部指定）
    //   1 = 用"角色前方"（省略调用方的坐标，由 SkillPusher 实时算）
    //
    //   为什么默认改成"前方"：
    //     实测把技能释放点设在角色脚下（pos 与玩家位置重合）时，
    //     游戏偶尔会卡死；设在角色正前方一小段距离更接近真实操作。
    uint8_t  posMode = 1;

    // 朝向角（弧度）。用于"前方"模式；调用方不填就用 0（+X 方向）。
    //   SkillPusher 会优先用这个值；为 NaN 时回退到 +X。
    double   headingRad = 0.0;

    // 是否根据"最近两次位置差"自己推朝向（1=是）。
    //   0 时直接用 headingRad。
    uint8_t  autoHeading = 1;

    // 前方距离（世界像素）。太小会贴着脚，太大会点到远处。
    // 60 ≈ 2.6 个格子，和普通技能的作用距离接近。
    int32_t  aheadDistance = 60;

    // ---- 会话卫生开关（修复"第二次必崩"的核心）----
    //   bit0 = 调用后把 [r14+0xE8] 状态位写回调用前的值
    //   bit1 = 控制块引用计数增量还原
    //   bit2 = 调用前深度校验（[[_Ptr]+0x520] 必须是模块内函数起点）
    // 全开 = 0x07，默认 0x03（bit2 会让某些技能更容易被拒，只作诊断）。
    uint8_t  hygiene = 0x03;
};

// 调用返回的观测值
struct SkillCallReport {
    SkillResult result = SkillResult::NoActionSystem;
    int32_t  retValue  = -1;        // 技能 call 的返回值（al / eax）


    uint64_t actionSys = 0;         // rcx
    uint64_t mapObject = 0;         // rdx（实际用的那个）
    uint64_t mapObjectAdd = 0;      // 另一种 rdx 候选（对照打印）
    uint64_t skillPtrAddr = 0;      // r9
    uint64_t skillPtr     = 0;      // [_Ptr]
    uint64_t skillRep     = 0;      // [_Rep]
    uint32_t skillRefCnt  = 0;

    int32_t  posX = 0;
    int32_t  posY = 0;
    uint64_t callCount = 0;         // 累计调用次数

    // 技能 call 前后 [r14+0xE8] 的值（游戏自己写回的状态位）
    // ★ 这是唯一可信的"有没有真正执行"的观测：
    //   ret 返回值不可靠（2/3/大负数都出现过），但状态位只在真正执行时才变。
    uint32_t stateBefore = 0;
    uint32_t stateAfter  = 0;

    // ---- 会话卫生观测（修复"第二次必崩"时新增）----
    // 控制块引用计数调用前后。正常情况 After == Before（增量已还原）。
    uint32_t usesBefore = 0;
    uint32_t usesAfter  = 0;
    uint32_t weakBefore = 0;
    uint32_t weakAfter  = 0;
    bool     stateRestored = false;   // 状态位是否写回成功
    uint8_t  hygieneUsed   = 0;       // 本次实际生效的卫生位
    uint32_t budgetLeft    = 0;       // 本次注入还剩几次 Cast 额度
};

// 技能器
class SkillPusher {
public:
    SkillPusher() = default;
    ~SkillPusher();

    SkillPusher(const SkillPusher&) = delete;
    SkillPusher& operator=(const SkillPusher&) = delete;

    // 绑定模块基址并校验 0x18C940 是不是函数起点
    bool Initialize(uint64_t moduleBase, std::string& err);

    // 世界坐标上界（由地图几何决定），越界的释放坐标会被拒绝
    void SetWorldBounds(int32_t maxX, int32_t maxY)
    {
        m_maxX = maxX;
        m_maxY = maxY;
    }

    // 玩家坐标读取回调（由 dllmain 注入 NavDriver::ReadPlayerAnimPos）。
    //
    //   ★ 必须是"实时读"，不能用缓存坐标：
    //     实测用旧坐标算"前方"会算到角色身后，技能就打到错的地方。
    using PosReader = bool (*)(int32_t& x, int32_t& y);
    void SetPosReader(PosReader fn) { m_posReader = fn; }

    // 重新解链动作系统对象（切图 / 重登后必须重做）
    SkillResult RefreshActionSystem(uint64_t* outObj = nullptr);

    // ---- 停止施法（RVA 0x211F0A0）----
    //   rcx = [[[[exe+BaseOffset]+0x348]+0x48]]，其余参数函数自造。
    //   只读校验不过就不调，返回 false。
    bool StopSkill();

    // 只读诊断：解出停止 call 的 rcx（0 = 解不出来）
    uint64_t ResolveStopThis() const;

    // 只读诊断：读停止 call 会碰的几个字段
    struct StopSnapshot {
        uint8_t  flag181 = 0;    // [rcx+0x181]
        uint64_t obj1A8  = 0;    // [rcx+0x1A8]
        uint64_t obj1A0  = 0;    // [rcx+0x1A0]
        uint8_t  gate669 = 0;    // [[rcx+0x1A0]+0x669]
    };
    StopSnapshot ReadStopSnapshot() const;

    // ---- 只读诊断：静态链解技能指针（无任何副作用）----
    SkillPtrInfo ResolveSkillSharedPtr(uint32_t slot) const;

    // ---- 只读诊断：按两种 rdxMode 各解一次地图对象，供对照 ----
    bool ResolveMapObjects(uint64_t& mapTail8F8, uint64_t& mapTail78) const;

    // ---- 诊断：懒加载槽位（虚调用，风险高，只能诊断时显式调）----
    // 返回 false = 没调成（入口不合法 / SEH 捕获 / 对象取不到）
    bool LazyLoadSlot(uint32_t slot, std::string& why);

    // ---- 正式调用 ----
    // dryRun = true 时只解链 + 填 report，不碰游戏函数。
    SkillResult Cast(const SkillCallArgs& args, SkillCallReport& report);

    // ---- 状态 ----
    bool     IsReady() const { return m_moduleBase != 0; }
    uint64_t ModuleBase() const { return m_moduleBase; }
    uint64_t CallEntry() const { return m_entry; }
    uint64_t ActionSystem() const { return m_actionSys; }
    uint64_t CallCount() const { return m_callCount; }
    SkillResult LastResult() const { return m_last; }

    // ---- 忙闲闸（"放一次就停"的核心）----
    //
    //   置位区间：从 Cast 开始，到停止调用结束。
    //   期间任何 Cast 请求直接返回 RateLimited，不排队、不重试。
    //   这是"点一次只放一次"的硬保证，不依赖调用方自律。
    bool     Busy() const { return m_busy; }

    // ---- 熔断参数（默认值引用 ActionWalker 那组常量，全工程只定义一次）----
    void SetMinIntervalMs(uint32_t ms) { m_minIntervalMs = ms; }
    void SetRateLimit(uint32_t perSecond, uint32_t totalMax)
    {
        m_maxPerSecond = perSecond ? perSecond : 1;
        m_maxTotal = totalMax;
    }
    uint32_t MinIntervalMs() const { return m_minIntervalMs; }
    uint32_t MaxPerSecond() const { return m_maxPerSecond; }
    uint32_t MaxTotal() const { return m_maxTotal; }
    uint32_t CallsThisSecond() const { return m_callsThisSecond; }

    // ---- 会话预算闸（"一次注入只放几次"的硬保证）----
    // 与 Throttle 的频率限制是两件事：Throttle 管"多快"，这里管"总共几次"。
    // 根因修好之后它仍然留着 —— 游戏的技能 call 本身有恢复期，
    // 无论如何都不该在一局里被外部连调几十次。
    void     SetMaxCasts(uint32_t n) { m_maxCasts = n; }
    uint32_t MaxCasts() const { return m_maxCasts; }
    uint64_t CastCount() const { return m_callCount; }
    uint32_t BudgetLeft() const
    {
        if (m_maxCasts == 0) return 0xFFFFFFFFu;      // 0 = 不限制
        return (m_callCount >= m_maxCasts)
            ? 0u : (uint32_t)(m_maxCasts - m_callCount);
    }

    // 进程内安全读（地址不合法或读崩都返回 0）
    static uint64_t SafeRead(uint64_t addr);
    static uint32_t SafeReadU32(uint64_t addr);

    // 判断某地址是否像"函数起点"（模块映像内 + 前面是 CC 填充）
    bool LooksLikeFuncStart(uint64_t addr) const;

    // 读 [r14+0xE8]（0x18C940 尾部写回的状态位）
    //   路径：this -> [this+0x1D0] -> [+0x3F0] = r14 -> [r14+0xE8]
    //   读不到返回 0。
    uint32_t ReadStateBit() const;

    // 把状态位写回指定值。返回 false = 链解不出 / 内存不可写 / SEH。
    //
    // ★ 这是"第二次不再崩"的关键一步：0x18C940 尾部会把本次执行结果写进
    //   这个字段，正常输入路径由游戏在动作播完后清回 0。外部直接调用没有
    //   那条清理链，必须我们自己收回，否则状态机永远停在"施法中"，
    //   第二次进入就踩到半初始化结构。
    bool WriteStateBit(uint32_t v);

private:
    uint64_t ResolveActionSystem() const;
    bool     Throttle();
    bool     BuildArgs(const SkillCallArgs& in, SkillCallArgs& out,
                       SkillCallReport& report);

    // 计算释放坐标（posMode=1 = 角色前方；全程实时读玩家坐标）
    bool ComputeCastPos(const SkillCallArgs& args, int32_t& outX, int32_t& outY);
    // 现拼一段桩并执行：把 3 个栈参搬到位，再 call 目标
    //   this_/rdx/r8 走寄存器；r9 = 指向 skill 的**栈上副本**（不是游戏对象）
    //   target 放进 r10；stackArgs 放进 r11
    bool     Dispatch(uint64_t this_, uint64_t rdx, uint64_t r8,
                      const SharedPtrSkill& skill,
                      uint64_t target, const uint64_t stackArgs[3],
                      int32_t& outRet);

    uint64_t    m_moduleBase = 0;
    uint64_t    m_entry = 0;          // = moduleBase + kSkillCallRva
    uint64_t    m_actionSys = 0;

    uint64_t    m_callCount = 0;      // 累计调用（含崩溃）
    uint32_t    m_minIntervalMs = 3000;   // Initialize 会按 config 覆盖（与 cooldownMs 对齐）
    uint64_t    m_lastCallTick = 0;

    uint32_t    m_maxPerSecond = 15;
    uint32_t    m_callsThisSecond = 0;
    uint64_t    m_secondWindowStart = 0;

    uint32_t    m_maxTotal = 40000;

    // 本次注入允许的真实 Cast 次数（0 = 不限制）。默认取
    // kSkillMaxCastsPerInject（3 次），由 Initialize 落定。
    uint32_t    m_maxCasts = kSkillMaxCastsPerInject;

    int32_t     m_maxX = 0;
    int32_t     m_maxY = 0;

    SkillResult m_last = SkillResult::Success;

    volatile bool m_busy = false;   // 忙闲闸

    PosReader m_posReader = nullptr;   // 实时读玩家坐标

    // ---- 朝向推算（"角色前方"模式用）----
    //   角色没有可靠的朝向字段，用"最近一次位移方向"近似。
    //   两次采样间隔内移动超过 2 像素才更新，避免站着不动时朝向抖动。
    int32_t  m_lastPX = 0;
    int32_t  m_lastPY = 0;
    double   m_headingRad = 0.0;
    bool     m_hasLastPos = false;
};

} // namespace navskill
