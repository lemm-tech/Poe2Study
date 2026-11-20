#pragma once
// ============================================================
// NavDriver.h —— 进程内导航状态机（Monitor 侧）
//
// 职责边界：
//   · Plug 侧负责"想去哪"（A* 算航点、平滑、"GPS 式"逐帧重规划）
//   · Monitor 侧负责"怎么走"（读玩家坐标、按路径顺序推进、
//     判断到位 / 卡住 / 防回头、上报状态）
//
// 为什么走路必须放 Monitor：走路 call 是游戏自己的成员函数，
// 只能在游戏进程内调用。Plug 是外部进程，只能经 OLA 读内存。
//
// ★ 这一版相对上一版的三个关键改动（全部来自实测踩坑）：
//
//   1. **路径按"序列"下发，不是按"队列"**。
//      MT_NavBatch 送来的是一整条路径 + 一个 seq。Monitor 不只是
//      把它们排进队列，而是保存成"当前路径"，并且记住**已经推进到
//      哪个下标**（m_idx）。于是：
//        · 同一条路径被反复下发（GPS 每帧重算）时不会重复走
//        · 路径从 185 个点变成 140 个点时，能认出"已经走过的部分"
//      上一版用 deque + pop_front，Plug 每帧重算一次就等于把整条
//      路径重走一遍 —— 这是"原地画圈"的直接原因。
//
//   2. **防回头**：新路径的第一个点常常落在角色身后（A* 从角色格
//      出发，但角色的真实位置可能已经在下一个格子了）。直接走过去
//      就是往回走一步再折返。所以下发前先"跳过身边一圈的点"，
//      并且**只允许沿路径向前推进**（m_idx 只能变大）。
//
//   3. **卡死判定改成"距离不缩短"**，不是"坐标不动"。
//      角色沿墙蹭着走时坐标一直在变，但离目标一点没近 ——
//      上一版判不出来，会一直蹭到超时。
//
// 线程模型：
//   导航在主线程 tick（由 dllmain 的循环驱动），不另开线程 ——
//   走路 call 会进游戏动作系统，多线程调用是自找竞态。
//   管道线程只负责把"路径 / 参数"丢进来，tick 里消费。
// ============================================================
#include <cstdint>
#include <string>
#include <vector>

#include "ActionWalker.h"

#include "../Common/Protocal.h"

namespace navdrive {

// 运行期可调参数（对应管道 MT_NavCfg）
struct NavConfig {
    // 到这里就算到了（世界像素）
    int32_t  arriveTolerance = 13;

    // 比这更近的航点直接跳过（防止绕回来踩自己身后的点）
    // ★ 必须明显小于航点间距（默认 23），否则每次重规划都会
    //   "跳过身边一圈"，路径下标跳着走，角色原地打转。
    //   实测：30 太大（> 23），改成 14（约半个航点）。
    int32_t  skipNear = 14;

    // 一个 tick 内位移小于它就当"没动"
    int32_t  stuckEps = 2;

    // 单个航点最多给它多少毫秒。超时判卡住。
    uint32_t maxMsWaypoint = 20000;

    // 到一个点之后静默多久再允许下发下一个（实测 130ms 以上才生效）
    uint32_t settleMs = 150;

    // 下发后多久角色还没动就补发一次
    uint32_t reissueMs = 1500;

    // ★ 判定"角色这一段走完了"所需的静止时长（毫秒）。
    //   走路 call 一次会让角色自主走 3~5 秒，中途不能打断；
    //   停下来超过这个时间才认为这一段落幕，可以发下一段。
    uint32_t stillBeforeMs = 250;

    // 距离多久没缩短算"软卡死"（不是硬超时）
    // ★ 必须大于"一个航点正常走完的时间"。23 像素一步大约
    //   0.6~1.2 秒，给它 4 秒余量才不会被误判。
    uint32_t noProgressMs = 4000;

    // 单点最多补发几次
    uint32_t maxReissue = 2;

    // 允许沿路径回退几格（防回头）。0 = 绝对不回头
    int32_t  backOff = 15;

    // 按索引推进（默认开）。关掉就退化成"最近点即目标"的老行为
    bool     byIndex = true;

    // 玩家实体缓存有效期（毫秒）
    uint32_t playerCacheMs = 1500;

    // 从管道 NavCfg 覆盖
    void Apply(const NavCfg& c);
    NavCfg ToWire() const;
};

// 设置 SendInput 前要找的游戏窗口标题（空 = 用当前前台窗口）。
// 由 dllmain 启动时从 config.ini 填入；指针必须长期有效。
void SetNudgeWindowTitle(const char* title);

class NavDriver {
public:
    NavDriver() = default;

    // 初始化：绑定模块基址、走路器、世界坐标上界
    bool Initialize(uint64_t moduleBase, const navwalk::WorldBounds& bounds,
                    std::string& err);

    // ---- 路径输入 ----
    //
    // 推进模式（GPS）：多次调用同一个 seq，按 baseIdx 拼成一张完整路径。
    // 一旦收到"最后一个包"（isLast），就把整条路径定为当前路径。
    // 返回 false = 参数非法
    bool FeedPathChunk(uint32_t seq, uint32_t total, uint32_t baseIdx,
                       int16_t tolerance, bool isLast,
                       const NavBatchChunk* pts, uint32_t count);

