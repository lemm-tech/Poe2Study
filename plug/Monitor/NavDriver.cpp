#include "NavDriver.h"
#include "Log.h"

#include <windows.h>
#include <climits>
#include <cmath>
#include <vector>

#include "../Common/GameOffsets.h"

namespace navdrive {
namespace {

// 游戏窗口标题（用于 SendInput 前把游戏切到前台）。
// 由 dllmain 启动时从 config.ini 填入；空 = 退回当前前台窗口。
const char* g_nudgeWindowTitle = nullptr;

// 进程内安全读（只读）
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

uint8_t SafeReadU8(uint64_t addr)
{
    if (!IsPlausibleAddress(addr)) return 0;
    __try {
        return *(const uint8_t*)addr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint32_t SafeReadU32(uint64_t addr)
{
    if (!IsPlausibleAddress(addr)) return 0;
    __try {
        return *(const uint32_t*)addr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

inline int32_t Dist2D(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    const double dx = (double)ax - (double)bx;
    const double dy = (double)ay - (double)by;
    return (int32_t)std::lround(std::sqrt(dx * dx + dy * dy));
}

// 在 [from, to) 窗口里找离 (x,y) 最近的路径点下标。找不到返回 -1。
int NearestIndexInWindow(const std::vector<NavBatchChunk>& pts,
                         int32_t x, int32_t y, int from, int to)
{
    if (pts.empty()) return -1;
    if (from < 0) from = 0;
    if (to > (int)pts.size()) to = (int)pts.size();
    if (from >= to) return -1;

    int best = -1;
    long long bestD = -1;
    for (int i = from; i < to; ++i) {
        const long long dx = (long long)pts[(size_t)i].x - x;
        const long long dy = (long long)pts[(size_t)i].y - y;
        const long long d = dx * dx + dy * dy;
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// 简化版本：窗口自动取 [cur - back, cur + fwd)
int NearestIndexAround(const std::vector<NavBatchChunk>& pts,
                       int32_t x, int32_t y, int cur, int back, int fwd)
{
    const int from = cur - (back < 0 ? 0 : back);
    const int to   = cur + (fwd < 0 ? 0 : fwd);
    return NearestIndexInWindow(pts, x, y, from, to);
}

// ------------------------------------------------------------
// 朝游戏窗口发一次真实鼠标点击（SendInput，硬件级输入）
//
// 为什么下发走路 call 前要点一次（实测结论）：
//   走路 call 只在"最近有真实输入"的窗口内才真正驱动角色。
//   闲置后直接调，游戏把指令吞掉、不报错、角色不动。
//   PostMessage 投递的消息游戏看不见（它读原始输入队列），
//   必须用 SendInput 才能进队列。
//
//   点在哪里不重要（我们只是要产生一次输入事件），
//   所以点在窗口客户区偏下方，避免误触 UI 按钮。
//
// 副作用控制：记录原光标位置，点完立刻恢复。
// ------------------------------------------------------------
// 保留但**不要再调用**（见下面 shouldIssue 分支里的说明）
[[maybe_unused]] void NudgeInputOnce(uint32_t minGapMs, const char* windowTitle)
{
    static uint64_t lastNudge = 0;
    const uint64_t now = GetTickCount64();
    if (lastNudge != 0 && (now - lastNudge) < minGapMs) {
        return;
    }
    lastNudge = now;

    // ★ 先确保游戏在前台：SendInput 打到的是前台窗口，
    //   游戏在后台时所有注入输入都会落到别的窗口上，
    //   表现为"调用成功但角色完全不动"。这是之前排查最久的一个坑。
    HWND hwnd = windowTitle ? FindWindowA(nullptr, windowTitle) : nullptr;
    if (!hwnd) {
        hwnd = GetForegroundWindow();
    }
    if (!hwnd) {
        return;
    }
    if (GetForegroundWindow() != hwnd) {
        const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD myThread = GetCurrentThreadId();
        bool attached = false;
        if (fgThread != 0 && fgThread != myThread) {
            attached = (AttachThreadInput(myThread, fgThread, TRUE) != 0);
        }
        SetForegroundWindow(hwnd);
        if (attached) {
            AttachThreadInput(myThread, fgThread, FALSE);
        }
        Sleep(40);
    }

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        return;
    }

    POINT origin{ 0, 0 };
    if (!ClientToScreen(hwnd, &origin)) {
        return;
    }

    const LONG screenW = GetSystemMetrics(SM_CXSCREEN);
    const LONG screenH = GetSystemMetrics(SM_CYSCREEN);
    if (screenW <= 1 || screenH <= 1) {
        return;
    }

    // 客户区 50% x 58%：偏下方，避开顶部技能栏和底部 UI
    const int cx = w / 2;
    const int cy = (int)((double)h * 0.58);
    const LONG sx = origin.x + cx;
    const LONG sy = origin.y + cy;

    const LONG nx = (LONG)((double)sx * 65535.0 / (double)(screenW - 1));
    const LONG ny = (LONG)((double)sy * 65535.0 / (double)(screenH - 1));

    POINT saved{};
    const bool haveSaved = (GetCursorPos(&saved) != 0);

    INPUT in[3] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[0].mi.dx = nx; in[0].mi.dy = ny;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE;
    in[1].mi.dx = nx; in[1].mi.dy = ny;
    in[2].type = INPUT_MOUSE;
    in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE;
    in[2].mi.dx = nx; in[2].mi.dy = ny;

    SendInput(3, in, sizeof(INPUT));

    if (haveSaved) {
        SetCursorPos(saved.x, saved.y);
    }
}

} // namespace

// ============================================================
// NavConfig <-> 管道结构
// ============================================================
void SetNudgeWindowTitle(const char* title)
{
    g_nudgeWindowTitle = title;
}

void NavConfig::Apply(const NavCfg& c)
{
    // 0 值一律视为"不改"，这样 Plug 想调哪个字段就只填哪个
    if (c.arrive > 0)        arriveTolerance = c.arrive;
    if (c.skipNear > 0)      skipNear = c.skipNear;
    if (c.stuckEps > 0)      stuckEps = c.stuckEps;
    if (c.maxMsWaypoint)     maxMsWaypoint = c.maxMsWaypoint;
    if (c.settleMs)          settleMs = c.settleMs;
    if (c.reissueMs)         reissueMs = c.reissueMs;
    if (c.noProgressMs)      noProgressMs = c.noProgressMs;
    if (c.maxReissue)        maxReissue = c.maxReissue;
    if (c.backOff >= 0)      backOff = c.backOff;
    byIndex = (c.flags & 0x02) != 0;
    (void)c.reserved;
}

NavCfg NavConfig::ToWire() const
{
    NavCfg c{};
    c.arrive        = (int16_t)arriveTolerance;
    c.skipNear      = (int16_t)skipNear;
    c.stuckEps      = (int16_t)stuckEps;
    c.maxMsWaypoint = (uint16_t)maxMsWaypoint;
    c.settleMs      = (uint16_t)settleMs;
    c.reissueMs     = (uint16_t)reissueMs;
    c.noProgressMs  = (uint16_t)noProgressMs;
    c.maxReissue    = (uint8_t)maxReissue;
    c.backOff       = (uint8_t)(backOff < 0 ? 0 : (backOff > 255 ? 255 : backOff));
    c.flags         = byIndex ? 0x02 : 0x00;
    return c;
}

// ============================================================
// 初始化
// ============================================================
bool NavDriver::Initialize(uint64_t moduleBase, const navwalk::WorldBounds& bounds,
                           std::string& err)
{
    err.clear();

    if (!m_walker.Initialize(moduleBase, err)) {
        return false;
    }
    m_walker.SetWorldBounds(bounds);

    // 熔断参数：宁可慢，不许崩。
    // ★ 数字只在 navwalk::ActionWalker 的 kDefault* 常量里定义一次。
    //   原来这里写死 60/15/40000，而 dllmain.cpp 的 MT_NavCfg 分支又
    //   SetMinIntervalMs(60) + SetRateLimit(10, 20000) —— 运行期会把这里
    //   刚设好的值覆盖成另一组，改参数等于白改。现在两边都引用同一组常量。
    m_walker.SetMinIntervalMs(navwalk::ActionWalker::kDefaultMinIntervalMs);
    m_walker.SetRateLimit(navwalk::ActionWalker::kDefaultMaxPerSecond,
                          navwalk::ActionWalker::kDefaultMaxTotal);

    LOGI("[Nav] 初始化完成：世界边界 %d x %d（熔断：%ums间隔 / %u次每秒 / 总%u次）",
         bounds.maxX, bounds.maxY,
         m_walker.MinIntervalMs(),
         m_walker.MaxPerSecond(), m_walker.MaxTotal());
    return true;
}

// ============================================================
// 路径输入
// ============================================================
void NavDriver::BeginNewPath()
{
    m_active = !m_pts.empty();

    // 本点状态全部复位
    m_issued = false;
    m_instructedMoving = false;
    m_reissueCount = 0;
    m_waypointStartTick = 0;
    m_issuedAtTick = 0;
    m_nextIssueNotBefore = 0;
    m_lastDist = -1;
    m_lastProgressTick = GetTickCount64();
}

bool NavDriver::FeedPathChunk(uint32_t seq, uint32_t total, uint32_t baseIdx,
                              int16_t tolerance, bool isLast,
                              const NavBatchChunk* pts, uint32_t count)
{
    if (total == 0) {
        // 空路径 = 走完了
        m_pts.clear();
        m_idx = 0;
        m_active = false;
        m_feeding = false;
        return true;
    }
    if (!pts && count) {
        return false;
    }

    // 新 seq：开一张新表
    if (!m_feeding || seq != m_feedSeq) {
        m_feeding = true;
        m_feedSeq = seq;
        m_feedExpect = total;
        m_feedTol = tolerance ? tolerance : m_cfg.arriveTolerance;
        m_pts.assign((size_t)total, NavBatchChunk{ 0, 0 });
        m_idx = 0;
        m_seq = seq;
        m_tolerance = m_feedTol;
        LOGI("[Nav] 新路径 seq=%u 共 %u 点", seq, total);
    }
    else if (tolerance) {
        m_feedTol = tolerance;
        m_tolerance = tolerance;
    }

    if (baseIdx >= (size_t)m_pts.size()) {
        return false;
    }
    uint32_t n = count;
    if (baseIdx + n > (uint32_t)m_pts.size()) {
        n = (uint32_t)m_pts.size() - baseIdx;
    }
    for (uint32_t i = 0; i < n; ++i) {
        m_pts[(size_t)baseIdx + i] = pts[i];
    }

    if (isLast) {
        m_feeding = false;
        if (n && baseIdx + n < (uint32_t)m_pts.size()) {
            // 本轮实际点比 total 少：裁掉尾巴
            m_pts.resize((size_t)baseIdx + n);
        }
        BeginNewPath();
        LOGI("[Nav] 路径就绪 seq=%u 实际 %zu 点 容差=%d",
             m_seq, m_pts.size(), (int)m_tolerance);
    }
    return true;
}

bool NavDriver::EnqueueGoal(int32_t x, int32_t y, uint16_t tolerance)
{
    if (x < 0 || y < 0) {
        LOGI("[Nav] 目标 (%d,%d) 为负，拒绝", x, y);
        return false;
    }

    m_pts.clear();
    m_pts.push_back(NavBatchChunk{ x, y });
    m_idx = 0;
    ++m_seq;
    m_tolerance = tolerance ? (int32_t)tolerance : m_cfg.arriveTolerance;
    m_feedExpect = 1;
    m_feeding = false;
    BeginNewPath();

    LOGI("[Nav] 单点目标 (%d,%d) 容差=%d", x, y, (int)m_tolerance);
    return true;
}

void NavDriver::Stop()
{
    if (m_active || !m_pts.empty()) {
        LOGI("[Nav] 中止导航（剩 %zu 点）", m_pts.size() - m_idx);
    }
    m_pts.clear();
    m_idx = 0;
    m_active = false;
    m_feeding = false;
    m_issued = false;
    m_instructedMoving = false;
    m_reissueCount = 0;
    m_waypointStartTick = 0;
    m_issuedAtTick = 0;
    m_lastDist = -1;
}

// ============================================================
// 实体 / 组件
// ============================================================
uint64_t NavDriver::FindComponent(uint64_t entity, const char* name)
{
    const uint64_t p0 = SafeReadU64(entity + kEntityPairsOff0);
    if (!p0) return 0;
    const uint64_t p1 = SafeReadU64(p0 + kEntityPairsOff1);
    if (!p1) return 0;
    const uint64_t pairs = SafeReadU64(p1 + kEntityPairsOff2);
    if (!pairs) return 0;

    const uint64_t comps = SafeReadU64(entity + kEntityComponentsOff);
    if (!comps) return 0;
    const uint64_t compsEnd = SafeReadU64(entity + kEntityComponentsEndOff);
    if (compsEnd <= comps) return 0;

    uint64_t count = (compsEnd - comps) / kComponentPtrSize;
    if (count > kMaxComponentCount) count = kMaxComponentCount;

    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t pairSlot = pairs + i * kComponentPairSize;
        const uint64_t pname = SafeReadU64(pairSlot);
        if (!pname) break;

        // 组件名是 ANSI 短串，直接逐字节比（避免为一个名字引入字符串解析）
        bool match = true;
        for (size_t k = 0; ; ++k) {
            const uint8_t c = SafeReadU8(pname + k);
            if (c != (uint8_t)name[k]) {
                match = false;
                break;
            }
            if (c == 0) break;
        }
        if (!match) continue;

        const uint32_t idx = SafeReadU32(pairSlot + kComponentPairIndexOff);
        if (idx >= count) return 0;
        return SafeReadU64(comps + (uint64_t)idx * kComponentPtrSize);
    }
    return 0;
}

uint64_t NavDriver::FindPlayerEntity()
{
    // Resolve the controlled local entity from the map-chain prefix. The first
    // Player in the nearby tree can be a teammate and is never a self identity.
    uint64_t entity = SafeReadU64(m_walker.ModuleBase() + BaseOffset);
    for (int i = 0; entity && i < kLocalPlayerChainDepth; ++i)
        entity = SafeReadU64(entity + kMapObjectChain[i]);
    if (!IsPlausibleAddress(entity)) {
        m_playerCache = m_positionedCache = 0;
        return 0;
    }
    if (entity != m_playerCache || !m_positionedCache) {
        const auto player = FindComponent(entity, "Player");
        const auto positioned = FindComponent(entity, "Positioned");
        if (!player || !positioned) {
            m_playerCache = m_positionedCache = 0;
            return 0;
        }
        m_playerCache = entity;
        m_positionedCache = positioned;
        m_playerCacheTick = GetTickCount64();
        LOGI("[Nav][SELF] source=local_player_chain entity=0x%llX Player=0x%llX Positioned=0x%llX xy_addr=0x%llX",
             (unsigned long long)entity, (unsigned long long)player,
             (unsigned long long)positioned, (unsigned long long)(positioned + kPositioned_XOff));
    }
    return entity;
}

bool NavDriver::ReadPlayerAnimPos(int32_t& x, int32_t& y)
{
    x = 0;
    y = 0;

    const uint64_t player = FindPlayerEntity();
    if (!player) {
        return false;
    }

    // Positioned 组件也缓存：找到玩家时顺手解析一次，
    // 之后每次读坐标只剩「2 次 4 字节读」，开销可以忽略。
    if (!m_positionedCache || !IsPlausibleAddress(m_positionedCache)) {
        m_positionedCache = FindComponent(player, "Positioned");
    }
    const uint64_t posComp = m_positionedCache;
    if (!posComp) {
        return false;
    }

    // 读 +0x444 / +0x448：各一个 int32，值就是世界像素，不做任何换算。
    // （旧版读 +0x490 的打包 float 对再 /10.88，那条路径已废弃）
    x = (int32_t)SafeReadU32(posComp + kPositioned_XOff);
    y = (int32_t)SafeReadU32(posComp + kPositioned_YOff);

    // 坐标系没加载时会是 0,0，当作读不到
    if (x == 0 && y == 0) {
        return false;
    }
    return true;
}

void NavDriver::FillStatus(NavStatus& st, uint8_t state,
                           int32_t px, int32_t py) const
{
    st.x = px;
    st.y = py;
    st.targetX = m_goalX;
    st.targetY = m_goalY;
    st.remaining = (uint32_t)(m_pts.empty()
        ? 0
        : m_pts.size() - (m_idx < m_pts.size() ? m_idx : m_pts.size()));
    st.state = state;
    st.reserved[0] = st.reserved[1] = st.reserved[2] = 0;
}

// ============================================================
// 一个 tick
// ============================================================
bool NavDriver::Tick(NavStatus& outStatus)
{
    int32_t px = 0, py = 0;
    const bool havePos = ReadPlayerAnimPos(px, py);

    // 没有导航任务：只上报空闲状态（低频，由调用方决定要不要发）
    if (!m_active || m_pts.empty()) {
        if (m_active && m_pts.empty()) {
            m_active = false;
        }
        FillStatus(outStatus, havePos ? NAV_IDLE : NAV_FAILED, px, py);
        return false;
    }

    if (!havePos) {
        FillStatus(outStatus, NAV_FAILED, px, py);
        LOGI("[Nav] 读不到玩家坐标，中止导航");
        Stop();
        return true;
    }

    const uint64_t nowTick = GetTickCount64();

    // ------------------------------------------------------------
    // 0) 推进策略：先看能不能"捞近点"
    //
    // 按索引推进（默认）：只在 [m_idx - backOff, m_idx + 若干] 窗口里重定位，
    //   且 m_idx 只允许变大。这条专治"路径重算后走回身后"。
    // 关掉时：全路径找最近点 —— 老行为，允许回头（一般不开）。
    // ------------------------------------------------------------
    if (m_cfg.byIndex) {
        const int best = NearestIndexAround(m_pts, px, py, (int)m_idx,
                                            m_cfg.backOff, 8);
        if (best > (int)m_idx) {
            m_idx = (uint32_t)best;
            m_issued = false;
            m_instructedMoving = false;
            m_reissueCount = 0;
            m_waypointStartTick = 0;
            m_issuedAtTick = 0;
            m_lastDist = -1;
        }
    }

    if (m_idx >= m_pts.size()) {
        m_active = false;
        FillStatus(outStatus, NAV_ARRIVED, px, py);
        LOGI("[Nav] 全部航点走完，终点 (%d,%d)", px, py);
        return true;
    }

    // ------------------------------------------------------------
    // 1) 跳过身边一圈的点
    //
    // 路径里角色身边那几个点（尤其是锚点）永远走不到 —— 角色已经
    // 站在它旁边了，再"走过去"就是原地蹭。直接跳过它们。
    // ------------------------------------------------------------
    {
        const long long skipSq = (long long)m_cfg.skipNear * m_cfg.skipNear;
        while (m_idx + 1 < m_pts.size()) {
            const long long dx = (long long)m_pts[m_idx].x - px;
            const long long dy = (long long)m_pts[m_idx].y - py;
            if (dx * dx + dy * dy > skipSq) break;
            ++m_idx;
            m_issued = false;
            m_instructedMoving = false;
            m_reissueCount = 0;
            m_waypointStartTick = 0;
            m_issuedAtTick = 0;
            m_lastDist = -1;
        }
    }

    const NavBatchChunk* target = CurPoint();
    if (!target) {
        m_active = false;
        FillStatus(outStatus, NAV_ARRIVED, px, py);
        return true;
    }

    m_goalX = target->x;
    m_goalY = target->y;

    const int32_t tol = m_tolerance > 0 ? m_tolerance : m_cfg.arriveTolerance;
    const int32_t dist = Dist2D(px, py, m_goalX, m_goalY);

    // ------------------------------------------------------------
    // 2) 到达判定
    // ------------------------------------------------------------
    if (dist <= tol) {
        ++m_idx;
        m_issued = false;
        m_instructedMoving = false;
        m_reissueCount = 0;
        m_waypointStartTick = 0;
        m_issuedAtTick = 0;
        m_lastDist = -1;
        // 到达后强制留一段"消化时间"：实测 10ms 内发下一个必被吞，
        // 130ms 以上才稳定生效。
        m_nextIssueNotBefore = nowTick + m_cfg.settleMs;

        if (m_idx >= m_pts.size()) {
            m_active = false;
            FillStatus(outStatus, NAV_ARRIVED, px, py);
            LOGI("[Nav] 全部航点走完，终点 (%d,%d)", px, py);
        }
        else {
            FillStatus(outStatus, NAV_REACHED, px, py);
        }
        return true;
    }

    // ------------------------------------------------------------
    // 3) 单点超时（硬卡死）
    // ------------------------------------------------------------
    if (m_waypointStartTick == 0) {
        m_waypointStartTick = nowTick;
        m_lastProgressTick = nowTick;
    }
    if ((nowTick - m_waypointStartTick) >= (uint64_t)m_cfg.maxMsWaypoint) {
        LOGI("[Nav] 航点 %u (%d,%d) 超时 %llu ms（当前 %d,%d），判卡住",
             m_idx, m_goalX, m_goalY,
             (unsigned long long)(nowTick - m_waypointStartTick), px, py);
        Stop();
        FillStatus(outStatus, NAV_STUCK, px, py);
        return true;
    }

    // ------------------------------------------------------------
    // 4) 观测：位移 / 距离是否缩短
    // ------------------------------------------------------------
    const bool isMoving = (px != m_lastPX || py != m_lastPY);
    if (isMoving) {
        m_lastMoveTick = nowTick;
    }
    else if (m_lastMoveTick == 0) {
        m_lastMoveTick = nowTick;
    }

    // "有进展" = 离目标明显更近了。用它判软卡死比"坐标有没有动"准得多
    // （沿墙蹭的时候坐标一直在动，但一点没近）。
    if (m_lastDist < 0 || dist < m_lastDist - (m_cfg.stuckEps > 0 ? m_cfg.stuckEps : 1)) {
        m_lastProgressTick = nowTick;
    }
    m_lastDist = dist;

    if (m_issued && isMoving) {
        m_instructedMoving = true;
    }
    m_lastPX = px;
    m_lastPY = py;

    const uint64_t stillMs  = nowTick - m_lastMoveTick;
    const uint64_t noProgMs = nowTick - m_lastProgressTick;

    // ------------------------------------------------------------
    // 5) 软卡死：一直没靠近
    //
    // ★★ 走路 call 的真实语义（walkhold_probe 单次调用实测）：
    //
    //    调一次 call -> 角色自己朝目标走一段（约 200 像素 / 4 秒），
    //    然后**自己停下**（到达 or 被地形挡住）。此时坐标不再变化。
    //
    //    也就是说：一次调用 = 一段自主移动，走完必须再发下一次。
    //    这正是之前所有版本都失败的原因 —— 要么发了不再发（走一段就停），
    //    要么高频连发（打断自主移动，一步都走不动）。
    //
    // 于是判定逻辑变成：
    //     · 已下发 & 角色在动  -> 等它走完（不干预）
    //     · 已下发 & 角色静止  -> 说明这一段落幕了：到点就推进，
    //                            没到就再发一次
    // ------------------------------------------------------------
    if (noProgMs >= (uint64_t)m_cfg.noProgressMs && m_issued && !m_instructedMoving) {
        // ★ 把"为什么没靠近"记下来，不然界面上只能看到"停了一会儿"，
        //   分不清是被墙挡、被怪挡，还是指令根本没生效。
        //   stillMs 是一直没动多久：接近 noProgMs = 原地卡死（墙/怪顶住）；
        //   明显小于 noProgMs  = 一直在动但没靠近（沿墙蹭/绕圈）。
        const bool rootedAtWall = (stillMs + 300 >= noProgMs);
        LOGI("[Nav] 航点 %u (%d,%d) 已 %llu ms 没靠近（当前 %d,%d，距离 %d）；"
             "静止 %llu ms -> %s，跳过",
             m_idx, m_goalX, m_goalY, (unsigned long long)noProgMs,
             px, py, dist, (unsigned long long)stillMs,
             rootedAtWall ? "原地不动（被墙/怪挡住）" : "在动但没靠近（沿墙蹭）");
        ++m_idx;
        m_issued = false;
        m_instructedMoving = false;
        m_reissueCount = 0;
        m_waypointStartTick = 0;
        m_issuedAtTick = 0;
        m_lastDist = -1;
        m_lastProgressTick = nowTick;
        m_nextIssueNotBefore = nowTick + m_cfg.settleMs;

        if (m_idx >= m_pts.size()) {
            m_active = false;
            FillStatus(outStatus, NAV_STUCK, px, py);
            LOGI("[Nav] 剩下的点全跳过了，导航结束");
            return true;
        }
        FillStatus(outStatus, NAV_MOVING, px, py);
        return true;
    }

    // ------------------------------------------------------------
    // 6) 下发判定
    //
    // ★★ 最终定论（walkhold_probe.dll 单次调用实测）：
    //
    //   调一次走路 call，角色会**自己朝目标走一段**（约 200 像素 /
    //   4 秒），然后自己停下（到点，或被地形挡住）。
    //
    //   所以正确节奏是：
    //     发一次 -> 等它走完（角色停下来） -> 还没到就再发一次
    //
    //   绝对不能高频连发：实测每 150ms 发一次会让角色原地抖动，
    //   因为每次新指令都把上一次的自主移动打断，一段都走不完。
    //   （这就是"角色一直在动但越走越远"的根因）
    //
    //   判定：
    //     settleOk   : 距上次下发已过 settleMs
    //     roleStill  : 角色已静止 stillBeforeMs（= 这一段走完了）
    //     未下发过 / 走完但没到 -> 下发
    // ------------------------------------------------------------
    const bool settleOk   = (nowTick >= m_nextIssueNotBefore);
    const bool roleStill  = (stillMs >= (uint64_t)m_cfg.stillBeforeMs);
    const bool neverIssued = !m_issued;

    uint64_t sinceIssueMs = 0;
    if (m_issuedAtTick != 0) {
        sinceIssueMs = nowTick - m_issuedAtTick;
    }

    // 走了但根本没动（指令被吞）：等 reissueMs 后重试
    const bool swallowed = m_issued && !m_instructedMoving &&
                           (sinceIssueMs >= (uint64_t)m_cfg.reissueMs) &&
                           (m_reissueCount < m_cfg.maxReissue);

    // 这一段走完了（角色停下来了）：该发下一段
    const bool segmentDone = m_issued && m_instructedMoving && roleStill;

    const bool shouldIssue = settleOk &&
                             (neverIssued || swallowed || segmentDone);

    if (shouldIssue) {
        // ------------------------------------------------------------
        // ★ 不要再注入鼠标点击！
        //
        // 旧逻辑在这里调 NudgeInputOnce() 往游戏窗口发一次左键点击，
        // 想用它"唤醒动作系统"。实测这是错的：
        //   · 游戏会把那次点击当成"点地面走过去"，产生一条自己的移动
        //     指令，和我们的走路 call 抢方向盘
        //   · 表现就是角色朝点击位置（客户区 50%,58%）跑，也就是
        //     "往左上/左下跑偏"，和我们算的路径完全无关
        //   · 而且这个跑偏是持续的，每 tick 点一次就偏一次
        //
        // 走路 call 本身完全不需要这种"唤醒" —— dir_probe / walkhold_probe
        // 都是裸调 call、零输入，角色照样走得好好的。
        // ------------------------------------------------------------
        const navwalk::WalkResult wr = m_walker.WalkTo(m_goalX, m_goalY);

        if (wr == navwalk::WalkResult::RateLimited) {
            // 被熔断拒了：不当作失败，下个 tick 再试
            FillStatus(outStatus, NAV_MOVING, px, py);
            return true;
        }
        if (wr != navwalk::WalkResult::Success) {
            LOGI("[Nav] 走路 call 失败: %s，中止", navwalk::WalkResultName(wr));
            Stop();
            FillStatus(outStatus, NAV_FAILED, px, py);
            return true;
        }

        if (neverIssued) {
            LOGI("[Nav] 下发航点 %u/%zu (%d,%d)  当前 (%d,%d)  距离 %d",
                 m_idx, m_pts.size(), m_goalX, m_goalY, px, py, dist);
        }
        else if (swallowed) {
            ++m_reissueCount;
            LOGI("[Nav] 指令被吞，重发 %u/%zu (%d,%d) 第 %u/%u 次（等了 %llu ms 没动）",
                 m_idx, m_pts.size(), m_goalX, m_goalY,
                 m_reissueCount, m_cfg.maxReissue,
                 (unsigned long long)sinceIssueMs);
        }
        else if (segmentDone) {
            ++m_segmentCount;
            LOGI("[Nav] 第 %u 段走完（当前 %d,%d，离点 %d），续推 (%d,%d)",
                 m_segmentCount, px, py, dist, m_goalX, m_goalY);
        }

        m_issuedAtTick = nowTick;
        m_issued = true;
        // 新一轮"是否观察到位移"的观察窗口
        m_instructedMoving = false;
        // 注意：这里**不要**复位 m_waypointStartTick ——
        // 续推不是新的航点，超时计时应该继续累计。
    }

    FillStatus(outStatus, NAV_MOVING, px, py);
    return true;
}

} // namespace navdrive
