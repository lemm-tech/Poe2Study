#include "ActionWalker.h"
#include "Log.h"

#include <windows.h>
#include <cstring>
#include <string>

#include "../Common/GameOffsets.h"
#include "../Common/GameBuild.h"

namespace navwalk {

namespace {

// ------------------------------------------------------------
// 进程内安全读取（只读，绝不写）
// 与 CostProbe.cpp 的 SafeMem 同一套思路：地址校验 + SEH
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

// 动作系统链里允许尝试的槽位数。
// 实测（2026-11-06 更新后）0x2D0 后面是 5 个元素的指针数组：
//   slot 0~2 = 别的类型/空，slot 3 与 slot 4 = 同一族动作控制器，
//   只有 slot 3 能走（见 GameOffsets.h 的实测记录）。
// 所以先试写死的 kActionSystemSlotIndex，再退化扫其余槽位兜底。
constexpr uint32_t kMaxActionSlots = 8;

} // namespace

const char* WalkResultName(WalkResult r)
{
    switch (r) {
    case WalkResult::Success:             return "Ok";
    case WalkResult::NoActionSystem:      return "NoActionSystem";
    case WalkResult::BadVtable:           return "BadVtable";
    case WalkResult::BadEntry:            return "BadEntry";
    case WalkResult::OutOfBounds:         return "OutOfBounds";
    case WalkResult::Crashed:             return "Crashed";
    case WalkResult::RateLimited:         return "RateLimited";
    default:                              return "Unknown";
    }
}

const char* AblationName(Ablation a)
{
    switch (a) {
    case Ablation::Both:     return "both";
    case Ablation::RdxOnly:  return "rdxOnly";
    case Ablation::R8Only:   return "r8Only";
    case Ablation::PackedR8: return "packedR8";
    default:                 return "?";
    }
}

ActionWalker::~ActionWalker()
{
    if (m_stub) {
        VirtualFree(m_stub, 0, MEM_RELEASE);
        m_stub = nullptr;
    }
}

bool ActionWalker::Initialize(uint64_t moduleBase, std::string& err)
{
    err.clear();

    if (!IsPlausibleAddress(moduleBase)) {
        err = "模块基址不合法";
        return false;
    }
    if (!gamebuild::VerifyLocal(moduleBase, err)) return false;
    m_moduleBase = moduleBase;

    if (!m_stub) {
        // 调用桩放在可执行内存里。
        // 用 PAGE_EXECUTE_READWRITE 是因为要先写后执行；写完立刻改成 RX。
        void* mem = VirtualAlloc(nullptr, sizeof(kActionWalkStub),
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!mem) {
            err = "VirtualAlloc 失败（分配调用桩可执行内存）";
            return false;
        }
        memcpy(mem, kActionWalkStub, sizeof(kActionWalkStub));

        DWORD oldProtect = 0;
        // 写完降权到 RX：减少内存扫描的显眼程度，也更安全
        if (!VirtualProtect(mem, sizeof(kActionWalkStub),
                            PAGE_EXECUTE_READ, &oldProtect)) {
            LOGI("[Walker] VirtualProtect 失败（不影响调用，继续）");
        }

        FlushInstructionCache(GetCurrentProcess(), mem, sizeof(kActionWalkStub));
        m_stub = static_cast<uint8_t*>(mem);
    }

    LOGI("[Walker] 调用桩就绪 @ 0x%llX (%zu 字节)  消融=%s",
         (unsigned long long)(uintptr_t)m_stub, sizeof(kActionWalkStub),
         AblationName(m_ablation));
    return true;
}

uint64_t ActionWalker::ResolveActionSystem(uint32_t slot) const
{
    if (!m_moduleBase) {
        return 0;
    }

    // 第一级：exe + kActionSystemBaseOffset
    const uint64_t level0 = SafeReadU64(m_moduleBase + kActionSystemBaseOffset);
    if (!IsPlausibleAddress(level0)) {
        return 0;
    }

    // 第二级：+ kActionSystemOuterOff + slot*8 + kActionSystemInnerOff
    const uint64_t outerSlot = level0 + kActionSystemOuterOff
                             + (uint64_t)slot * 8
                             + kActionSystemInnerOff;
    const uint64_t level1 = SafeReadU64(outerSlot);
    if (!IsPlausibleAddress(level1)) {
        return 0;
    }

    // 第三级：+ kActionSystemObjOff
    const uint64_t obj = SafeReadU64(level1 + kActionSystemObjOff);
    return IsPlausibleAddress(obj) ? obj : 0;
}

bool ActionWalker::ResolveWalkEntry(uint64_t actionSys, uint64_t& entry) const
{
    entry = 0;
    if (!IsPlausibleAddress(actionSys)) {
        return false;
    }

    const uint64_t vtbl = SafeReadU64(actionSys);
    if (!IsPlausibleAddress(vtbl)) {
        return false;
    }

    const uint64_t fn = SafeReadU64(vtbl + kActionWalkVtblOff);
    if (!IsPlausibleAddress(fn)) {
        return false;
    }

    entry = fn;
    return true;
}

WalkResult ActionWalker::RefreshActionSystem(uint64_t* outObj)
{
    m_actionSys = 0;
    m_walkEntry = 0;

    // 顺序：先写死的槽位（实测 3），再退化扫其它槽位
    uint32_t order[kMaxActionSlots + 1] = { 0 };
    uint32_t nOrder = 0;
    order[nOrder++] = (uint32_t)kActionSystemSlotIndex;
    for (uint32_t s = 0; s < kMaxActionSlots; ++s) {
        if (s != (uint32_t)kActionSystemSlotIndex) {
            order[nOrder++] = s;
        }
    }

    for (uint32_t i = 0; i < nOrder; ++i) {
        const uint32_t slot = order[i];
        const uint64_t obj = ResolveActionSystem(slot);
        if (!obj) {
            continue;
        }

        uint64_t entry = 0;
        if (!ResolveWalkEntry(obj, entry)) {
            LOGI("[Walker] slot=%u 对象 0x%llX 的 vtable 槽无效，跳过",
                 slot, (unsigned long long)obj);
            continue;
        }

        const uint64_t entryRva = (entry > m_moduleBase) ? (entry - m_moduleBase) : 0;
        if (entryRva == kActionWalkEntryRva) {
            m_actionSys = obj;
            m_walkEntry = entry;
            if (outObj) {
                *outObj = obj;
            }
            LOGI("[Walker] slot=%u 动作系统=0x%llX vtable入口=RVA0x%llX（与实测一致）",
                 slot, (unsigned long long)obj, (unsigned long long)entryRva);
            return WalkResult::Success;
        }

        LOGI("[Walker] slot=%u 对象 0x%llX 入口 RVA0x%llX 与实测 0x%llX 不符，跳过",
             slot, (unsigned long long)obj, (unsigned long long)entryRva,
             (unsigned long long)kActionWalkEntryRva);
    }

    m_last = WalkResult::NoActionSystem;
    LOGI("[Walker] 解链失败：所有 slot 都没拿到有效动作系统对象");
    return m_last;
}

bool ActionWalker::Throttle()
{
    const uint64_t now = GetTickCount64();

    // ---- 熔断 1：总次数硬上限 ----
    if (m_maxTotal != 0 && m_calls >= m_maxTotal) {
        LOGI("[Walker] 已达总调用上限 %u 次，拒绝继续调用", m_maxTotal);
        return false;
    }

    // ---- 熔断 2：每秒调用上限（滑动 1 秒窗口）----
    if (m_secondWindowStart == 0 || (now - m_secondWindowStart) >= 1000) {
        m_secondWindowStart = now;
        m_callsThisSecond = 0;
    }
    if (m_callsThisSecond >= m_maxPerSecond) {
        return false;   // 这一秒的额度用完了，让调用方下个 tick 再来
    }

    // ---- 节流：保证两次调用之间有最小间隔 ----
    if (m_minIntervalMs != 0 && m_lastCallTick != 0) {
        const uint64_t elapsed = now - m_lastCallTick;
        if (elapsed < m_minIntervalMs) {
            const uint64_t wait = (uint64_t)m_minIntervalMs - elapsed;
            // 需要等的超过一秒就说明额度本来就不够，直接拒绝而不是长睡
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

WalkResult ActionWalker::WalkTo(int32_t x, int32_t y)
{
    if (!m_stub) {
        m_last = WalkResult::NoActionSystem;
        return m_last;
    }

    // 坐标钳制：越界的目标绝不交给游戏
    if (m_bounds.Valid() && !m_bounds.Contains(x, y)) {
        LOGI("[Walker] 目标 (%d,%d) 越界（界 %d x %d），拒绝",
             x, y, m_bounds.maxX, m_bounds.maxY);
        m_last = WalkResult::OutOfBounds;
        return m_last;
    }

    // 动作系统对象可能已随切图失效 —— 每次调用前校验
    if (!IsPlausibleAddress(m_actionSys)) {
        if (RefreshActionSystem() != WalkResult::Success) {
            m_last = WalkResult::NoActionSystem;
            return m_last;
        }
    }

    uint64_t entry = 0;
    if (!ResolveWalkEntry(m_actionSys, entry) || entry != m_walkEntry) {
        // 入口变了（游戏更新 vtable 或对象被换）——重解一次
        if (RefreshActionSystem() != WalkResult::Success) {
            m_last = WalkResult::BadEntry;
            return m_last;
        }
    }

    // 熔断/节流：被拒就返回 RateLimited，调用方下个 tick 再试（不 Sleep 长等）
    if (!Throttle()) {
        m_last = WalkResult::RateLimited;
        return m_last;
    }

    // ---- 坐标参数（三组消融实测确定的语义）----
    //   rdx = 0x400（常数！不是指针）
    //   r8  = (y << 32) | x（高位 y、低位 x）
    //   r9  = 0
    // 详见 GameOffsets.h 里 kActionWalkArgRdx 上方的实测记录。
    const uint64_t packed = PackWalkPos(x, y);

    uint64_t rdx = kActionWalkArgRdx;
    uint64_t r8  = packed;
    switch (m_ablation) {
    case Ablation::Both:      break;                       // 生产路径
    case Ablation::RdxOnly:   r8  = 0; break;              // 对照：不给 r8
    case Ablation::R8Only:    rdx = 0; break;              // 对照：不给 rdx
    case Ablation::PackedR8:  r8  = packed;                // 对照：rdx 故意给指针
                              rdx = (uint64_t)&packed;
                              break;
    }

    const ActionWalkFn fn = reinterpret_cast<ActionWalkFn>(m_stub);

    // 首次调用前把调用参数记进日志（诊断"调了但没走"的关键证据）
    if (m_calls == 0) {
        LOGI("[Walker] 首次调用参数：actionSys=0x%llX rdx=0x%llX r8=0x%016llX "
             "pos=%d,%d 消融=%s r9=0x%llX stub=0x%llX entry=0x%llX",
             (unsigned long long)m_actionSys,
             (unsigned long long)rdx, (unsigned long long)r8,
             x, y, AblationName(m_ablation),
             (unsigned long long)kActionWalkArgR9,
             (unsigned long long)(uintptr_t)m_stub,
             (unsigned long long)m_walkEntry);
    }
    else if ((m_calls % 20) == 0) {
        // 每 20 次打一条，防止日志爆掉，又能看清调用节奏
        LOGI("[Walker] call#%llu 目标(%d,%d) packed=0x%016llX 每秒 %u 次",
             (unsigned long long)(m_calls + 1), x, y,
             (unsigned long long)packed, m_callsThisSecond);
    }

    bool crashed = false;
    __try {
        fn(m_actionSys, rdx, r8, kActionWalkArgR9);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        crashed = true;
    }

    ++m_calls;

    if (crashed) {
        LOGI("[Walker] call#%llu 目标(%d,%d) rdx=0x%llX r8=0x%llX CRASHED",
             (unsigned long long)m_calls, x, y,
             (unsigned long long)rdx, (unsigned long long)r8);
        // 异常后动作系统对象大概率已不可信，下次调用会重新解链
        m_actionSys = 0;
        m_walkEntry = 0;
        m_last = WalkResult::Crashed;
        return m_last;
    }

    m_last = WalkResult::Success;
    return m_last;
}

// ============================================================
// 诊断：任意对象 + 任意槽位 + 任意参数形态，调一次
// ============================================================
// ============================================================
// 进程内安全读 qword：地址不合法或读崩都返回 0
// ============================================================
uint64_t ActionWalker::SafeRead(uint64_t addr)
{
    if (!IsPlausibleAddress(addr)) {
        return 0;
    }
    return SafeReadU64(addr);   // 内部已有 SEH 兜底
}

bool ActionWalker::LooksLikeFuncStart(uint64_t addr) const
{
    if (!m_moduleBase || !IsPlausibleAddress(addr)) {
        return false;
    }
    // 必须在模块映像内（0x4CA0000 是 size of image，留点余量）
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
    // 前 8 字节里至少有一半是 CC，就算"像起点"
    int cc = 0;
    for (int i = 0; i < 8; ++i) {
        if (pre[i] == 0xCC) ++cc;
    }
    return cc >= 4;
}

WalkResult ActionWalker::ProbeSlot(uint64_t obj, uint32_t slotOff,
                                   int32_t x, int32_t y, uint8_t argMode)
{
    m_last = WalkResult::NoActionSystem;

    if (!IsPlausibleAddress(obj)) {
        return m_last;
    }

    // obj 首字段必须是可读的 vtable 指针
    uint64_t vtbl = 0;
    __try {
        vtbl = *(const uint64_t*)obj;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        vtbl = 0;
    }
    if (!IsPlausibleAddress(vtbl)) {
        m_last = WalkResult::BadVtable;
        return m_last;
    }

    uint64_t entry = 0;
    __try {
        entry = *(const uint64_t*)(vtbl + slotOff);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        entry = 0;
    }
    if (!LooksLikeFuncStart(entry)) {
        m_last = WalkResult::BadEntry;
        return m_last;
    }

    // 现拼一个调用桩：kActionWalkStub 的 call [rax+disp32] 里 disp32 在偏移 27
    //   0  48 83 EC 38                  sub  rsp, 0x38
    //   4  48 C7 44 24 20 00 00 00 00   mov  qword [rsp+0x20], 0
    //   13 48 C7 44 24 28 00 04 00 00   mov  qword [rsp+0x28], 0x400
    //   22 48 8B 01                     mov  rax, [rcx]
    //   25 FF 90 98 01 00 00            call qword [rax+0x198]   <- disp32 @ 27
    //   31 48 83 C4 38                  add  rsp, 0x38
    //   35 C3                           ret
    uint8_t stub[sizeof(kActionWalkStub)] = { 0 };
    memcpy(stub, kActionWalkStub, sizeof(kActionWalkStub));
    memcpy(stub + 27, &slotOff, sizeof(slotOff));

    void* mem = VirtualAlloc(nullptr, sizeof(stub), MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) {
        m_last = WalkResult::NoActionSystem;
        return m_last;
    }
    memcpy(mem, stub, sizeof(stub));
    DWORD old = 0;
    VirtualProtect(mem, sizeof(stub), PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(stub));

    // 参数形态
    const uint64_t packed = PackWalkPos(x, y);
    WalkPos pos{ x, y };
    uint64_t rdx = kActionWalkArgRdx;
    uint64_t r8  = packed;
    switch (argMode) {
    case 1: rdx = (uint64_t)(uintptr_t)&pos; r8 = (uint64_t)(uintptr_t)&pos; break;
    case 2: rdx = 0;                         r8 = packed; break;
    case 3: rdx = packed;                    r8 = 0; break;
    default: break;   // 0
    }

    bool crashed = false;
    __try {
        ((ActionWalkFn)(void*)mem)(obj, rdx, r8, kActionWalkArgR9);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        crashed = true;
    }

    VirtualFree(mem, 0, MEM_RELEASE);

    m_last = crashed ? WalkResult::Crashed : WalkResult::Success;
    return m_last;
}

} // namespace navwalk