    // 单点模式（旧接口 / 手工调试）：直接把它当作“一步路径”
    bool EnqueueGoal(int32_t x, int32_t y, uint16_t tolerance);

    // 中止导航，清空路径
    void Stop();

    // ---- 运行期参数 ----
    void SetConfig(const NavCfg& c) { m_cfg.Apply(c); }
    NavCfg GetConfigWire() const { return m_cfg.ToWire(); }

    // ---- 曲线查询（读坐标用）----
    bool ReadPlayerAnimPos(int32_t& x, int32_t& y);
    uint64_t PositionedAddr() const { return m_positionedCache; }

    // 当前路径快照（给扫图落盘/诊断用）
    const std::vector<NavBatchChunk>& PathPoints() const { return m_pts; }
    uint32_t CurrentSeq() const { return m_seq; }
    uint32_t CurrentIndex() const { return m_idx; }

    // 主循环里调用一次 = 一个 tick（内部含节流 Sleep）。
    // 返回 true 表示 outStatus 有待上报的状态变化。
    bool Tick(NavStatus& outStatus);

    // ---- 状态 ----
    bool     Active() const { return m_active; }
    uint32_t QueueDepth() const { return (uint32_t)(m_pts.empty() ? 0 : m_pts.size() - m_idx); }
    navwalk::ActionWalker& Walker() { return m_walker; }

    // ---- 诊断用（走路 call 槽位扫描）----
    // 把私有的"找玩家 / 找组件"临时开放出来：扫描器要拿玩家身上的
    // Actor / Pathfinding 组件当候选对象试槽位，逻辑和这里完全一样，
    // 没必要在 dllmain 里再抄一份组件表解析。
    uint64_t DiagPlayerEntity() { return FindPlayerEntity(); }
    uint64_t DiagComponent(uint64_t entity, const char* name)
    {
        return entity ? FindComponent(entity, name) : 0;
    }

private:
    uint64_t FindPlayerEntity();
    uint64_t FindComponent(uint64_t entity, const char* name);

    void FillStatus(NavStatus& st, uint8_t state,
                    int32_t px, int32_t py) const;

    // 收到完整路径后：定下标 + 复位本点状态
    void BeginNewPath();

    // 当前目标（按 m_idx）
    const NavBatchChunk* CurPoint() const
    {
        if (m_idx >= m_pts.size()) return nullptr;
        return &m_pts[m_idx];
    }

    navwalk::ActionWalker m_walker;
    NavConfig             m_cfg;

    // ---- 当前路径 ----
    std::vector<NavBatchChunk> m_pts;
    uint32_t              m_seq = 0;          // 当前路径的序号
    uint32_t              m_idx = 0;          // 已推进到的下标（= 当前目标）
    int32_t               m_tolerance = 13;
    bool                  m_active = false;

    // ---- 拼包状态 ----
    uint32_t              m_feedSeq = 0;      // 正在拼的 seq
    uint32_t              m_feedExpect = 0;   // 正在拼的 total
    int32_t               m_feedTol = 13;
    bool                  m_feeding = false;

    // ---- 本点的执行状态 ----
    bool                  m_issued = false;       // 本点是否已下发过
    bool                  m_instructedMoving = false;
    uint64_t              m_issuedAtTick = 0;     // 本点最近一次下发时刻
    uint32_t              m_reissueCount = 0;
    uint32_t              m_segmentCount = 0;   // 已续推段数（诊断）
    uint64_t              m_nextIssueNotBefore = 0;
    uint64_t              m_waypointStartTick = 0;

    // ---- 观测 ----
    int32_t               m_lastPX = -1;
    int32_t               m_lastPY = -1;
    int32_t               m_lastDist = -1;        // 上一次到目标的距离
    uint64_t              m_lastProgressTick = 0; // 距离最后一次"变近"的时刻
    uint64_t              m_lastMoveTick = 0;     // 最后一次"有位移"的时刻

    int32_t               m_goalX = 0;
    int32_t               m_goalY = 0;

    // ---- 缓存 ----
    uint64_t              m_playerCache = 0;
    uint64_t              m_playerCacheTick = 0;
    uint64_t              m_positionedCache = 0;
    uint64_t              m_lastScanTick = 0;

public:
    // 距上次"有位移"过了多久（毫秒）。
    //
    // ★ 给战斗模块用：走路 call 一次会让角色自主走 3~5 秒然后自己停下，
    //   只有"停下了"的窗口才适合插技能（插在移动中会打断自主移动，
    //   表现就是"走一步停一下"，比不打还慢）。
    //   这个值就是"角色已经静止多久"，0 = 刚动过 / 从未观测到位移。
    //
    // 纯观测值，只读不写，不参与任何导航决策；
    // 主循环是单线程 tick 模型，所以也不需要额外同步。
    uint32_t LastStillMs() const
    {
        // ★ 从未观测到位移时，返回"足够长"而不是 0。
        //   原实现返回 0 会让"角色一直站着没动过"被误判成"刚移动过"，
        //   于是技能永远被安全闸门拒绝（实测：启动后静置 3 秒仍然被拒）。
        //   从未动过 = 静止时间无穷大，语义上就该返回一个大值。
        if (m_lastMoveTick == 0) return 0xFFFFFFFFu;
        const uint64_t now = GetTickCount64();
        if (now <= m_lastMoveTick) return 0;
        const uint64_t d = now - m_lastMoveTick;
        return (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)d;
    }
};

} // namespace navdrive
