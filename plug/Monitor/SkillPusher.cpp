#include "SkillPusher.h"
#include "ActionWalker.h"
#include "Log.h"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>

#include "../Common/GameOffsets.h"
#include "../Common/GameBuild.h"

namespace navskill {

namespace {

// ------------------------------------------------------------
// 进程内安全读取（只读，绝不写）
// ------------------------------------------------------------
uint64_t SafeReadU64(uint64_t addr)
{
    if (!IsPlausibleAddress(addr)) return 0;
    __try {
        return *(const uint64_t*)addr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint32_t SafeReadU32v(uint64_t addr)
{
    if (!IsPlausibleAddress(addr)) return 0;
    __try {
        return *(const uint32_t*)addr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ------------------------------------------------------------
// 控制块引用计数快照 / 增量还原
//
// ★ 为什么必须做这件事（实测数据，不要凭直觉改方向）：
//
//   一次成功 Cast 前后，控制块的实际变化是：
//       _Uses  2 -> 1     ← 掉了 1
//       _Weaks 6 -> 6     ← 没动
//
//   也就是说 0x18C940 **对传进去的 shared_ptr 做了一次 DecRef**，
//   它把 r9 当成"移交所有权"的入参来消费掉一份引用。
//
//   正常输入路径下游戏自己会把那份引用补回来（因为它本来就要持有）；
//   我们从独立线程直接调用，**没有那条补回链**，于是每调一次 _Uses 就
//   永久 -1：
//     第一次 2 -> 1     相安无事
//     第二次 1 -> 0     → 触发 _Destroy() + _Delete_this()，
//                        **技能对象被析构**
//     游戏下一帧访问那个技能槽 → 访问已释放对象 → 卡死 / 闪退
//
//   这正是"注入一次只能放一次技能"的完整机制。
//
// ★ 因此修复方向是"预支 + 双向拉回"，不是"减增量"：
//   1) 调用**前**先 +1。保证调用期间 _Uses 最低也有 2，函数那次 DecRef
//      怎么扣都不会归 0 —— 归 0 是不可逆的，事后补计数已经来不及。
//   2) 调用**后**把 _Uses 拉回调用前的值。涨了减、掉了加，双向。
//
// ★ 布局（实测确认，旧注释是错的）：
//   MSVC 的 _Ref_count_base 是多态类，vfptr 占 +0x00，
//   _Uses 在 +0x08（实测值 2）、_Weaks 在 +0x0C（实测值 6）。
//   旧注释把 +0x0C 当成 _Uses —— 正是这条错误注释把上一版修复引向了
//   "以为是泄漏、去减计数"的反方向。
// ------------------------------------------------------------
struct RefSnapshot {
    uint32_t uses  = 0;
    uint32_t weak  = 0;
    bool     valid = false;
};

RefSnapshot SnapRef(uint64_t rep)
{
    RefSnapshot s;
    if (!IsPlausibleAddress(rep)) {
        return s;
    }
    s.uses  = SafeReadU32v(rep + kRefUsesOff);
    s.weak  = SafeReadU32v(rep + kRefWeakOff);
    s.valid = (s.uses != 0 || s.weak != 0);
    return s;
}

// 调用**前**替 0x18C940 预支一份引用（_Uses +1）。
//
// ★ 必须在调用前做，不能事后补：函数内部那次 DecRef 一旦把 _Uses 扣到 0，
//   _Destroy()/_Delete_this() 会立刻把技能对象析构，是不可逆的。
//   提前 +1 之后调用期间最低也有 2 份，怎么扣都不会归零。
bool PreBorrowRef(uint64_t rep)
{
    if (!IsPlausibleAddress(rep)) {
        return false;
    }
    __try {
        InterlockedIncrement((volatile LONG*)(rep + kRefUsesOff));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 调用**后**把 _Uses 拉回"调用前应有的值"，双向（涨了减、掉了加）。
//
// 目标值 = max(before.uses, kSkillMinRefUses)：
//   · 正常：before.uses == 2，预支后 3，函数扣 1 回到 2 == 目标，不用动。
//   · 历史欠账：before.uses == 1（被上一版代码扣过），目标补到 2，
//     把欠的那一份一起还清 —— 否则下一次调用照样会踩到 0。
//
// 只做小幅调整（±4 以内）。差值过大说明控制块已处于异常状态，
// 硬改会把游戏自己的引用也搞坏，比原 bug 崩得更快；此时只在日志里报，
// balanced = false，由上层据此拒绝后续调用。
void RestoreRefBalanced(uint64_t rep, const RefSnapshot& before,
                        RefSnapshot& out, bool& balanced)
{
    out = before;
    balanced = false;

    if (!before.valid || !IsPlausibleAddress(rep) || before.uses == 0) {
        return;
    }

    const uint32_t target = (before.uses < kSkillMinRefUses)
                                ? kSkillMinRefUses
                                : before.uses;

    __try {
        const uint32_t now = SafeReadU32v(rep + kRefUsesOff);
        out.uses = now;
        out.weak = SafeReadU32v(rep + kRefWeakOff);

        if (now == target) {
            balanced = true;
            return;
        }

        const LONG diff = (LONG)target - (LONG)now;
        if (diff > 4 || diff < -4) {
            return;   // 差太多，不动
        }

        InterlockedExchangeAdd((volatile LONG*)(rep + kRefUsesOff), diff);
        out.uses = SafeReadU32v(rep + kRefUsesOff);
        out.weak = SafeReadU32v(rep + kRefWeakOff);
        balanced = (out.uses == target);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        out.uses = SafeReadU32v(rep + kRefUsesOff);
        out.weak = SafeReadU32v(rep + kRefWeakOff);
        balanced = (out.uses == target);
    }
}

// 进程内安全写一个 u32（用于复位状态位）
bool WriteU32Raw(uint64_t addr, uint32_t v)
{
    if (!IsPlausibleAddress(addr)) {
        return false;
    }
    __try {
        *(volatile uint32_t*)addr = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// ------------------------------------------------------------
// 现拼调用桩
//
// 为什么必须现拼：技能 call 的 3 个栈参数只能由调用者摆在
// [rsp+0x20..0x30]，而 C++ 函数指针无法表达"额外栈参数"，
// 更无法指定"把目标放进 r10 / 参数块放进 r11"。
//
// 桩体（逐字节拼出来，共 5 个可变部分以 immediate 形式打在桩里）：
//
//   48 B8 <imm64>                     mov  rax, imm64        ; 参数块地址
//   49 89 C3                          mov  r11, rax
//   49 B8 <imm64>                     mov  r8,  imm64        ; = r8 参数（见下）
//   ...
//
// ★ 更稳的写法：不改 r8，改成先把 4 个真参数原样保留。
//   由于 C++ 侧已经能把 rcx/rdx/r8/r9 通过普通函数指针传进去，
//   桩只需要负责"r10 = target、r11 = 参数块、摆 3 个栈参、call"。
//
//   但普通函数指针调用桩时，C++ 只能传 4 个寄存器参数，第 5、6 个
//   会落到栈上 —— 那正好会占掉我们想用来放栈参数的位置。
//
//   所以最终的桩形态是：**不打参数**，而是
//     · r10 / r11 用 imm64 立即数在桩内自己设置
//     · 3 个栈参数从 r11（参数块）里搬
//     · 4 个真参数由 C++ 侧用普通函数指针传进来（它就放在 rcx/rdx/r8/r9）
//
//   逐条机器码：
//     48 83 EC 48                    sub  rsp, 0x48        ; shadow + 栈参 + 对齐
//     48 B8 <imm64>                  mov  rax, <block>     ; 参数块地址
//     49 89 C3                       mov  r11, rax
//     48 B8 <imm64>                  mov  rax, <target>    ; 技能 call 入口
//     49 89 C2                       mov  r10, rax
//     49 8B 03                       mov  rax, [r11]       ; sp20
//     48 89 44 24 20                 mov  [rsp+0x20], rax
//     49 8B 43 08                    mov  rax, [r11+8]     ; sp28
//     48 89 44 24 28                 mov  [rsp+0x28], rax
//     49 8B 43 10                    mov  rax, [r11+0x10]  ; sp30
//     48 89 44 24 30                 mov  [rsp+0x30], rax
//     41 FF D2                       call r10
//     48 83 C4 48                    add  rsp, 0x48
//     44 8B D0                       mov  r10d, eax        ; 返回值带回来
//     4C 89 D0                       mov  rax, r10
//     C3                             ret
//
// ★ 返回值怎么带出来：call r10 之后 eax 就是技能 call 的返回。
//   这里先 mov r10d,eax / mov rax,r10，最后 ret 时 rax 里就是返回值。
//   C++ 侧按 int32_t 接即可（只关心 al/eax）。
//
// ★ 为什么用 rax 当临时是安全的：rax 在 Microsoft x64 里是**易失寄存器**，
//   调用方不指望它跨 call 存活；4 个真参数（rcx/rdx/r8/r9）我们没碰。
// ============================================================
constexpr size_t kStubMax = 128;

struct StubImage {
    uint8_t  code[kStubMax] = { 0 };
    size_t   size = 0;
};

inline void Put8(StubImage& s, uint8_t b)      { if (s.size < kStubMax) s.code[s.size++] = b; }
inline void Put32(StubImage& s, uint32_t v)    { for (int i = 0; i < 4; ++i) Put8(s, (uint8_t)(v >> (i * 8))); }
inline void Put64(StubImage& s, uint64_t v)    { for (int i = 0; i < 8; ++i) Put8(s, (uint8_t)(v >> (i * 8))); }

StubImage BuildStub(uint64_t target, uint64_t blockAddr)
{
    StubImage s;

    // sub rsp, 0x48
    Put8(s, 0x48); Put8(s, 0x83); Put8(s, 0xEC); Put8(s, 0x48);

    // mov rax, blockAddr ; mov r11, rax
    Put8(s, 0x48); Put8(s, 0xB8); Put64(s, blockAddr);
    Put8(s, 0x49); Put8(s, 0x89); Put8(s, 0xC3);

    // mov rax, target ; mov r10, rax
    Put8(s, 0x48); Put8(s, 0xB8); Put64(s, target);
    Put8(s, 0x49); Put8(s, 0x89); Put8(s, 0xC2);

    // [rsp+0x20] = [r11]
    Put8(s, 0x49); Put8(s, 0x8B); Put8(s, 0x03);
    Put8(s, 0x48); Put8(s, 0x89); Put8(s, 0x44); Put8(s, 0x24); Put8(s, 0x20);

    // [rsp+0x28] = [r11+8]
    Put8(s, 0x49); Put8(s, 0x8B); Put8(s, 0x43); Put8(s, 0x08);
    Put8(s, 0x48); Put8(s, 0x89); Put8(s, 0x44); Put8(s, 0x24); Put8(s, 0x28);

    // [rsp+0x30] = [r11+0x10]
    Put8(s, 0x49); Put8(s, 0x8B); Put8(s, 0x43); Put8(s, 0x10);
    Put8(s, 0x48); Put8(s, 0x89); Put8(s, 0x44); Put8(s, 0x24); Put8(s, 0x30);

    // call r10
    Put8(s, 0x41); Put8(s, 0xFF); Put8(s, 0xD2);

    // add rsp, 0x48
    Put8(s, 0x48); Put8(s, 0x83); Put8(s, 0xC4); Put8(s, 0x48);

    // mov r10d, eax ; mov rax, r10
    Put8(s, 0x44); Put8(s, 0x8B); Put8(s, 0xD0);
    Put8(s, 0x4C); Put8(s, 0x89); Put8(s, 0xD0);

    // ret
    Put8(s, 0xC3);
    return s;
}

// 简化桩（懒加载虚调用用，无栈参数）
StubImage BuildSimpleStub(uint64_t target)
{
    StubImage s;

    // mov rax, target ; call rax ; ret
    Put8(s, 0x48); Put8(s, 0xB8); Put64(s, target);
    Put8(s, 0xFF); Put8(s, 0xD0);
    Put8(s, 0xC3);
    return s;
}

// 往 RWX 内存里贴一段桩并执行，执行完立刻释放
//   ret 由桩自己用 rax 带回
using StubEntry = int32_t (*)(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9);

int32_t RunStub(const StubImage& img, uint64_t rcx, uint64_t rdx,
                uint64_t r8, uint64_t r9, bool& ok, DWORD* outErr)
{
    ok = false;
    if (outErr) *outErr = 0;
    if (img.size == 0) return 0;

    void* mem = VirtualAlloc(nullptr, img.size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) {
        if (outErr) *outErr = GetLastError();
        return 0;
    }
    memcpy(mem, img.code, img.size);
    FlushInstructionCache(GetCurrentProcess(), mem, img.size);

    DWORD old = 0;
    // 写完立刻降权到 RX：减少内存扫描显眼程度，也更安全
    VirtualProtect(mem, img.size, PAGE_EXECUTE_READ, &old);

    int32_t ret = 0;
    __try {
        ret = ((StubEntry)mem)(rcx, rdx, r8, r9);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    return ret;
}

} // namespace

const char* SkillResultName(SkillResult r)
{
    switch (r) {
    case SkillResult::Success:          return "Ok";
    case SkillResult::NoActionSystem:   return "NoActionSystem";
    case SkillResult::BadEntry:         return "BadEntry";
    case SkillResult::NoSkillPtr:       return "NoSkillPtr";
    case SkillResult::OutOfBounds:      return "OutOfBounds";
    case SkillResult::Crashed:          return "Crashed";
    case SkillResult::RateLimited:      return "RateLimited";
    case SkillResult::PreflightFailed:  return "PreflightFailed";
    case SkillResult::CastBudgetExhausted: return "CastBudgetExhausted";
    default:                            return "Unknown";
    }
}

bool SkillPtrInfo::Valid() const
{
    return IsPlausibleAddress(ptr) && IsPlausibleAddress(rep);
}

SkillPusher::~SkillPusher() = default;

// ============================================================
// 初始化
// ============================================================
bool SkillPusher::Initialize(uint64_t moduleBase, std::string& err)
{
    err.clear();

    if (!IsPlausibleAddress(moduleBase)) {
        err = "模块基址不合法";
        return false;
    }
    if (!gamebuild::VerifyLocal(moduleBase, err)) return false;
    m_moduleBase = moduleBase;
    m_entry = moduleBase + kSkillCallRva;

    if (!IsPlausibleAddress(m_entry)) {
        err = "技能 call 入口地址不合法（RVA 算出越界）";
        return false;
    }

    // ★ 入口校验：必须在模块映像内 + 前面是 CC 填充（确认是函数起点，
    //   不是函数中段）。这条专治"游戏更新后 0x18C940 指到别的函数"。
    if (!LooksLikeFuncStart(m_entry)) {
        char buf[224];
        sprintf_s(buf,
            "入口 0x%llX（RVA 0x%llX）不像函数起点："
            "不在模块映像内，或前面不是 CC 填充",
            (unsigned long long)m_entry, (unsigned long long)kSkillCallRva);
        err = buf;
        return false;
    }

    // 熔断参数与走路 call 共用同一组默认值，避免两处不一致
    // ★ 2026-11-08 放宽：实测连续调用会让游戏卡死（每次调用都往动作队列塞施法）。
    //   验证阶段先压到 1500ms / 每秒 1 次，功能稳定后再回调。
    // ★★★ 2026-11-09 关键修正：最小间隔提到 30000ms ★★★
    //
    //   实测数据（同一游戏会话，全部同一组参数）：
    //     间隔  6 秒 -> ret=-1039727408  失败，游戏卡死
    //     间隔  5 秒 -> ret=-771292144   失败，游戏卡死
    //     间隔 32 秒 -> ret=2            成功
    //
    //   结论：0x18C940 内部有恢复期（10~30 秒量级），间隔太短时
    //   读到的是未恢复的坏状态，返回大数值，继续调就卡死。
    //
    //   这不是"修 bug"，而是尊重游戏本身的冷却机制：
    //   真实玩家也不可能 5 秒内连放同一个技能。
    //
    //   30000 是保守值。要更快可以试 20000，但要重新测稳定性。
    // ★ 下面这组是"config.ini 里没写 [skill] 段"时的兜底值，
    //   正常情况下会被 dllmain 从配置覆盖掉。
    //   5000ms 的依据：引用计数 bug 修掉之后实测 3000ms 间隔连续 4 次
    //   都成功且计数配平（旧版 30000ms 的"恢复期"结论是那个 bug 的假象）。
    m_minIntervalMs = 3000;
    m_maxPerSecond  = 1;
    m_maxTotal      = navwalk::ActionWalker::kDefaultMaxTotal;

    // 会话预算：一次注入最多真实 Cast 这么多次。
    // 根因（状态位 / 引用计数 / 缓存对象）已经修掉，但 0x18C940 本身有
    // 10~30 秒量级的恢复期，一局里被外部连调几十次无论如何都不该发生。
    m_maxCasts = kSkillMaxCastsPerInject;

    LOGI("[Skill] 就绪 入口=0x%llX (RVA 0x%llX)  熔断=%ums/%u每满秒/%u总  预算=%u次",
         (unsigned long long)m_entry, (unsigned long long)kSkillCallRva,
         m_minIntervalMs, m_maxPerSecond, m_maxTotal, m_maxCasts);
    return true;
}

// ============================================================
// 安全读 / 函数起点判定
// ============================================================
uint64_t SkillPusher::SafeRead(uint64_t addr)     { return SafeReadU64(addr); }
uint32_t SkillPusher::SafeReadU32(uint64_t addr)  { return SafeReadU32v(addr); }

// ============================================================
// 读 [r14+0xE8]（技能 call 尾部写回的状态位）
//
// 为什么需要它：
//   0x18C940 的返回值和游戏实际表现**没有对应关系**（实测同一个状态
//   可能返回 2 / 3 / 大负数），所以不能用返回值判断"放没放出去"。
//   但函数尾部 +0B3/+0BA 会做：
//     mov ebp,[r14+0xE8]      ; 旧值
//     mov [r14+0xE8],ebx      ; 写入本次结果
//   也就是说这个字段是游戏自己维护的，真正执行过才会被改。
//
// 路径：this -> [this+0x1D0] = f1D0 -> [f1D0+0x3F0] = r14 -> [r14+0xE8]
// ============================================================
uint32_t SkillPusher::ReadStateBit() const
{
    if (!m_actionSys) return 0;
    const uint64_t f1D0 = SafeRead(m_actionSys + kSkillThisOff1D0);
    if (!IsPlausibleAddress(f1D0)) return 0;
    const uint64_t r14 = SafeRead(f1D0 + kSkillThisOff3F0);
    if (!IsPlausibleAddress(r14)) return 0;
    return SafeReadU32v(r14 + kSkillStateOffE8);
}

// ============================================================
// 写回状态位 —— "第二次不再崩"的第一道修复
//
//   0x18C940 尾部（+0B3/+0BA）会做：
//     mov ebp,[r14+0xE8]      ; 旧值
//     mov [r14+0xE8],ebx      ; 写入本次结果
//   正常输入路径下，游戏在动作播完 / 被取消时会把这里清回去。
//   我们从独立线程直接调用，**没有那条清理链**，于是它永久停在
//   "施法中"（实测值 2/3）。第二次进函数时，那条分支假定状态干净，
//   遍历一个半初始化的动作结构 -> 卡死或访问违规。
//
//   所以调用成功后必须由我们自己把它写回调用前的值，语义上等价于
//   "这次调用没发生过"，让游戏的状态机保持干净。
// ============================================================
bool SkillPusher::WriteStateBit(uint32_t v)
{
    if (!m_actionSys) return false;
    const uint64_t f1D0 = SafeRead(m_actionSys + kSkillThisOff1D0);
    if (!IsPlausibleAddress(f1D0)) return false;
    const uint64_t r14 = SafeRead(f1D0 + kSkillThisOff3F0);
    if (!IsPlausibleAddress(r14)) return false;
    return WriteU32Raw(r14 + kSkillStateOffE8, v);
}

bool SkillPusher::LooksLikeFuncStart(uint64_t addr) const
{
    if (!m_moduleBase || !IsPlausibleAddress(addr)) {
        return false;
    }
    // 必须落在当前 PE SizeOfImage 的半开区间内。
    if (addr < m_moduleBase || addr >= m_moduleBase + kGameImageSize) {
        return false;
    }
    // 函数起点前面是 int3 填充（链接器 /hotpatch 对齐后会填 CC）
    uint8_t pre[8] = { 0 };
    __try {
        memcpy(pre, (const void*)(addr - sizeof(pre)), sizeof(pre));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    int cc = 0;
    for (int i = 0; i < 8; ++i) {
        if (pre[i] == 0xCC) ++cc;
    }
    return cc >= 4;
}

// ============================================================
// 动作系统对象
//
// ★ 与走路 call 的 this **不是同一个**：技能用 [l0+0x2D0+0x18+3*8] 本身，
//   走路还要再 +0x3E8。详见下面 ResolveActionSystem 的说明。
// ============================================================
uint64_t SkillPusher::ResolveActionSystem() const
{
    if (!m_moduleBase) {
        return 0;
    }

    const uint64_t level0 = SafeReadU64(m_moduleBase + kActionSystemBaseOffset);
    if (!IsPlausibleAddress(level0)) {
        return 0;
    }

    const uint64_t outerSlot = level0 + kActionSystemOuterOff
                             + (uint64_t)kActionSystemSlotIndex * 8
                             + kActionSystemInnerOff;
    const uint64_t objMid = SafeReadU64(outerSlot);
    if (!IsPlausibleAddress(objMid)) {
        return 0;
    }

    // ★★★ this = objMid + 0x3E8 再解引用（与走路 call 同一个对象）★★★
    //
    //   2026-11-08 进程内实测（skillrcx2.dll）确认：
    //     0x18C940 的序言是：
    //       mov rax, [rcx+0x1D0]
    //       mov r14, [rax+0x3F0]
    //       mov rbx, [rax+0x3E0]
    //       mov rcx, rbx ; call 0x19448A0
    //     所以 this 必须满足：[this+0x1D0] 可读，且其中 +0x3E0/+0x3F0 可读。
    //
    //   实测对照（slot 3）：
    //     objMid            = 0x4B2A99F0000 -> [1D0] = 0x100000027F  ✗ 垃圾值
    //     *(objMid + 0x3E8) = 0x4B2B29EDB80 -> [1D0] = 0x4B2A99F0000 ✓
    //                                          [3E0] = 0x7FF64D17C170 ✓
    //                                          [3F0] = 0x4B2A89BBB00 ✓
    //   即：objMid 与 +0x3E8 派生对象是一对，0x18C940 要的是**派生对象**。
    //
    //   ★ 曾经两次判断出错：
    //     1) 先用 [+0x390] 做判据 —— 那是**另一个函数**（调用者）的序言，不是这里；
    //     2) 然后误以为要用 objMid 本身 —— 反而把本来正确的取值改坏了。
    //   现在按 0x18C940 自己的序言判定，证据在上面。
    const uint64_t thisObj = SafeReadU64(objMid + kActionSystemObjOff);
    if (!IsPlausibleAddress(thisObj)) {
        LOGI("[Skill] objMid=0x%llX 的 [+0x3E8] 不可读（切图了？）",
             (unsigned long long)objMid);
        return 0;
    }

    // 顺手校验 [this+0x1D0] —— 这正是 0x18C940 第一条真正读的字段。
    // 不可读说明这个对象不是技能 call 要的，解链作废让上层重试。
    const uint64_t f1D0 = SafeReadU64(thisObj + 0x1D0);
    if (!IsPlausibleAddress(f1D0)) {
        LOGI("[Skill] this=0x%llX 的 [+0x1D0] 不可读（=0x%llX），解链作废",
             (unsigned long long)thisObj, (unsigned long long)f1D0);
        return 0;
    }

    return thisObj;
}

SkillResult SkillPusher::RefreshActionSystem(uint64_t* outObj)
{
    m_actionSys = ResolveActionSystem();
    if (outObj) {
        *outObj = m_actionSys;
    }
    if (!m_actionSys) {
        LOGI("[Skill] 解链失败：拿不到动作系统对象");
        m_last = SkillResult::NoActionSystem;
        return m_last;
    }
    LOGI("[Skill] 动作系统=0x%llX", (unsigned long long)m_actionSys);
    return SkillResult::Success;
}

// ============================================================
// 只读诊断：静态链解 shared_ptr<skill>
//
//   [[[[exe+0x45F15D8]+0x578]+0x330]+slot*8]+0x2D8 = _Ptr
//                                              +0x2E0 = _Rep
//
// 全程只读，不碰引用计数，不会让游戏产生状态变化。
// ============================================================
SkillPtrInfo SkillPusher::ResolveSkillSharedPtr(uint32_t slot) const
{
    SkillPtrInfo info;
    if (!m_moduleBase) {
        return info;
    }
    if (slot > 0x40) {   // 技能栏不可能有 64 个以上槽位
        return info;
    }

    info.containerL0 = SafeReadU64(m_moduleBase + kSkillContainerBase);
    if (!IsPlausibleAddress(info.containerL0)) {
        return info;
    }

    info.containerL1 = SafeReadU64(info.containerL0 + kSkillContainerOff0);
    if (!IsPlausibleAddress(info.containerL1)) {
        return info;
    }

    info.containerL2 = SafeReadU64(info.containerL1 + kSkillContainerOff1);
    if (!IsPlausibleAddress(info.containerL2)) {
        return info;
    }

    // ★ 这一级是**加法**：L2 里是一个指针数组，每 8 字节一个槽位对象指针。
    //   （2026-11-08 进程内实测：L2+0x000/+0x008/+0x010/... 全是合法堆地址，
    //     每个指向一个 0x600 大小的槽位对象）
    const uint64_t slotPtrAddr = info.containerL2 + (uint64_t)slot * kSkillSlotStride;
    if (!IsPlausibleAddress(slotPtrAddr)) {
        return SkillPtrInfo{};
    }

    // ★ 关键一跳（原来漏掉）：先把数组元素解引用，才拿到槽位对象。
    //   历史错误：把 slotPtrAddr 直接当成 shared_ptr 的位置去读 +0x2D8，
    //   于是读出来是数组里下一个指针的高位拼接值（如 0x376A8000240E3），
    //   明显不是堆地址 —— 这正是 skillscan 之前 _Ptr 看起来乱七八糟的原因。
    const uint64_t slotObj = SafeReadU64(slotPtrAddr);
    if (!IsPlausibleAddress(slotObj)) {
        return SkillPtrInfo{};
    }

    // ★ r9 要的是 **shared_ptr 的地址**，不是槽位对象地址。
    //   这是最后一处坑：把 slotObj 本身传给 r9，函数内部会把它当 shared_ptr
    //   结构去读 _Ptr/_Rep，读到的就是槽位对象开头的两个字段 —— 崩。
    //   正确值 = slotObj + kSkillSharedPtrOff（0x2D8）。
    info.slotEntry = slotObj + kSkillSharedPtrOff;   // 给 r9 用的地址
    info.ptr = SafeReadU64(info.slotEntry);
    info.rep = SafeReadU64(info.slotEntry + 0x08);
    if (IsPlausibleAddress(info.rep)) {
        // MSVC 的 _Ref_count_base 在 _Rep + 0xC 上放 _Uses（32 位）
        info.refCount = SafeReadU32v(info.rep + 0xC);
    }
    return info;
}

// ============================================================
// 只读诊断：两种 rdx 候选各解一次
//
//   候选 A：kMapObjectChain 末级 +0x8F8（现有地图对象的官方路径）
//   候选 B：用户实测那条链，末尾 +0x78（不含 +0x8F8）
// 两者都打出来，再用真实释放验证哪个对。
// ============================================================
// ============================================================
// 停止施法（RVA 0x211F0A0）
// ============================================================
//
// ★ 这条链是**四层**方括号，用户最初给的少写了最后一层：
//     [l1+0x48]     = 中间对象（0x5B15AC1C060）
//     [[l1+0x48]]   = 真正的 this（0x5B15AE18000，与用户样本一致）
//   少解一层会拿中间对象当 this，函数读 [rcx+0x1A8] 就是垃圾值。
uint64_t SkillPusher::ResolveStopThis() const
{
    if (!m_moduleBase) return 0;

    const uint64_t l0 = SafeReadU64(m_moduleBase + BaseOffset);
    if (!IsPlausibleAddress(l0)) return 0;

    const uint64_t l1 = SafeReadU64(l0 + kSkillStopChainOff1);
    if (!IsPlausibleAddress(l1)) return 0;

    const uint64_t l2 = SafeReadU64(l1 + kSkillStopChainOff2);
    if (!IsPlausibleAddress(l2)) return 0;

    const uint64_t obj = SafeReadU64(l2);
    return IsPlausibleAddress(obj) ? obj : 0;
}

SkillPusher::StopSnapshot SkillPusher::ReadStopSnapshot() const
{
    StopSnapshot s;
    const uint64_t rcx = ResolveStopThis();
    if (!rcx) return s;

    s.flag181 = (uint8_t)SafeReadU32(rcx + kSkillStopFlagOff);
    s.obj1A8  = SafeReadU64(rcx + kSkillStopObj1A8);
    s.obj1A0  = SafeReadU64(rcx + kSkillStopObj1A0);
    if (IsPlausibleAddress(s.obj1A0)) {
        s.gate669 = (uint8_t)SafeReadU32(s.obj1A0 + kSkillStopGate669);
    }
    return s;
}

// 停止 call 只有一个参数：rcx
using StopFn = void (*)(uint64_t rcx);

// 独立函数：__try 不能和需要对象展开的代码同函数（C2712）
static bool CallStopOnce(uint64_t entry, uint64_t rcx)
{
    __try {
        ((StopFn)entry)(rcx);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SkillPusher::StopSkill()
{
    if (!m_moduleBase) {
        LOGI("[Skill] StopSkill: 模块基址为空");
        return false;
    }

    // ---- 只读校验：链 / 入口 / 关键字段 ----
    const uint64_t rcx = ResolveStopThis();
    if (!rcx) {
        LOGI("[Skill] StopSkill: rcx 链解不出来，放弃");
        return false;
    }

    const uint64_t entry = m_moduleBase + kSkillStopRva;
    if (!LooksLikeFuncStart(entry)) {
        LOGI("[Skill] StopSkill: 入口 0x%llX 不像函数起点，放弃",
             (unsigned long long)entry);
        return false;
    }

    const uint64_t obj1A8 = SafeReadU64(rcx + kSkillStopObj1A8);
    if (!IsPlausibleAddress(obj1A8)) {
        LOGI("[Skill] StopSkill: [rcx+0x1A8]=0x%llX 非法，放弃",
             (unsigned long long)obj1A8);
        return false;
    }

    const StopSnapshot before = ReadStopSnapshot();
    LOGI("[Skill] StopSkill 调用前: flag181=0x%02X obj1A8=0x%llX gate669=0x%02X",
         before.flag181, (unsigned long long)before.obj1A8, before.gate669);

    // ---- 单次调用 ----
    const bool ok = CallStopOnce(entry, rcx);

    const StopSnapshot after = ReadStopSnapshot();
    LOGI("[Skill] StopSkill 返回=%s  调用后: flag181=0x%02X gate669=0x%02X",
         ok ? "正常" : "异常", after.flag181, after.gate669);

    if (before.gate669 == after.gate669 && before.flag181 == after.flag181) {
        LOGI("[Skill] StopSkill: 两个字段都没变（只发包、未做本地中断 —— 符合序言推断）");
    }
    else {
        LOGI("[Skill] StopSkill: 字段有变化，说明它做了本地状态修改");
    }

    return ok;
}

bool SkillPusher::ResolveMapObjects(uint64_t& mapTail8F8, uint64_t& mapTail78) const
{
    mapTail8F8 = 0;
    mapTail78 = 0;
    if (!m_moduleBase) {
        return false;
    }

    // ---- 候选 A：kMapObjectChain（末级是加法）----
    {
        uint64_t addr = SafeReadU64(m_moduleBase + BaseOffset);
        bool ok = IsPlausibleAddress(addr);
        if (ok) {
            for (int i = 0; i < kMapObjectChainDepth - 1; ++i) {
                addr = SafeReadU64(addr + kMapObjectChain[i]);
                if (!IsPlausibleAddress(addr)) { ok = false; break; }
            }
        }
        if (ok) {
            const uint64_t tail = addr + kMapObjectChain[kMapObjectChainDepth - 1];
            mapTail8F8 = IsPlausibleAddress(tail) ? tail : 0;
        }
    }

    // ---- 候选 B：用户实测链 0x348/0x48/0x1A8/0x20F0/0x5C0/0x0/0x78 ----
    {
        static const uint64_t kChain[] = {
            0x348, 0x48, 0x1A8, 0x20F0, 0x5C0, 0x0, 0x78
        };
        uint64_t addr = SafeReadU64(m_moduleBase + BaseOffset);
        bool ok = IsPlausibleAddress(addr);
        if (ok) {
            for (int i = 0; i < (int)(sizeof(kChain) / sizeof(kChain[0])); ++i) {
                addr = SafeReadU64(addr + kChain[i]);
                if (!IsPlausibleAddress(addr)) { ok = false; break; }
            }
        }
        if (ok) {
            mapTail78 = addr;
        }
    }

    return mapTail8F8 != 0 || mapTail78 != 0;
}

// ============================================================
// 诊断：懒加载槽位（虚调用，风险高）
//
//   rcx = [动作系统+0x390] ; rax = [rcx] ; call [rax+0x3F0]
//   推断签名：单个 size_t 参数（槽位号）
//
// 目的：让游戏自己把该槽位的技能对象引用一次（引用计数++），
// 这样静态链才有东西可解。默认不调，只在静态链失效时诊断用。
// ============================================================
bool SkillPusher::LazyLoadSlot(uint32_t slot, std::string& why)
{
    why.clear();

    if (!m_actionSys) {
        if (RefreshActionSystem() != SkillResult::Success) {
            why = "动作系统对象为空";
            return false;
        }
    }

    const uint64_t outer = SafeReadU64(m_actionSys + kSkillLazyLoadOuterOff);
    if (!IsPlausibleAddress(outer)) {
        why = "取不到 [动作系统+0x390]";
        return false;
    }

    const uint64_t vtbl = SafeReadU64(outer);
    if (!IsPlausibleAddress(vtbl)) {
        why = "外层对象的 vtable 不可读";
        return false;
    }

    const uint64_t entry = SafeReadU64(vtbl + kSkillLazyLoadVtblOff);
    if (!LooksLikeFuncStart(entry)) {
        why = "vtable 槽 +0x3F0 不是函数起点";
        return false;
    }

    LOGI("[Skill] 懒加载槽位 %u：outer=0x%llX vtbl=0x%llX entry=0x%llX（RVA 0x%llX）",
         slot, (unsigned long long)outer, (unsigned long long)vtbl,
         (unsigned long long)entry,
         (unsigned long long)(entry > m_moduleBase ? entry - m_moduleBase : 0));

    const StubImage img = BuildSimpleStub(entry);
    bool ok = false;
    DWORD err = 0;
    (void)RunStub(img, outer, (uint64_t)slot, 0, 0, ok, &err);

    if (!ok) {
        why = "懒加载调用抛异常（已 SEH 兜底）";
        return false;
    }
    return true;
}

// ============================================================
// 熔断 / 节流（与 ActionWalker 同一套规则）
// ============================================================
bool SkillPusher::Throttle()
{
    const uint64_t now = GetTickCount64();

    if (m_maxTotal != 0 && m_callCount >= m_maxTotal) {
        LOGI("[Skill] 已达总调用上限 %u 次，拒绝继续调用", m_maxTotal);
        return false;
    }

    if (m_secondWindowStart == 0 || (now - m_secondWindowStart) >= 1000) {
        m_secondWindowStart = now;
        m_callsThisSecond = 0;
    }
    if (m_callsThisSecond >= m_maxPerSecond) {
        return false;
    }

    if (m_minIntervalMs != 0 && m_lastCallTick != 0) {
        const uint64_t elapsed = now - m_lastCallTick;
        if (elapsed < m_minIntervalMs) {
            const uint64_t wait = (uint64_t)m_minIntervalMs - elapsed;
            if (wait >= 1000) {
                return false;
            }
            Sleep((DWORD)wait);
        }
    }

    m_lastCallTick = GetTickCount64();
    ++m_callsThisSecond;
    return true;
}

// ============================================================
// 组装参数
// ============================================================
// ============================================================
// 计算释放坐标：角色前方
//
// ★ 为什么不用"角色脚下"：
//   实测把技能释放点设在玩家位置本身，游戏偶尔卡死。
//   设在角色正前方一段距离，更接近真实操作（鼠标点前方地面）。
//
// ★ 朝向从哪来：
//   角色没有可靠的朝向字段，用"最近一次位移方向"近似：
//     · 每次调用都实时读坐标（绝不缓存）
//     · 与上次位置比较，位移 > 2 像素才更新朝向
//     · 站着不动时沿用上次朝向
// ★ 全程实时读坐标：调用方给的 posX/posY 在 posMode=1 时被忽略。
// ============================================================
bool SkillPusher::ComputeCastPos(const SkillCallArgs& args,
                                 int32_t& outX, int32_t& outY)
{
    outX = args.posX;
    outY = args.posY;

    if (args.posMode == 0) {
        return true;   // 外部指定，原样用
    }

    // ---- 1) 实时读玩家坐标 ----
    int32_t px = 0, py = 0;
    if (!m_posReader || !m_posReader(px, py)) {
        LOGI("[Skill] 读不到玩家坐标，无法计算前方");
        return false;
    }

    // ---- 2) 推朝向（用最近位移方向）----
    if (args.autoHeading) {
        if (m_hasLastPos) {
            const double dx = (double)px - (double)m_lastPX;
            const double dy = (double)py - (double)m_lastPY;
            const double len2 = dx * dx + dy * dy;
            if (len2 >= 4.0) {   // 位移 > 2 像素才更新，避免抖动
                m_headingRad = std::atan2(dy, dx);
            }
        }
        m_lastPX = px;
        m_lastPY = py;
        m_hasLastPos = true;
    }
    else {
        m_headingRad = args.headingRad;
    }

    // ---- 3) 前方 = 玩家位置 + 朝向 * 距离 ----
    const double dist = (args.aheadDistance > 0) ? (double)args.aheadDistance : 60.0;
    outX = px + (int32_t)std::lround(std::cos(m_headingRad) * dist);
    outY = py + (int32_t)std::lround(std::sin(m_headingRad) * dist);

    // ---- 4) 越界检查（世界边界外会让游戏拒绝/出错）----
    if (m_maxX > 0 && m_maxY > 0) {
        if (outX < 0) outX = 0;
        if (outY < 0) outY = 0;
        if (outX >= m_maxX) outX = m_maxX - 1;
        if (outY >= m_maxY) outY = m_maxY - 1;
    }

    LOGI("[Skill] 释放坐标: 玩家(%d,%d) 朝向%.1f° 前方%lld -> (%d,%d)",
         px, py, m_headingRad * 180.0 / 3.14159265358979323846,
         (long long)dist, outX, outY);
    return true;
}

bool SkillPusher::BuildArgs(const SkillCallArgs& in, SkillCallArgs& out,
                            SkillCallReport& report)
{
    out = in;

    // ---- 动作系统对象 ----
    //
    // ★ 每次强制重解，**绝不复用上一次的缓存**。
    //
    //   原来这里是 `if (!m_actionSys) RefreshActionSystem();` —— 解出来一次
    //   就长期持有。但 0x18C940 执行完会重建/推进内部动作队列，切图、换装、
    //   技能栏变更也都可能把对象换掉。第二次调用还在往第一次那个对象上写，
    //   就是"第二次必崩"的第三条原因。
    //   重解一次只有 4 次内存读，代价可以忽略。
    m_actionSys = 0;
    RefreshActionSystem();
    report.actionSys = m_actionSys;
    if (!m_actionSys) {
        report.result = SkillResult::NoActionSystem;
        return false;
    }

    // ---- 两种 rdx 候选都解出来，方便对照 ----
    uint64_t mapA = 0, mapB = 0;
    ResolveMapObjects(mapA, mapB);
    report.mapObjectAdd = mapA;                                   // +0x8F8
    report.mapObject    = (in.rdxMode == 1) ? mapB : mapA;         // 实际采用

    if (report.mapObject == 0) {
        LOGI("[Skill] rdxMode=%u 对应的地图对象解不出来（A(+8F8)=0x%llX B(+78)=0x%llX）",
             (unsigned)in.rdxMode, (unsigned long long)mapA,
             (unsigned long long)mapB);
        report.result = SkillResult::NoActionSystem;
        return false;
    }

    // ---- 技能 shared_ptr ----
    if (in.r9Mode == 1) {
        std::string why;
        if (!LazyLoadSlot(in.skillSlot, why)) {
            LOGI("[Skill] 懒加载失败：%s（继续尝试静态链）", why.c_str());
        }
    }

    const SkillPtrInfo info = ResolveSkillSharedPtr(in.skillSlot);
    report.skillPtrAddr = info.slotEntry;
    report.skillPtr     = info.ptr;
    report.skillRep     = info.rep;
    report.skillRefCnt  = info.refCount;

    LOGI("[Skill] 槽位 %u 链: L0=0x%llX L1=0x%llX L2=0x%llX entry=0x%llX "
         "_Ptr=0x%llX _Rep=0x%llX ref=%u",
         in.skillSlot,
         (unsigned long long)info.containerL0,
         (unsigned long long)info.containerL1,
         (unsigned long long)info.containerL2,
         (unsigned long long)info.slotEntry,
         (unsigned long long)info.ptr,
         (unsigned long long)info.rep,
         info.refCount);

    // ★ 不做 [[_Ptr]+0x520] 校验（v15 版行为，已实测可成功调用）。
    //   加过一版"加固校验"，但反而影响了调用行为，已回滚。
    //   若以后要恢复保护，参照 git 历史或 GameOffsets.h 的说明。

    if (!info.Valid()) {
        LOGI("[Skill] 静态链没解出有效的 shared_ptr（槽位可能没被游戏引用过）");
        report.result = SkillResult::NoSkillPtr;
        return false;
    }

    // ---- 深度校验（hygiene bit2，默认关）----
    //
    //   0x18C940 的 +09D 是 `mov r8,[rcx+0x520]`，+0AA 是 `call r8`。
    //   所以 [[_Ptr]+0x520] 必须是可执行函数，否则那个 call 会跳进堆数据
    //   （实测出现过 0x2100650063 这种 UTF-16 字符串，直接执行型访问违规）。
    //   v15 加过一版这检查，实测反而更容易被误拒（有些技能槽位这个字段
    //   是懒加载的，第一次调之前还是数据），所以默认关，只作诊断。
    if ((in.hygiene & 0x04) != 0 && IsPlausibleAddress(info.ptr)) {
        const uint64_t fn520 = SafeRead(info.ptr + kSkillAbortVtblOff);
        if (!LooksLikeFuncStart(fn520)) {
            LOGI("[Skill] preflight 失败：[[_Ptr]+0x520]=0x%llX 不像函数起点"
                 "（技能可能还没被激活，先在游戏里手动放一次）",
                 (unsigned long long)fn520);
            report.result = SkillResult::PreflightFailed;
            return false;
        }
    }

    // ---- 释放坐标（posMode=1 时算"角色前方"，并实时读玩家坐标）----
    {
        int32_t cx = 0, cy = 0;
        if (!ComputeCastPos(in, cx, cy)) {
            report.result = SkillResult::NoActionSystem;
            return false;
        }
        out.posX = cx;
        out.posY = cy;
    }

    report.posX = out.posX;
    report.posY = out.posY;
    return true;
}

// ============================================================
// 正式调用
// ============================================================
SkillResult SkillPusher::Cast(const SkillCallArgs& args, SkillCallReport& report)
{
    report = SkillCallReport{};
    report.result = SkillResult::NoActionSystem;

    // ---- 忙闲闸：已经在放/在停，直接拒绝 ----
    //
    // ★ 这是"放一次就停"的硬保证：不排队、不重试、不叠加。
    //   置位从 Cast 进来开始，到停止调用结束才清。
    if (m_busy) {
        LOGI("[Skill] 忙，拒绝本次释放（上一次还在放/在停）");
        report.result = SkillResult::RateLimited;
        m_last = report.result;
        return m_last;
    }

    if (!IsReady()) {
        LOGI("[Skill] 尚未 Initialize，拒绝调用");
        report.result = SkillResult::BadEntry;
        return report.result;
    }

    // ---- 会话预算闸：本次注入的真实 Cast 次数硬上限 ----
    //
    // ★ 为什么要有这个：0x18C940 内部有 10~30 秒量级的恢复期，一局里被
    //   外部连调几十次无论如何都不该发生。就算根因已经修掉，
    //   这条也留着当兜底 —— 额度用完就明确拒绝，而不是把游戏试到崩。
    //   重新注入 Monitor 会新建 SkillPusher，额度自然重置。
    if (m_maxCasts != 0 && m_callCount >= m_maxCasts) {
        LOGI("[Skill] 本次注入的 Cast 额度已用完（%llu/%u），拒绝调用。"
             "重新注入 Monitor 可重置额度。",
             (unsigned long long)m_callCount, m_maxCasts);
        report.result = SkillResult::CastBudgetExhausted;
        report.budgetLeft = 0;
        m_last = report.result;
        return m_last;
    }

    SkillCallArgs ready;
    if (!BuildArgs(args, ready, report)) {
        m_last = report.result;
        return m_last;
    }

    if (args.dryRun) {
        LOGI("[Skill] dryRun：参数全部就绪，未真正调用 "
             "(slot=%u r8=0x%llX rcx=0x%llX rdx=0x%llX r9=0x%llX pos=%d,%d)",
             args.skillSlot, (unsigned long long)ready.r8,
             (unsigned long long)report.actionSys,
             (unsigned long long)report.mapObject,
             (unsigned long long)report.skillPtrAddr,
             report.posX, report.posY);
        report.result = SkillResult::Success;
        m_last = report.result;
        return m_last;
    }

    // 进入真实调用：置忙
    // ★ 记录调用前状态位（游戏自己写的 [r14+0xE8]）
    report.stateBefore = ReadStateBit();
    report.hygieneUsed = args.hygiene;

    // ★ 记录控制块引用计数（调用前）。实测 0x18C940 每调一次会把
    //   _Uses 扣 1，扣到 0 就析构技能对象 —— 这是"第二次必崩"的真凶。
    const RefSnapshot refBefore = SnapRef(report.skillRep);
    report.usesBefore = refBefore.uses;
    report.weakBefore = refBefore.weak;
    report.usesAfter  = refBefore.uses;
    report.weakAfter  = refBefore.weak;

    m_busy = true;

    if (!Throttle()) {
        m_busy = false;
        report.result = SkillResult::RateLimited;
        m_last = report.result;
        return m_last;
    }

    // ---- 预支一份引用（只在真正要调用时才做）----
    //   Throttle 之后、Dispatch 之前。保证调用期间 _Uses >= 2，
    //   函数内部那次 DecRef 怎么扣都不会归 0。
    bool borrowed = false;
    if ((args.hygiene & 0x02) != 0 && refBefore.valid && refBefore.uses != 0) {
        borrowed = PreBorrowRef(report.skillRep);
        if (borrowed) {
            LOGI("[Skill] 引用计数预支 +1：_Uses %u -> %u（保证调用期间不到 0）",
                 refBefore.uses, refBefore.uses + 1);
        }
        else {
            LOGI("[Skill] 引用计数预支失败（控制块不可写），本次调用有归零风险");
        }
    }

    // ---- 参数 ----
    LOGI("[Skill] 调用 slot=%u r8=0x%llX rcx=0x%llX rdx=0x%llX(候选A=0x%llX) "
         "r9=0x%llX(_Ptr=0x%llX ref=%u) pos=%d,%d sp28=0x%llX sp30=0x%llX entry=0x%llX",
         ready.skillSlot,
         (unsigned long long)ready.r8,
         (unsigned long long)report.actionSys,
         (unsigned long long)report.mapObject,
         (unsigned long long)report.mapObjectAdd,
         (unsigned long long)report.skillPtrAddr,
         (unsigned long long)report.skillPtr,
         report.skillRefCnt,
         ready.posX, ready.posY,
         (unsigned long long)ready.stack28,
         (unsigned long long)ready.stack30,
         (unsigned long long)m_entry);

    struct Pos { int32_t x; int32_t y; };
    Pos pos{ ready.posX, ready.posY };

    uint64_t stackArgs[3] = {
        (uint64_t)(uintptr_t)&pos,   // [rsp+0x20] 释放坐标指针
        ready.stack28,               // [rsp+0x28] 原 al
        ready.stack30                // [rsp+0x30] 原 r8d
    };

    int32_t ret = 0;

    // ★ r9 不再直接用 report.skillPtrAddr（那是**游戏对象内部**的
    //   shared_ptr 地址），而是把这个 16 字节复制一份到 Dispatch 的栈上，
    //   传副本的地址进去。详见 Dispatch 里的说明。
    const SharedPtrSkill skill{ report.skillPtr, report.skillRep };

    if (!Dispatch(report.actionSys, report.mapObject, ready.r8,
                  skill, m_entry, stackArgs, ret)) {
        // Dispatch 内部已 SEH 兜底并打日志
        ++m_callCount;
        report.budgetLeft = BudgetLeft();
        m_actionSys = 0;   // 异常后动作系统对象不可信，下次重新解链
        m_busy = false;

        // ★ 崩溃路径也必须把预支的那份引用还回去。
        //   否则每崩一次就欠一份，下一次调用的起点更低 —— 会越用越容易崩。
        if (borrowed) {
            const RefSnapshot raw = SnapRef(report.skillRep);
            RefSnapshot fin;
            bool bal = false;
            RestoreRefBalanced(report.skillRep, refBefore, fin, bal);
            report.usesAfter = fin.uses;
            report.weakAfter = fin.weak;
            LOGI("[Skill] 崩溃后引用计数还原：_Uses %u -预支-> %u -调用后-> %u -还原-> %u  %s",
                 refBefore.uses, refBefore.uses + 1, raw.uses, fin.uses,
                 bal ? "(已配平)" : "(未配平)");
        }

        report.result = SkillResult::Crashed;
        m_last = report.result;
        return m_last;
    }

    ++m_callCount;
    report.retValue  = ret;
    report.callCount = m_callCount;
    report.budgetLeft = BudgetLeft();

    LOGI("[Skill] 返回 %d（call#%llu，本秒 %u 次，剩余额度 %u）",
         ret, (unsigned long long)m_callCount, m_callsThisSecond, report.budgetLeft);

    // ============================================================
    // 调用后"会话卫生" —— 三条，缺一条都会让第二次崩
    // （这就是"注入一次只能放一次技能"的直接修复）
    // ============================================================

    // ---- 1) 引用计数：拉回调用前的值（双向）----
    //   实测 0x18C940 会 DecRef 一次。预支 +1 之后这次 DecRef 只是把
    //   预支的还回来，但为了兜住"函数扣了不止一次"和"上一版代码留下的
    //   历史欠账"，这里统一再对齐一次到 max(before, 2)。
    if (borrowed) {
        const RefSnapshot raw = SnapRef(report.skillRep);
        RefSnapshot fin;
        bool bal = false;
        RestoreRefBalanced(report.skillRep, refBefore, fin, bal);
        report.usesAfter = fin.uses;
        report.weakAfter = fin.weak;

        LOGI("[Skill] 引用计数：_Uses %u -预支-> %u -调用后-> %u -还原-> %u   "
             "_Weaks %u -> %u   %s",
             refBefore.uses, refBefore.uses + 1, raw.uses, fin.uses,
             refBefore.weak, fin.weak,
             bal ? "(已配平，可安全再调用)"
                 : "(未配平！请重启游戏或换图后再试)");
    }
    else if ((args.hygiene & 0x02) == 0) {
        report.usesAfter = SnapRef(report.skillRep).uses;
        LOGI("[Skill] 引用计数：hygiene bit1 关闭，未预支/未还原"
             "（_Uses 现在 %u，每调一次递减 1，连续调用会归零析构）",
             report.usesAfter);
    }

    // ---- 2) 状态位：写回调用前的值 ----
    //   0x18C940 尾部刚把本次结果写进 [r14+0xE8]。正常输入路径由游戏
    //   在动作播完后清回去，外部调用没有那条链 —— 不收回就永远停在
    //   "施法中"，第二次进函数必崩。
    if ((args.hygiene & 0x01) != 0) {
        const uint32_t dirty = ReadStateBit();
        report.stateRestored = WriteStateBit(report.stateBefore);
        report.stateAfter = ReadStateBit();
        LOGI("[Skill] 状态位: 调用前 0x%X -> 调用后 0x%X -> 写回 0x%X %s（现在 0x%X）",
             report.stateBefore, dirty, report.stateBefore,
             report.stateRestored ? "成功" : "失败", report.stateAfter);
    }
    else {
        report.stateAfter = ReadStateBit();
        LOGI("[Skill] 状态位: 调用前 0x%X -> 调用后 0x%X（hygiene bit0 关闭，未写回）",
             report.stateBefore, report.stateAfter);
    }

    // ★ stopAfterMs 的处理**搬到调用方（dllmain）**，这里只提示。
    //
    //   原因：本函数是在 g_navMutex 保护下被调用的（技能 call 与走路 call
    //   共用同一个动作系统对象）。原来在这里 Sleep(wait) 再 StopSkill()，
    //   等于**持着 g_navMutex 睡 300ms 以上**，把 NavDriver::Tick 一起冻住，
    //   导航状态机停摆、位置坐标断档。现在 Cast 只管释放，
    //   停下来由 dllmain 在释放锁之后再 Sleep + StopSkill。
    if (args.stopAfterMs > 0) {
        LOGI("[Skill] stopAfterMs=%u 交由调用方在释放锁后执行", args.stopAfterMs);
    }

    m_busy = false;

    // ★ 强制下次重新解链：本次调用后对象可能已被游戏换掉，
    //   绝不能让下一次复用这个地址。
    m_actionSys = 0;

    report.result = SkillResult::Success;
    m_last = SkillResult::Success;
    return m_last;
}

// ============================================================
// Dispatch：现拼桩并执行
// ============================================================
bool SkillPusher::Dispatch(uint64_t this_, uint64_t rdx, uint64_t r8,
                           const SharedPtrSkill& skill,
                           uint64_t target, const uint64_t stackArgs[3],
                           int32_t& outRet)
{
    outRet = 0;

    struct Block {
        uint64_t sp20;
        uint64_t sp28;
        uint64_t sp30;
    } block{ stackArgs[0], stackArgs[1], stackArgs[2] };

    // ============================================================
    // ★ r9 = 指向**我们自己栈上的 shared_ptr 副本**，不是游戏对象里的那个
    //
    //   原实现把 slotObj+0x2D8（游戏自己持有的 shared_ptr）地址直接当 r9
    //   传进去。0x18C940 序言里 `mov [rsp+20], r9`，之后把它当
    //   shared_ptr<skill>* 用；只要函数内部对 r9 做任何写操作 ——
    //   重置、移动、赋值、写 _Ptr/_Rep —— 都会**直接改写游戏对象**。
    //   正常输入路径下游戏自己会收拾；我们从独立线程直接调，没有那个收尾，
    //   于是第二次进入时读到的就是被上一次调用改脏的 16 字节。
    //
    //   传副本之后，这类写全部落在栈上，随调用结束一起消失，
    //   游戏对象里的 shared_ptr 保持原样。
    //
    //   ★ 注意：副本里的 _Rep 仍然指向同一个控制块，所以函数内部对该控制块
    //     的引用计数增减依然会生效 —— 那部分由 Cast 里的"预支 + 双向拉回"
    //     （PreBorrowRef / RestoreRefBalanced）补回来。
    // ============================================================
    SharedPtrSkill mine = skill;

    const StubImage img = BuildStub(target, (uint64_t)(uintptr_t)&block);

    bool ok = false;
    DWORD err = 0;
    const int32_t ret = RunStub(img, this_, rdx, r8,
                                (uint64_t)(uintptr_t)&mine, ok, &err);

    if (!ok) {
        LOGI("[Skill] 调用抛异常（已 SEH 兜底）。"
             "target=0x%llX rcx=0x%llX rdx=0x%llX r8=0x%llX r9(副本)=0x%llX "
             "_Ptr=0x%llX _Rep=0x%llX",
             (unsigned long long)target,
             (unsigned long long)this_, (unsigned long long)rdx,
             (unsigned long long)r8, (unsigned long long)(uintptr_t)&mine,
             (unsigned long long)mine.ptr, (unsigned long long)mine.rep);
        return false;
    }

    // 调用后副本若被函数改写过，记下来 —— 这是"保护是否奏效"的直接证据。
    if (mine.ptr != skill.ptr || mine.rep != skill.rep) {
        LOGI("[Skill] 栈上副本被 0x18C940 改写过：_Ptr 0x%llX->0x%llX "
             "_Rep 0x%llX->0x%llX（游戏对象未受影响）",
             (unsigned long long)skill.ptr, (unsigned long long)mine.ptr,
             (unsigned long long)skill.rep, (unsigned long long)mine.rep);
    }

    outRet = ret;
    return true;
}

} // namespace navskill
