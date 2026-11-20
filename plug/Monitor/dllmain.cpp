// dllmain.cpp：Monitor DLL 入口（注入游戏进程内运行）
//
// 职责：
//   1. DLL 被注入 PathOfExile.exe 后，开线程启动命名管道服务端
//   2. 收到客户端 MT_MapReq 请求时，采集地图代价表并推送给客户端
//   3. 收到 MT_NavBatch（整条路径）时交给 NavDriver 逐点执行走路 call
//   4. 持续把玩家坐标 + 导航状态上报给客户端
//   5. 收到 MT_ScanBmp 时把代价表直接落盘（诊断用，不经过管道）
//
// 采集方式：
//   CostProbe 提供两条路：
//     CaptureArray —— 直接读 map+0xD0+layer*0x18 的 vector（快，不碰游戏线程）
//     CaptureByCall —— 逐点调 CostCall（慢，但作为兜底）
//   默认优先 CaptureArray，失败自动回退 CaptureByCall。
//
// 打包：统一走 mapdata::ToFrame（nibble 打包唯一实现），不再手写移位。
//
// 日志写文件（monitor_log.txt 落在 DLL 同目录），不弹控制台。

#include <windows.h>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include <cmath>
#include <thread>


#include "NamedPipeServer.h"
#include "CostProbe.h"
#include "MapSource.h"
#include "MonitorConfig.h"
#include "NavDriver.h"
#include "SkillPusher.h"
#include "IdSkillCaster.h"
#include "PacketService.h"
#include "ViewService.h"
#include "Dump.h"
#include "Log.h"

#include "../Common/Protocal.h"
#include "../Common/MonitorInterface.h"

// Read by NavGUI directly from the mapped image; no remote call is needed.
extern "C" {
__declspec(dllexport) extern const monitorabi::Info MonitorInterfaceInfo{};
}
// ---- 键盘驱动（WASD，老路径，保留）----
// 前向声明：这两个函数定义在文件靠后处，但消息分发要用到。
// 必须放在 Protocal.h 之后，因为参数类型 NavGo 定义在那里。
static void PushPosToHook(uint64_t positioned);
static void HandleKeyGo(const NavGo& go);
static void HandleKeyStop();
#include "../Common/MapFrame.h"

// 全局配置（启动时从 DLL 同目录 config.ini 读，读不到就用默认值）
static mcfg::MonitorConfig g_cfg;

// 本 DLL 所在目录（供 mcfg 定位 config.ini 用）。
// 必须是唯一定义（非 inline），否则各编译单元各一份，设了也读不到。
namespace mcfg { namespace detail { std::string g_dllDir; } }

NamedPipeServer*                     g_PipeServer = nullptr;
std::unique_ptr<mapscan::IMapSource> g_source;      // 取数策略（Prepare 自检通过后才可用）
std::atomic_bool                     g_scanning{ false };
// ★ 走路槽位扫描专用标志。
//   不能和 g_scanning 共用：g_scanning 是"扫图"的闸，扫图请求在读线程里
//   同步跑完就复位；而 walkscan 是甩到独立线程跑的，生命周期完全不同。
//   共用会导致一次 walkscan 把后续扫图请求全当"重复请求"拒掉，两边各自
//   复位时还会互相踩。
std::atomic_bool                     g_walkScanning{ false };

// ------------------------------------------------------------
// 导航状态机（只有管道线程写、主循环 tick 消费，用锁保护边界）
// ------------------------------------------------------------
static navdrive::NavDriver           g_nav;
static std::mutex                    g_navMutex;
static std::atomic_bool              g_navReady{ false };
// ------------------------------------------------------------
// 技能器（新战斗模块的地基）
//
// 和 g_nav 是两条独立通道：
//   g_nav    -> 走路 call，入口随 vtable 变（[[动作系统]+0x198]）
//   g_skill  -> 技能 call，固定 RVA 0x18C940
// 但两者共用同一个动作系统对象（rcx），往同一个动作队列塞东西，
// 所以对动作系统的一切调用都走同一把锁 g_navMutex。
// ------------------------------------------------------------
static navskill::SkillPusher         g_skill;
static navskill::IdSkillCaster       g_idSkill;
static std::atomic_bool              g_skillReady{ false };

// 桥接：SkillPusher 要一个普通函数指针，而 ReadPlayerAnimPos 是成员。
// 这里转发一下，注意走 g_navMutex —— NavDriver 的缓存不是原子的。
// ★★★ 重要：这里绝不能加 g_navMutex！★★★
//
//   调用链：MT_SkillCast 线程先 lock(g_navMutex) -> g_skill.Cast()
//           -> ComputeCastPos -> 本函数。
//   std::mutex 不可重入，同一线程二次加锁 = 死锁 ->
//   g_navMutex 被永久占住 -> NavDriver::Tick 卡死 -> 游戏闪退。
//   （2026-11-09 实测：点一次释放技能，游戏直接闪退，日志停在"槽位链"那行）
//
//   安全性：ReadPlayerAnimPos 只做只读操作（SafeReadU32 读 +0x444/+0x448），
//   不会改 NavDriver 状态，所以不需要额外加锁保护。
static bool SkillPosBridge(int32_t& x, int32_t& y)
{
    if (!g_navReady.load()) return false;
    return g_nav.ReadPlayerAnimPos(x, y);
}

// ============================================================
// 技能调用的"安全窗口"判断
//
// ★ 为什么需要：实测两次操作暴露的规律：
//     1) 静止时（朝向0.0°，久未移动）放技能 -> 调用"成功"但游戏毫无反应
//     2) 刚走完一段路（2.5 秒前还在下发航点）放技能 -> 游戏卡死
//  原因是走路 call 和技能 call 共用同一个动作系统 + 同一个动作队列
//  （日志里两边打印的 actionSys 都是 0x56F591E2D00）。
//  导航还在跑、或刚停下动作还没消化完时插技能，就会打架。
//
// 判据（全部满足才允许放技能）：
//   1. 导航不活跃（NavDriver::Active() == false）
//   2. 角色已静止至少 kSkillMinStillMs（默认 1200ms）
//      —— "静止"= NavDriver 观测到最近一次位移距今超过这个时长
//
// 返回 true 表示可以放；false 时 why 里写原因，调用方打日志给用户看。
// ============================================================
// 导航停止后至少静止这么久才允许放技能（毫秒）
//   实测 2.5 秒仍会卡死，所以留到 1200ms 起步 + Active() 双重判断；
//   如果实测还卡，把这个值往上调（3000 / 5000）。
static constexpr uint32_t kSkillMinStillMs = 1200;

// 手动边走边放诊断通道的判据。
//
// ★ 这段判据被实测推翻过两次，把结论留在这里免得以后又改回去：
//
//   v1「距上次位移 >= 250ms」：想等角色停下再放。
//     结果**永远不满足** —— 导航推进时走路 call 让角色持续移动，
//     LastStillMs 一直是十几到几十毫秒。日志里每 6 秒一条拒绝，一次没放出去。
//
//   v2「距上次位移 >= 50ms」：想避开"刚下发走路 call 的那一瞬间"。
//     结果**误杀一半** —— 实测放行时那个值是 875ms / 1562ms，
//     被拒的几次全是 0~16ms。原因是 LastStillMs 的语义是
//     "距上次**观测到**位移多久"，采样粒度就是 NavDriver 的 tick，
//     角色一移动这个值立刻归零 —— "刚下发 call"和"正在移动"分不开。
//
//   v3（现在）：**不做移动相关判定**。
//     真正要防的是"技能 call 和走路 call 抢动作队列"，而这件事
//     已经由两条独立节流保证：
//       · 走路 call：60ms 最小间隔 + 每秒 15 次
//       · 技能 call：5000ms 最小间隔 + 每秒 1 次
//     两者天然错开，5 秒一次的技能插不进 60ms 一次的走路里。
//     再加一道基于坐标的门槛只会误杀，不会增加安全性。
//
//   这个变量留作"手动收紧"的旋钮：设成 >0 就恢复 v2 行为，
//   用于将来排查"技能把移动打断"这类问题时做 A/B。
//   （用变量而不是 constexpr，否则 `if (x == 0)` 会被编译器判成恒真而报警告）
static uint32_t g_motionTestMinSinceMove = 0;

static bool SkillSafeWindow(std::string& why, bool allowDuringMotionTest = false)
{
    why.clear();

    if (!g_navReady.load()) {
        why = "导航未就绪";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_navMutex);

    // ---- 手动移动施法诊断通道（hygiene bit3）----
    //
    // ★ 为什么必须区别对待：
    //   下面那套判据（导航不活跃 + 静止 ≥1200ms）是给**人工点按钮**设计的。
    //   人工点按钮时角色一般站着不动，插一次技能很安全。
    //   移动施法诊断需要验证一次受控调用能否与行走并行，因此单独放行；
    //   该位只由用户主动点击的测试面板设置，不由导航后台自动触发。
    //
    //   测试通道的判据见上方说明：默认不做
    //   移动相关判定，直接放行（节流已经在 SkillPusher 里做过）。
    if (allowDuringMotionTest) {
        if (g_motionTestMinSinceMove == 0) {
            return true;      // 默认：不检查移动状态
        }

        // 只有把旋钮调大于 0 时才走这条（排查"技能打断移动"用的 A/B）
        const uint32_t sinceMove = g_nav.LastStillMs();
        if (sinceMove != 0xFFFFFFFFu && sinceMove < g_motionTestMinSinceMove) {
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "边走边放测试：距上次位移 %u ms（需 >= %u ms）",
                        sinceMove, g_motionTestMinSinceMove);
            why = buf;
            return false;
        }
        return true;
    }

    if (g_nav.Active()) {
        why = "导航进行中（技能与走路共用动作队列，会打架）";
        return false;
    }

    const uint32_t still = g_nav.LastStillMs();
    if (still < kSkillMinStillMs) {
        char buf[96];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "刚移动过，静止 %u ms（需 >= %u ms）", still, kSkillMinStillMs);
        why = buf;
        return false;
    }

    return true;
}

// ============================================================
// 技能调用"进行中"标记 + 看门狗
//
// 作用：
//   1. 防重入：上一次调用没返回，就不接受新请求（避免请求堆积）
//   2. 卡死自检：一次调用超过 kSkillStuckMs 还没返回，说明游戏主线程
//      被我们卡住了。此时打显著日志并**永久停用**技能功能，
//      避免用户反复卡死/闪退（重新注入 Monitor 可恢复）。
//
// 为什么需要这道防线：动作系统是游戏主线程的东西，我们是从
// 独立线程调进去的，一旦游戏那边状态不对，调用就可能永远不返回。
// 有看门狗至少能保证"只坏一次"。
// ============================================================
static std::atomic_bool g_skillInFlight{ false };
static std::atomic<uint64_t> g_skillStartTick{ 0 };
static std::atomic_bool g_skillDisabled{ false };
static std::atomic<int32_t>  g_skillFailStreak{ 0 };   // 连续"未执行"次数
static constexpr uint32_t kSkillStuckMs = 5000;

static void SkillMarkDone()
{
    g_skillInFlight = false;
    g_skillStartTick = 0;
}

// 由主循环定期调用
static void SkillWatchdog()
{
    if (g_skillDisabled.load()) return;
    if (!g_skillInFlight.load()) return;
    const uint64_t start = g_skillStartTick.load();
    if (start == 0) return;
    const uint64_t now = GetTickCount64();
    const uint64_t spent = (now > start) ? (now - start) : 0;
    if (spent > kSkillStuckMs) {
        g_skillDisabled = true;
        LOGI("[Skill] *** 看门狗：本次调用已 %llu ms 未返回，"
             "判定游戏被卡住，技能功能已停用（重新注入可恢复）",
             (unsigned long long)spent);
    }
}

// 主循环存活标志：DllMain 的 PROCESS_DETACH 里置 false，
// StartThread 的循环据此立刻退出（否则 FreeLibrary 后线程还在跑已卸载代码，
// 会把游戏一起带走 —— 这是之前"游戏莫名退出"的直接原因）。
static std::atomic_bool              g_threadRunning{ true };
static HANDLE g_startThread = nullptr;
static std::mutex g_shutdownMutex;

// ------------------------------------------------------------
// 键盘驱动（WASD）—— 老执行层，保留
//
// 和 g_nav 是两条并行的实现：
//   g_nav      走游戏动作系统的 vtable（现在的生产路径）
//   g_keyDrv   走 WASD 按键，方向已实测标定
// ------------------------------------------------------------
#include "KeyDriver.h"

static keydrive::KeyDriver            g_keyDriver;
static keydrive::KeyDriveConfig       g_keyCfg;

static std::atomic_bool               g_keyRunning{ false };
static std::atomic_bool               g_keyStopReq{ false };
static int32_t                        g_keyTargetX = 0;
static int32_t                        g_keyTargetY = 0;

// ------------------------------------------------------------
// 把 MapFrame 按行分块发给客户端
// ------------------------------------------------------------
static bool SendFrameOverPipe(const mapdata::MapFrame& frame, const managed::Route& route)
{
    if (!g_PipeServer) return false;

    const mapdata::MapFrame::Meta& m = frame.GetMeta();

    // 1) 元信息
    MapMeta meta{};
    meta.width     = m.width;
    meta.height    = m.height;
    meta.rowStride = m.rowStride;
    meta.layer     = m.layer;

    MapMetaMessage metaMsg(meta);
    if (!g_PipeServer->SendPackageTo(route, (void*)&metaMsg)) {
        LOGI("[发送] 地图元信息失败");
        return false;
    }
    LOGI("[发送] 元信息: %u x %u rowStride=%u layer=%u",
         m.width, m.height, m.rowStride, (unsigned)m.layer);

    // 2) 逐行发送，每行可能跨多个包
    const size_t maxPayload = MSG_LEN - sizeof(MapRowHeader);
    uint64_t sentPackets = 0;

    for (uint32_t y = 0; y < m.height; ++y) {
        uint32_t off = 0;
        while (off < m.rowStride) {
            const uint32_t chunk = (uint32_t)(
                (maxPayload < (size_t)(m.rowStride - off))
                    ? maxPayload : (size_t)(m.rowStride - off));

            const mapdata::MapFrame::RowSlice slice = frame.SliceRow(y, off, chunk);
            if (!slice.data || slice.len == 0) break;

            std::vector<uint8_t> pkt(sizeof(MapRowHeader) + slice.len);
            MapRowHeader hdr{};
            hdr.rowIndex  = y;
            hdr.offset    = off;
            hdr.rowStride = m.rowStride;
            memcpy(pkt.data(), &hdr, sizeof(hdr));
            memcpy(pkt.data() + sizeof(hdr), slice.data, slice.len);

            MapRowMessage rowMsg(pkt.data(), (uint32_t)pkt.size());
            if (!g_PipeServer->SendPackageTo(route, (void*)&rowMsg)) {
                LOGI("[发送] 地图行失败 y=%u", y);
                return false;
            }
            ++sentPackets;
            off += slice.len;
        }
    }

    LOGI("[发送] 完成，共 %llu 个 Row 包", (unsigned long long)sentPackets);
    return true;
}

// ------------------------------------------------------------
// 重建取数策略
//
// 为什么需要：g_source 里缓存的探针持有 map 对象地址，
// 玩家切图/回城后游戏会换一个新的 map 对象，旧地址指向的
// vector 变成空的 —— 此时必须重新解链，不能一直用旧探针。
// ------------------------------------------------------------
static bool RebuildSource(const char* why)
{
    LOGI("[采集] 重建取数策略（原因：%s）", why);

    std::string err;
    std::unique_ptr<mapscan::IMapSource> fresh = mapscan::MakeBestSource(err);
    if (!fresh) {
        LOGI("[采集] 重建失败: %s", err.c_str());
        return false;
    }
    g_source = std::move(fresh);
    return true;
}

// ------------------------------------------------------------
// 采集一张地图
// ------------------------------------------------------------
static bool CaptureMap(uint8_t layer, mapdata::CostBuffer& out)
{
    if (!g_source && !RebuildSource("首次采集前未初始化")) {
        return false;
    }

    // 最多尝试 2 轮：第 1 轮用现有策略，失败则重建后再试
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const mapdata::Geometry& geo = g_source->Geo();
        LOGI("[采集] 第 %d 轮：策略=%s layer=%u 几何 %u x %u (网格 %u x %u)",
             attempt, g_source->Name(), (unsigned)layer,
             geo.WorldW(), geo.WorldH(), geo.Grid().w, geo.Grid().h);

        const ULONGLONG t0 = GetTickCount64();
        const bool ok = g_source->Capture(out, layer);
        const ULONGLONG cost = GetTickCount64() - t0;

        if (ok) {
            const size_t nonzero = out.CountNonZero();
            LOGI("[采集] 成功：%llu ms，非零点 %zu / %zu",
                 (unsigned long long)cost, nonzero, out.Size());
            if (nonzero == 0) {
                LOGI("[采集] 警告：整张图非零点为 0（地形可能正在加载）");
            }
            return true;
        }

        LOGI("[采集] 第 %d 轮失败（%s，%llu ms）",
             attempt, g_source->Name(), (unsigned long long)cost);

        if (attempt == 1) {
            if (!RebuildSource("采集失败，可能是切图导致 map 对象变化")) {
                return false;
            }
        }
    }
    return false;
}

// ------------------------------------------------------------
// 扫图落盘（诊断）：在进程内直接把当前代价表写成 BMP
// ------------------------------------------------------------
static void HandleScanBmp(const ScanBmpReq& req, const managed::Route& route)
{
    mapdata::CostBuffer cost;
    if (!CaptureMap(req.layer, cost) || cost.Empty()) {
        LOGI("[Dump] 采集失败，无法落盘");
        return;
    }

    dump::Style style = dump::Style::Raw;
    if (req.style == 1) style = dump::Style::Grade;
    else if (req.style == 2) style = dump::Style::Walkable;

    // 文件名：DLL 同目录 dump_layerN_style.bmp
    char name[128];
    sprintf_s(name, "dump_layer%u_%s.bmp",
              (unsigned)req.layer, dump::StyleName(style));
    const std::string path = mcfg::detail::ModuleDir() + name;

    int32_t mx = req.markX;
    int32_t my = req.markY;
    if (req.markStart) {
        int32_t px = 0, py = 0;
        if (g_nav.ReadPlayerAnimPos(px, py)) {
            mx = px;
            my = py;
        }
    }

    if (dump::SaveCostBuffer(cost, path, style, mx, my)) {
        LOGI("[Dump] 已写出 %s（%u x %u, 标记 %d,%d）",
             path.c_str(), cost.Geo().WorldW(), cost.Geo().WorldH(), mx, my);
    }
    else {
        LOGI("[Dump] 写文件失败: %s", path.c_str());
    }

    // 顺便把"判墙结果"也存一张，用来对照判墙是不是过严
    // 回执：Plug 侧靠它知道写完了
    if (g_PipeServer && g_PipeServer->IsConnected()) {
        g_PipeServer->Reply(route, MT_Ack, nullptr, 0);
    }
}

// ------------------------------------------------------------
// 走路 call 槽位扫描（诊断，一次性）
//
// 背景：游戏更新后 kActionWalkVtblOff 失效 —— 解出来的入口不是走路函数，
// 调用返回成功但角色纹丝不动。静态分析只能给出 0x1A0/0x1A8 这类候选，
// 实测都不对，所以只能让游戏自己挨个试。
//
// 判据：调一次 → 等 settleMs → 重读玩家坐标，位移超过 2 像素就算命中。
// 一次只试一个槽位，命中立刻停。
// ------------------------------------------------------------
static void HandleWalkScan(const WalkScanReq& req, const managed::Route& route)
{
    WalkScanAck ack;
    ack.argMode = req.argMode;
    ack.objMode = req.objMode;

    int32_t px = 0, py = 0;
    if (!g_nav.ReadPlayerAnimPos(px, py)) {
        LOGI("[Scan] 读不到玩家坐标，放弃扫描");
        return;
    }
    ack.fromX = px;
    ack.fromY = py;

    // 目标点：默认在玩家右侧 3 格，避免走太远
    int32_t tx = req.tx;
    int32_t ty = req.ty;
    if (tx == 0 && ty == 0) {
        tx = px + 69;
        ty = py + 23;
    }

    // 选对象：显式地址优先，其次按 objMode 从玩家身上取组件，最后才是全局链
    uint64_t obj = 0;
    if (req.objAddr != 0) {
        obj = req.objAddr;
    }
    else if (req.objMode == 1 || req.objMode == 2) {
        const uint64_t player = g_nav.DiagPlayerEntity();
        obj = g_nav.DiagComponent(player, (req.objMode == 1) ? "Actor" : "Pathfinding");
    }
    else {
        obj = g_nav.Walker().ActionSystem();
    }

    if (!obj) {
        LOGI("[Scan] 取不到目标对象（objMode=%u）", (unsigned)req.objMode);
        return;
    }

    LOGI("[Scan] 开始：objMode=%u obj=0x%llX argMode=%u 槽位 0x%X~0x%X 目标(%d,%d) 从(%d,%d)",
         (unsigned)req.objMode, (unsigned long long)obj, (unsigned)req.argMode,
         req.slotStart, req.slotEnd, tx, ty, px, py);

    int tested = 0;
    uint64_t vtbl = g_nav.Walker().SafeRead(obj);

    for (uint32_t slot = req.slotStart; slot < req.slotEnd && vtbl; slot += 8) {
        const uint64_t entry = g_nav.Walker().SafeRead(vtbl + slot);

        // 只试"函数起点"，跳过数据槽和函数中段（省钱也降风险）
        if (!g_nav.Walker().LooksLikeFuncStart(entry)) {
            continue;
        }

        ++tested;

        // ★ 调用前先记一行：万一这个槽位把线程卡死（调用内部死循环），
        //   日志里最后一条就是嫌疑人，不至于什么都查不到。
        LOGI("[Scan]   试 slot 0x%03X entry=0x%llX ...", slot, (unsigned long long)entry);

        const navwalk::WalkResult wr = g_nav.Walker().ProbeSlot(obj, slot, tx, ty, req.argMode);
        Sleep(req.settleMs ? req.settleMs : 400);

        int32_t nx = 0, ny = 0;
        if (!g_nav.ReadPlayerAnimPos(nx, ny)) continue;

        const int dx = nx - px;
        const int dy = ny - py;

        LOGI("[Scan]   slot 0x%03X entry=0x%llX %s -> 位移(%d,%d)",
             slot, (unsigned long long)entry, navwalk::WalkResultName(wr), dx, dy);

        if (dx * dx + dy * dy >= 4) {   // 位移 >= 2 像素就算命中
            ack.slot = (int32_t)slot;
            ack.toX = nx;
            ack.toY = ny;
            LOGI("[Scan] *** 命中 slot=0x%X argMode=%u objMode=%u，位移(%d,%d) ***",
                 slot, (unsigned)req.argMode, (unsigned)req.objMode, dx, dy);
            break;
        }
    }

    ack.tested = tested;
    LOGI("[Scan] 结束：试了 %d 个槽位，结果 slot=%d", tested, (int)ack.slot);

    if (g_PipeServer && g_PipeServer->IsConnected()) {
        WalkScanAckMessage msg(ack);
        g_PipeServer->SendPackageTo(route, (void*)&msg);
    }
}

// ============================================================
// 用当前取数策略的地图几何更新世界边界
//
// ★ 为什么必须有这个函数：
//   走路器（ActionWalker）和技能器（SkillPusher）都有一层
//   "目标坐标越界就拒绝"的钳制。这个边界原来只在 DLL 加载时
//   从**当时那张图**算一次，之后再没更新。
//   于是切到更大的图（2691x1863）后，Plug 算出的航点 x=2001 仍然
//   拿旧边界（2001x2139）去校验 —— 2001 落在 [0,2001) 之外一格，
//   直接被拒。GUI 上表现为"行走失败 rc=12"，日志里是
//   "[Walker] 目标 (2001,1127) 越界（界 2001 x 2139），拒绝"。
//
//   注意：坐标换算本身没错，错的是拿旧图的边界卡新图的坐标。
//
// 边界语义是**半开区间** [0, maxX) × [0, maxY)：
//   WorldBounds::Contains 用的是 `x < maxX`，
//   所以 maxX 直接取 WorldW()，不需要减 1。
// ============================================================
static void UpdateWorldBoundsFromSource()
{
    if (!g_source) {
        return;
    }

    const mapdata::Geometry& g = g_source->Geo();
    const int32_t maxX = (int32_t)g.WorldW();
    const int32_t maxY = (int32_t)g.WorldH();

    if (maxX <= 0 || maxY <= 0) {
        // 0 在 WorldBounds 里表示"不做钳制"，这里不能把有效边界改成 0
        LOGI("[Nav] 地图几何非法（%d x %d），保持原边界", maxX, maxY);
        return;
    }

    navwalk::WorldBounds b;
    b.maxX = maxX;
    b.maxY = maxY;

    {
        std::lock_guard<std::mutex> lock(g_navMutex);
        g_nav.Walker().SetWorldBounds(b);
    }
    g_skill.SetWorldBounds(maxX, maxY);

    LOGI("[Nav] 世界边界已更新: %d x %d（网格 %u x %u）",
         maxX, maxY, g.Grid().w, g.Grid().h);
}

// ------------------------------------------------------------
// 收到客户端消息（每连接独立工作线程，接收线程只做握手、心跳、入队）
// ------------------------------------------------------------
static void OnMessage(MessageType type, const void* data, size_t size, const managed::Route& route)
{
    if (!g_threadRunning.load() || !g_PipeServer || !g_PipeServer->IsRouteCurrent(route)) return;
    switch (type) {
    case MT_PacketCommand: {
        packet::Reply reply{};
        if (size == sizeof(packet::Request)) {
            packet::Request request{}; std::memcpy(&request, data, sizeof(request));
            std::lock_guard<std::mutex> lock(g_navMutex);
            if (g_threadRunning.load() && g_PipeServer->IsRouteCurrent(route)) {
                const bool sends = packet::Sends(request.operation);
                if (sends && !(request.options & packet::PreviewOnly) &&
                    (g_nav.Active() || g_idSkill.Active() || g_skillInFlight.load())) {
                    reply.result = packet::Result::Busy;
                    strcpy_s(reply.detail, "导航或技能操作正在执行，请停止后再发送封包");
                } else reply = packetservice::Handle(request, route);
            }
        }
        g_PipeServer->Reply(route, MT_PacketReply, &reply, sizeof(reply), true);
        break;
    }
    case MT_ComboCast: {
        combo::CastReply reply{};
        if (size == sizeof(combo::CastRequest)) {
            combo::CastRequest request{};
            std::memcpy(&request, data, sizeof(request));
            std::lock_guard<std::mutex> lock(g_navMutex);
            if (!g_PipeServer->IsRouteCurrent(route)) break;
            if (request.operation != combo::Operation::Release && g_skillInFlight.load()) {
                reply.token = request.token; reply.result = combo::Result::Busy;
            } else reply = g_idSkill.Handle(request, route);
            LOGI("[Combo] op=%u token=%llu id=0x%08X hold=%u result=%u invoked=%u %s",
                unsigned(request.operation), request.token, request.skillId, request.chargeMs,
                unsigned(reply.result), reply.invoked, reply.detail);
        }
        g_PipeServer->Reply(route, MT_ComboCastAck, &reply, sizeof(reply), true);
        break;
    }
    case MT_PositionReq: {
        NavStatus st{}; bool valid = false;
        { std::lock_guard<std::mutex> lock(g_navMutex);
          valid = g_navReady.load() && g_nav.ReadPlayerAnimPos(st.x, st.y);
          st.state = NAV_IDLE; }
        if (valid) g_PipeServer->Reply(route, MT_PositionReply, &st, sizeof(st));
        else { managed::ErrorReply error{managed::Error::Unavailable, type};
               g_PipeServer->Reply(route, MT_ManagedError, &error, sizeof(error), true); }
        break;
    }
    case MT_MapReq: {
        if (g_scanning.exchange(true)) {
            LOGI("[扫图] 已在扫图中，忽略重复请求");
            break;
        }

        LOGI("[扫图] 收到扫图请求");

        if (!g_source) {
            LOGI("[扫图] 取数策略未就绪，尝试重建");
            std::string err;
            g_source = mapscan::MakeBestSource(err);
            if (!g_source) {
                LOGI("[扫图] 重建失败: %s", err.c_str());
                g_scanning = false;
                break;
            }
        }

        mapdata::CostBuffer cost;
        const uint8_t layer = 0;

        if (CaptureMap(layer, cost)) {
            const mapdata::MapFrame frame = mapdata::ToFrame(cost, layer);
            LOGI("[打包] %zu 字节 nibble，%u 行",
                 frame.PackedSize(), frame.GetMeta().height);
            SendFrameOverPipe(frame, route);

            // ★ 换图后必须重设世界边界。
            //
            //   走路器（ActionWalker）会拒绝"坐标超出边界"的目标，
            //   而这个边界原来只在 DLL 加载时从当时那张图算一次，
            //   之后再没更新过。于是切到更大的图（2691x1863）后，
            //   Plug 算出的航点 x=2001 仍然拿旧边界（2001x2139）去校验
            //   —— 2001 正好落在 [0,2001) 之外一格，被拒。
            //   表现就是 GUI 上"行走失败 rc=12"，Monitor 日志里
            //   "[Walker] 目标 (2001,1127) 越界（界 2001 x 2139），拒绝"。
            //
            //   坐标换算没算错：x 是 2001 说明格中心落在最后一格边缘，
            //   真正的问题是拿旧图的边界去卡新图的坐标。
            UpdateWorldBoundsFromSource();
        }
        else {
            LOGI("[扫图] 采集失败");
        }

        g_scanning = false;
        break;
    }

    // ---- 整条路径下发（GPS 重规划模式的主力通道）----
    case MT_NavBatch: {
        if (size < (size_t)NAV_BATCH_HDR_LEN) {
            LOGI("[Nav] MT_NavBatch 过短: %zu", size);
            break;
        }
        NavBatchHeader h{};
        memcpy(&h, data, sizeof(h));

        const size_t bodyLen = size - NAV_BATCH_HDR_LEN;
        const uint32_t count = (uint32_t)(bodyLen / sizeof(NavBatchChunk));
        const NavBatchChunk* pts =
            (const NavBatchChunk*)((const uint8_t*)data + NAV_BATCH_HDR_LEN);

        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(g_navMutex);
            if (!g_navReady.load()) {
                LOGI("[Nav] 导航未就绪，丢弃路径 seq=%u", h.seq);
                break;
            }
            ok = g_nav.FeedPathChunk(h.seq, h.total, h.baseIdx,
                                     h.tolerance, (h.flags & 1u) != 0,
                                     pts, count);
        }
        if (!ok) {
            LOGI("[Nav] 路径包被拒 seq=%u base=%u", h.seq, h.baseIdx);
        }
        break;
    }

    case MT_NavCfg: {
        if (size < sizeof(NavCfg)) {
            LOGI("[Nav] MT_NavCfg 过短: %zu", size);
            break;
        }
        NavCfg c{};
        memcpy(&c, data, sizeof(c));
        {
            std::lock_guard<std::mutex> lock(g_navMutex);
            g_nav.SetConfig(c);
            // ★ 巡航参数变了，**不该**顺手改熔断参数（调用频率上限）。
            //
            //   这里原来是：
            //     SetMinIntervalMs(60); SetRateLimit(10, 20000);
            //   而 NavDriver::Initialize 已经按 kDefault*(60/15/40000) 设过一遍。
            //   于是只要 Plug 发一条 MT_NavCfg（每次导航开头必发），
            //   熔断上限就被静默从 15/40000 降成 10/20000 —— 两处数字
            //   不一致，且谁后跑谁生效，改参数等于白改。
            //
            //   现在熔断只在 NavDriver::Initialize 里设，用的是
            //   navwalk::ActionWalker 里的同一组常量；这条分支只管巡航。
        }
        LOGI("[Nav] 参数更新: arrive=%d skipNear=%d stuckEps=%d "
             "maxMsWp=%u settle=%u reissue=%u noProg=%u maxReissue=%u "
             "backOff=%u flags=0x%02X",
             (int)c.arrive, (int)c.skipNear, (int)c.stuckEps,
             (unsigned)c.maxMsWaypoint, (unsigned)c.settleMs,
             (unsigned)c.reissueMs, (unsigned)c.noProgressMs,
             (unsigned)c.maxReissue, (unsigned)c.backOff, (unsigned)c.flags);

        // 回显生效值，Plug 侧可以直接打印出来核对
        if (g_PipeServer && g_PipeServer->IsConnected()) {
            NavCfg echo;
            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                echo = g_nav.GetConfigWire();
            }
            NavCfgMessage msg(echo);
            g_PipeServer->SendPackageTo(route, (void*)&msg);
        }
        break;
    }

    case MT_ScanBmp: {
        if (size < sizeof(ScanBmpReq)) break;
        ScanBmpReq req{};
        memcpy(&req, data, sizeof(req));
        HandleScanBmp(req, route);
        break;
    }




    case MT_WalkScan: {
        if (size < sizeof(WalkScanReq)) break;
        WalkScanReq req{};
        memcpy(&req, data, sizeof(req));

        // ★ 必须甩到独立线程跑，不能留在管道读线程里。
        //
        //   HandleWalkScan 内部每个槽位都要 Sleep(settleMs) 再复读坐标，
        //   一次扫描至少几百毫秒、扫多个槽位就是好几秒。原来它直接在
        //   OnMessage 里跑，而 OnMessage 是读线程的回调 —— 整条管道会
        //   在这几秒里完全停摆，GUI 那边所有请求(坐标/地图/导航)全部超时。
        //
        // 用独立标志 g_walkScanning：扫描不允许并发，第二次直接拒掉，
        // 但不能和扫图共用 g_scanning（见它的注释）。
        if (g_walkScanning.exchange(true)) {
            LOGI("[Scan] 已有走路扫描在进行，忽略重复请求");
            break;
        }
        [req, route]() {

            // ★ 用 RAII 保证无论从哪条 return 退出，标志都会复位。
            //   HandleWalkScan 里有多处提前 return（读不到坐标、取不到对象），
            //   如果只在调用点写 false，那些提前返回会让标志位永久卡在 true，
            //   后续所有扫描请求都会被当成"重复请求"拒掉。
            struct ScanGuard {
                ~ScanGuard() { g_walkScanning = false; }
            } guard;
            HandleWalkScan(req, route);
        }();
        break;
    }

    // ---- 技能释放（新战斗模块）----
    //
    // 为什么也甩到独立线程：Cast 内部有熔断 Sleep（最小间隔 60ms）和
    // SEH 兜底调用，虽然都不长，但管道读线程一旦被占住，坐标上报就会
    // 断档，GUI 上的角色点会卡。甩出去最省事。
    //
    // 用 g_navMutex 保护：技能 call 与走路 call 共用同一个动作系统对象，
    // 同时调会互相打断（往同一个动作队列塞东西）。
    case MT_SkillCast: {
        if (size < sizeof(SkillCastReq)) {
            LOGI("[Skill] MT_SkillCast 过短: %zu", size);
            break;
        }
        SkillCastReq req{};
        memcpy(&req, data, sizeof(req));

        if (!g_skillReady.load()) {
            LOGI("[Skill] 技能器未就绪，丢弃请求");
            SkillAck ack{};
            ack.result = (int32_t)navskill::SkillResult::NoActionSystem;
            if (g_PipeServer && g_PipeServer->IsConnected()) {
                SkillAckMessage msg(ack);
                g_PipeServer->SendPackageTo(route, (void*)&msg);
            }
            break;
        }

        [req, route]() {
            // ============================================================
            // 安全闸门（三道，顺序不能换）
            //
            // ★ 为什么需要（实测两次操作暴露的规律）：
            //     1) 静止时放技能 -> 调用"成功"(ret=2) 但游戏毫无反应
            //     2) 寻路刚结束 2.5 秒放技能 -> 游戏卡死
            //   根因：走路 call 和技能 call 共用同一个动作系统 + 同一个动作队列
            //   （日志里两边打印的 actionSys 完全相同）。导航还在跑、或刚停下
            //   动作还没消化完时插技能，就会打架。
            //
            // ★ 注意这里**绝不能**持有 g_navMutex：
            //   SkillSafeWindow 内部会 lock(g_navMutex)，同一线程重复加锁 =
            //   死锁 -> 占住锁 -> NavDriver::Tick 卡死 -> 游戏闪退。
            //   （曾经就是因为桥接函数里又加了一次锁，点一次释放技能直接闪退。）
            // ============================================================
            if (g_skillDisabled.load()) {
                LOGI("[Skill] 拒绝释放：技能功能已被看门狗停用（重新注入 Monitor 可恢复）");
                return;
            }
            if (g_skillInFlight.load()) {
                LOGI("[Skill] 拒绝释放：上一次调用尚未返回");
                return;
            }
            {
                // hygiene bit3 仅供用户主动发起的边走边放诊断；
                // 常规单次施法与命令行诊断仍走严格安全闸。
                const bool allowDuringMotionTest = (req.hygiene & 0x08) != 0;

                std::string why;
                if (!SkillSafeWindow(why, allowDuringMotionTest)) {
                    LOGI("[Skill] 拒绝释放%s：%s",
                         allowDuringMotionTest ? "（边走边放测试）" : "", why.c_str());
                    if (g_PipeServer && g_PipeServer->IsConnected()) {
                        SkillAck ack{};
                        ack.result = (int32_t)navskill::SkillResult::RateLimited;
                        SkillAckMessage msg(ack);
                        g_PipeServer->SendPackageTo(route, (void*)&msg);
                    }
                    return;
                }
                if (allowDuringMotionTest) {
                    LOGI("[Skill] 边走边放测试通道放行（距上次位移 %u ms）",
                         g_nav.LastStillMs());
                }
            }

            g_skillInFlight = true;
            g_skillStartTick = GetTickCount64();

            // RAII：无论从哪条 return 退出，标志都复位
            struct SkillGuard {
                ~SkillGuard() { SkillMarkDone(); }
            } skillGuard;


            navskill::SkillCallArgs args;
            args.skillSlot = (uint32_t)req.slot;
            args.r8        = req.r8;
            args.stack28   = req.stack28;
            args.stack30   = req.stack30;
            args.posX      = req.posX;
            args.posY      = req.posY;

            // ★ 坐标处理交给 SkillPusher 的 posMode：
            //   posMode=1（默认）= 角色前方，实时读玩家坐标自己算。
            //   这里只透传 req 里的值，不再做"0 就取玩家位置"的转换 ——
            //   那样会用缓存坐标，实测算到角色身后。
            args.posMode = req.posMode;
            args.aheadDistance = req.aheadDistance;
            args.rdxMode   = req.rdxMode;
            args.r9Mode    = req.r9Mode;
            args.dryRun    = (req.dryRun != 0);
            args.stopAfterMs = req.stopAfterMs;
            // ★ 会话卫生位：bit0 状态位写回 / bit1 引用计数还原 / bit2 深度校验。
            //   这就是"第二次不再崩"的开关；传 0 可退回旧行为做 A/B 对比。
            args.hygiene   = req.hygiene;

            navskill::SkillCallReport rep{};
            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                if (g_idSkill.Active()) rep.result = navskill::SkillResult::RateLimited;
                else g_skill.Cast(args, rep);
            }

            // ============================================================
            // ★ 停止施法必须在**释放 g_navMutex 之后**做。
            //
            //   原来这段在 SkillPusher::Cast 内部，等于持着 g_navMutex
            //   Sleep(300ms) 以上再调 StopSkill：NavDriver::Tick 被一起冻住，
            //   导航状态机停摆、位置坐标断档，长 Sleep 还会让游戏主线程
            //   看起来"卡死"。现在拆成锁外独立步骤。
            //
            //   为什么停止不能省：0x18C940 只是把施法压进动作队列，
            //   角色会继续播完整个动作（几百毫秒到几秒）。想"放一次就停"
            //   必须再调 0x211F0A0 打断。
            // ============================================================
            if (req.stopAfterMs > 0 &&
                rep.result == navskill::SkillResult::Success &&
                req.dryRun == 0) {
                LOGI("[Skill] 释放完成，%u ms 后调用停止 call", req.stopAfterMs);
                Sleep(req.stopAfterMs);
                bool stopOk = false;
                {
                    std::lock_guard<std::mutex> lock(g_navMutex);
                    stopOk = g_skill.StopSkill();
                }
                LOGI("[Skill] 停止 call 结果: %s", stopOk ? "已调用" : "未调用/失败");
            }

            LOGI("[Skill] 结果=%s ret=%d rcx=0x%llX rdx=0x%llX(A=0x%llX) "
                 "r9=0x%llX _Ptr=0x%llX ref=%u pos=%d,%d",
                 navskill::SkillResultName(rep.result), rep.retValue,
                 (unsigned long long)rep.actionSys,
                 (unsigned long long)rep.mapObject,
                 (unsigned long long)rep.mapObjectAdd,
                 (unsigned long long)rep.skillPtrAddr,
                 (unsigned long long)rep.skillPtr,
                 rep.skillRefCnt, rep.posX, rep.posY);

            // ★ 预算耗尽 / 前置校验失败：这类结果不是"这次没放出来"，
            //   而是"别再试了"。直接把技能功能停用，避免用户反复点。
            if (rep.result == navskill::SkillResult::CastBudgetExhausted) {
                g_skillDisabled = true;
                LOGI("[Skill] *** 本次注入的 Cast 额度已用完（%llu 次），技能功能停用。"
                     "重新注入 Monitor 可重置额度。",
                     (unsigned long long)g_skill.CastCount());
            }
            else if (rep.result == navskill::SkillResult::PreflightFailed) {
                LOGI("[Skill] 前置校验没过（技能槽可能还没被游戏激活）。"
                     "先在游戏里手动放一次该技能，再试。");
            }

            // 注：ret 值不可靠（实测 2 / 3 / 大负数都出现过，同一状态可能返回不同值），
            //     所以这里不再用它做成败判定，只记录。成败靠 stateBitBefore/After 对比。

            // ============================================================
            // ============================================================
            // ★★★ 成败判定：用 ret 是不是"小数"，不用状态位 ★★★
            //
            //   实测数据（用户确认过第一次真的放出技能）：
            //     第一次 ret=2               -> 技能真的放出来了
            //     第二次 ret=-1039727408     -> 游戏卡死
            //
            //   规律：ret 是小数（2/3）表示正常执行；
            //         出现大数值（正负都算）表示游戏内部状态已坏，
            //         再调一次必卡死。
            //
            //   ★ 之前用 [r14+0xE8] 状态位判定是错的：
            //     那个字段在成功时也可能一直是 0（实测两次都是 0->0），
            //     导致"成功了却判成失败"，白白触发了熔断。
            // ============================================================
            if (rep.result != navskill::SkillResult::Success) {
                // 没真正调用（预算耗尽 / 前置校验 / 忙 / 熔断）：
                // retValue 此时是默认 -1，**不能**拿它当"未执行"的证据。
                LOGI("[Skill] 判定：未调用（result=%s），不计入失败次数",
                     navskill::SkillResultName(rep.result));
            }
            else {
                // 小数返回值 = 正常执行；大数值 = 游戏状态还没恢复（间隔太短）
                //
                // ★ 不再"永久停用"：那个大数值只是本次状态未恢复，
                //   等够了时间下次就能成功。
                //   改成"计一次失败，下次仍允许尝试"。
                const bool ok = (rep.retValue >= 0 && rep.retValue <= 16);
                if (ok) {
                    g_skillFailStreak = 0;
                    LOGI("[Skill] 判定：成功（ret=%d 是正常返回值）", rep.retValue);
                }
                else {
                    ++g_skillFailStreak;
                    LOGI("[Skill] 判定：本次未执行（ret=%d 是大数值，说明状态还没恢复；"
                         "间隔太短，等够时间再试。本秒累计失败 %d 次）",
                         rep.retValue, (int)g_skillFailStreak);
                }
            }
            if (g_PipeServer && g_PipeServer->IsConnected()) {
                SkillAck ack{};
                ack.result        = (int32_t)rep.result;
                ack.retValue      = rep.retValue;
                ack.actionSys     = rep.actionSys;
                ack.mapObject     = rep.mapObject;
                ack.mapObjectA    = rep.mapObjectAdd;
                ack.skillPtrAddr  = rep.skillPtrAddr;
                ack.skillPtr      = rep.skillPtr;
                ack.skillRep      = rep.skillRep;
                ack.skillRefCnt   = rep.skillRefCnt;
                ack.posX          = rep.posX;
                ack.posY          = rep.posY;
                ack.callCount     = (uint32_t)rep.callCount;

                // 会话卫生观测（Plug 侧会直接打印，用来确认"已配平"）
                ack.usesBefore    = rep.usesBefore;
                ack.usesAfter     = rep.usesAfter;
                ack.weakBefore    = rep.weakBefore;
                ack.weakAfter     = rep.weakAfter;
                ack.stateRestored = rep.stateRestored ? 1 : 0;
                ack.hygiene       = rep.hygieneUsed;
                ack.budgetLeft    = rep.budgetLeft;

                SkillAckMessage msg(ack);
                g_PipeServer->SendPackageTo(route, (void*)&msg);
            }
        }();
        break;
    }

    // ---- 只停止施法（不释放）----
    //
    // 为什么和 MT_SkillCast 分开：
    //   释放 call（0x18C940）状态依赖极强，多数时候空转，反复调会让游戏
    //   内存从 9.5GB 涨到 20GB 然后崩溃（实测三次）。
    //   而停止 call（0x211F0A0）结构简单（只需 rcx）、每次都能生效。
    //
    //   所以停止功能独立成一条通道：用户自己按键放技能，我们只负责打断。
    //   不碰释放逻辑 = 不引入新的崩溃风险。
    case MT_SkillStop: {
        if (!g_skillReady.load()) {
            LOGI("[Skill] 停止请求：技能器未就绪");
            SkillStopAck ack{};
            ack.result = 1;
            if (g_PipeServer && g_PipeServer->IsConnected()) {
                SkillStopAckMessage msg(ack);
                g_PipeServer->SendPackageTo(route, (void*)&msg);
            }
            break;
        }

        [route]() {
            SkillStopAck ack{};

            // 先解 rcx 并快照，用于回执诊断
            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                ack.stopThis = g_skill.ResolveStopThis();
                const navskill::SkillPusher::StopSnapshot snap = g_skill.ReadStopSnapshot();
                ack.gateBefore = snap.gate669;
                ack.flagBefore = snap.flag181;
            }

            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                ok = g_skill.StopSkill();
            }

            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                const navskill::SkillPusher::StopSnapshot snap = g_skill.ReadStopSnapshot();
                ack.gateAfter = snap.gate669;
                ack.flagAfter = snap.flag181;
            }

            ack.result = ok ? 0 : 1;
            LOGI("[Skill] 停止请求完成：%s rcx=0x%llX gate 0x%02X->0x%02X",
                 ok ? "成功" : "失败", (unsigned long long)ack.stopThis,
                 ack.gateBefore, ack.gateAfter);

            if (g_PipeServer && g_PipeServer->IsConnected()) {
                SkillStopAckMessage msg(ack);
                g_PipeServer->SendPackageTo(route, (void*)&msg);
            }
        }();
        break;
    }

    case MT_Text:
        LOGI("[收] 文本: %.*s", (int)size, (const char*)data);
        break;

    // ---- 键盘驱动（WASD）老路径 ----
    case MT_NavKeyGo: {
        if (size < sizeof(NavGo)) {
            LOGI("[Key] MT_NavKeyGo 长度不足: %zu", size);
            break;
        }
        NavGo go{};
        memcpy(&go, data, sizeof(NavGo));
        HandleKeyGo(go);
        break;
    }
    case MT_NavKeyStop:
        HandleKeyStop();
        break;

    case MT_NavGo: {
        if (size < sizeof(NavGo)) {
            LOGI("[Nav] MT_NavGo 长度不足: %zu", size);
            break;
        }
        NavGo go{};
        memcpy(&go, data, sizeof(NavGo));

        std::lock_guard<std::mutex> lock(g_navMutex);
        if (!g_navReady.load()) {
            LOGI("[Nav] 导航未就绪，忽略目标 (%d,%d)", go.x, go.y);
            break;
        }
        g_nav.EnqueueGoal(go.x, go.y, go.tolerance);
        break;
    }
    case MT_NavStop: {
        std::lock_guard<std::mutex> lock(g_navMutex);
        if (g_navReady.load()) {
            g_nav.Stop();
        }
        break;
    }
    case MT_WalkCall: {
        if (size < sizeof(WalkCallReq)) {
            LOGI("[WalkCall] 长度不足: %zu", size);
            break;
        }
        WalkCallReq req{};
        memcpy(&req, data, sizeof(req));

        if (!g_navReady.load()) {
            WalkCallAck ack{};
            ack.ok = 1;
            ack.targetX = req.x;
            ack.targetY = req.y;
            LOGI("[WalkCall][nav=%04X call=%u] 导航未就绪，拒绝 target=(%d,%d)",
                 req.traceRunId, req.traceCallId, req.x, req.y);
            if (g_PipeServer && g_PipeServer->IsConnected()) {
                WalkCallAckMessage msg(ack);
                g_PipeServer->SendPackageTo(route, (void*)&msg);
            }
            break;
        }

        // 甩到独立线程：WalkTo 内部可能有熔断 Sleep，不能在读线程里阻塞，
        // 否则这条连接上后续的消息（坐标上报等）全被堵住。
        [route, req]() {
            WalkCallAck ack{};
            ack.targetX = req.x;
            ack.targetY = req.y;

            // ---- 调用前坐标：单独保留，不能被调用后的坐标覆盖 ----
            int32_t beforeX = 0, beforeY = 0;
            bool haveBefore = false;
            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                haveBefore = g_nav.ReadPlayerAnimPos(beforeX, beforeY);
                if (haveBefore) {
                    const double dx = (double)req.x - (double)beforeX;
                    const double dy = (double)req.y - (double)beforeY;
                    ack.distBefore = (int32_t)std::lround(std::sqrt(dx * dx + dy * dy));
                }
            }

            // ---- 裸调一次走路 call ----
            const uint64_t t0 = GetTickCount64();
            navwalk::WalkResult wr = navwalk::WalkResult::NoActionSystem;
            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                if (!g_threadRunning.load() || !g_PipeServer->IsRouteCurrent(route)) return;
                wr = g_nav.Walker().WalkTo(req.x, req.y);
            }
            ack.elapsedMs = (uint32_t)(GetTickCount64() - t0);
            ack.result = (int32_t)wr;
            ack.ok = (wr == navwalk::WalkResult::Success) ? 0 : 2;

            // ---- 调用后坐标（立刻读一次，不做任何等待）----
            int32_t afterX = 0, afterY = 0;
            bool haveAfter = false;
            {
                std::lock_guard<std::mutex> lock(g_navMutex);
                haveAfter = g_nav.ReadPlayerAnimPos(afterX, afterY);
                if (haveAfter) {
                    const double dx = (double)req.x - (double)afterX;
                    const double dy = (double)req.y - (double)afterY;
                    ack.distAfter = (int32_t)std::lround(std::sqrt(dx * dx + dy * dy));
                    ack.playerX = afterX;
                    ack.playerY = afterY;
                }
            }

            LOGI("[WalkCall][nav=%04X call=%u] target=(%d,%d) result=%s ok=%d elapsed_ms=%u before=%s(%d,%d) after=%s(%d,%d) dist=%ld->%ld",
                 req.traceRunId, req.traceCallId, req.x, req.y, navwalk::WalkResultName(wr), ack.ok,
                 (unsigned)ack.elapsedMs, haveBefore ? "valid" : "unavailable", beforeX, beforeY,
                 haveAfter ? "valid" : "unavailable", afterX, afterY,
                 (long)ack.distBefore, (long)ack.distAfter);

            if (g_PipeServer && g_PipeServer->IsConnected()) {
                WalkCallAckMessage msg(ack);
                g_PipeServer->SendPackageTo(route, (void*)&msg);
            }
        }();
        break;
    }

    case MT_Exit:
        LOGI("[收] 退出指令");
        break;

    default:
        LOGI("[收] 未知消息类型: %d 长度: %zu", (int)type, size);
        break;
    }
}

// ------------------------------------------------------------
// 启动线程：选取数策略 + 初始化导航 + 建管道 + 主循环
// ------------------------------------------------------------
static DWORD WINAPI StartThread(LPVOID)
{
    std::string err;
    g_source = mapscan::MakeBestSource(err);

    if (!g_source) {
        // 策略失败不阻止管道创建：客户端连上来后可以重试，
        // 也让 Plug.exe map 看到"已连接但采集失败"，而不是干等。
        LOGI("[启动] 没有可用的取数策略: %s", err.c_str());
        LOGI("[启动] 仍将创建管道，收到请求时会重试");

        LOGI("[启动] 候选策略:");
        for (const char* const* p = mapscan::AvailableSourceNames(); p && *p; ++p) {
            LOGI("[启动]   - %s", *p);
        }
    }
    else {
        const mapdata::Geometry& g = g_source->Geo();
        LOGI("[启动] 策略 '%s' 就绪：网格 %u x %u，世界 %u x %u，可用层 0~%u",
             g_source->Name(), g.Grid().w, g.Grid().h, g.WorldW(), g.WorldH(),
             (unsigned)(g_source->UsableLayerCount() - 1));
        LOGI("[启动] %s", g_source->Describe().c_str());
    }

    // ---- 初始化导航（走路 call）----
    // 世界坐标上界取地图几何；拿不到地图时用宽松上限，靠走路器自身的
    // 坐标钳制兜底。
    navwalk::WorldBounds bounds;
    if (g_source) {
        const mapdata::Geometry& g = g_source->Geo();
        bounds.maxX = (int32_t)g.WorldW();
        bounds.maxY = (int32_t)g.WorldH();
    }
    else {
        bounds.maxX = 0;   // 0 = 不做边界钳制
        bounds.maxY = 0;
    }

    {
        std::string nerr;
        uint64_t moduleBase = (uint64_t)GetModuleHandleA(g_cfg.gameModuleName.c_str());
        if (!moduleBase) {
            LOGI("[Nav] 取不到模块 %s 的基址，导航不可用", g_cfg.gameModuleName.c_str());
        }
        else if (!g_nav.Initialize(moduleBase, bounds, nerr)) {
            LOGI("[Nav] 初始化失败: %s", nerr.c_str());
        }
        else {
            // SendInput 前要把游戏切到前台，窗口标题来自配置
            navdrive::SetNudgeWindowTitle(g_cfg.gameWindowTitle.c_str());
            // 动作系统对象可能在切图后才出现，这里多试几次
            for (int i = 0; i < 3; ++i) {
                if (g_nav.Walker().RefreshActionSystem() == navwalk::WalkResult::Success) {
                    break;
                }
                Sleep(200);
            }
            g_navReady = true;
            LOGI("[Nav] 导航已就绪：世界边界 %d x %d", bounds.maxX, bounds.maxY);
            // ---- 初始化技能器（新战斗模块）----
            // 与走路 call 共用同一个 moduleBase / 世界边界，
            // 但入口是固定 RVA，独立校验（0x18C940 必须是函数起点）。
            {
                std::string serr;
                if (g_skill.Initialize(moduleBase, serr)) {
                    g_skill.SetWorldBounds(bounds.maxX, bounds.maxY);
                    g_skill.SetPosReader(&SkillPosBridge);   // 实时读玩家坐标

                    // ---- 从 config.ini 的 [skill] 段覆盖熔断参数 ----
                    // 这两个值的实测依据见 MonitorConfig.h 的说明：
                    // 旧版"5~6 秒必崩"很可能是引用计数被扣到 0 的假象，
                    // 计数修好之后间隔可以往下调，所以做成可配置而不是写死。
                    if (g_cfg.skillMinIntervalMs > 0) {
                        g_skill.SetMinIntervalMs((uint32_t)g_cfg.skillMinIntervalMs);
                    }
                    if (g_cfg.skillMaxCasts >= 0) {
                        g_skill.SetMaxCasts((uint32_t)g_cfg.skillMaxCasts);
                    }
                    LOGI("[Skill] 熔断来自配置：间隔=%ums 预算=%u次",
                         g_skill.MinIntervalMs(), g_skill.MaxCasts());

                    if (g_skill.RefreshActionSystem() == navskill::SkillResult::Success) {
                        LOGI("[Skill] 技能器已就绪：入口 RVA 0x%llX，动作系统 0x%llX",
                             (unsigned long long)kSkillCallRva,
                             (unsigned long long)g_skill.ActionSystem());
                    }
                    else {
                        LOGI("[Skill] 技能器初始化完成，动作系统对象尚未就绪（进图后自动重解）");
                    }
                    g_skillReady = true;
                }
                else {
                    LOGI("[Skill] 技能器初始化失败: %s", serr.c_str());
                }
            }
        }
    }

    if (!g_threadRunning.load()) return 0;
    g_PipeServer = new NamedPipeServer(mcfg::ToWide(g_cfg.pipeName), OnMessage);
    if (!g_PipeServer->Initialize()) {
        LOGI("[启动] 管道服务端初始化失败");
        return 1;
    }
    LOGI("[启动] 管道服务端已就绪，等待客户端...");

    // ---- 主循环：导航 tick 永远跑，管道只是"有客户端就顺手发" ----
    // 为什么不能把 while 条件写成 IsConnected()：
    //   CreatePipeAndWait() 是阻塞式的 —— 没客户端连接时主循环根本
    //   进不来，导航 tick 就永远不跑，Plug 连上后也收不到任何
    //   NavStatus，5 秒超时报"没收到玩家坐标"。
    //   所以主循环必须无条件推进导航，管道有连接时才发状态。
    //
    // ★ 退出闸门（游戏退出的教训）：
    //   DllMain PROCESS_DETACH 里把 g_threadRunning 置 false，
    //   循环立刻退出；每次迭代开头检查，退出前不再碰任何游戏内存。
    uint64_t lastIdleReport = 0;

    while (g_threadRunning.load()) {
        if (!g_PipeServer) {
            Sleep(50);
            continue;
        }

        NavStatus st{};
        bool changed = false;

        // 导航 tick（保护"对动作系统的一切调用"的同一把锁）
        {
            std::lock_guard<std::mutex> lock(g_navMutex);
            g_idSkill.Tick(!g_idSkill.Active() || g_PipeServer->IsRouteCurrent(g_idSkill.Owner()));
            packetservice::RecoveryTick([](const managed::Route& route) { return g_PipeServer->IsRouteCurrent(route); },
                                        g_skillInFlight.load());
            viewservice::Tick([](const managed::Route& route) { return g_PipeServer->IsRouteCurrent(route); });
            if (g_navReady.load()) {
                changed = g_nav.Tick(st);
            SkillWatchdog();   // 技能调用卡死自检（超时就永久停用技能）
            }
        }

        const bool connected = g_PipeServer->IsConnected();

        packetservice::Tick([](const managed::Route& route) { return g_PipeServer->IsRouteCurrent(route); },
            [](const packet::CaptureEvent& event) { g_PipeServer->Publish(MT_PacketCapture, &event, sizeof(event)); });

        // ★ 位置变化也要立刻上报。
        //   原来只在「导航状态变了」时才发，而走路过程中 state 一直是
        //   MOVING 不变，于是落到下面 1 秒一次的空闲分支 ——
        //   GUI 上的角色点最多 1 秒才动一次，看起来非常卡。
        static int32_t lastX = -999999, lastY = -999999;
        if (changed || st.x != lastX || st.y != lastY) {
            lastX = st.x;
            lastY = st.y;
            changed = true;
        }

        if (changed && connected && st.state != NAV_FAILED) {
            NavStatusMessage msg(st);
            g_PipeServer->PublishPackage((void*)&msg);
        }
        else if (connected) {
            // 空闲时也要定期上报（GUI 靠它刷新角色位置）。
            // 50ms 足够流畅，又不会把管道打满。
            const uint64_t now = GetTickCount64();
            if (now - lastIdleReport >= 50) {
                lastIdleReport = now;
                NavStatus idle{};
                {
                    std::lock_guard<std::mutex> lock(g_navMutex);
                    if (g_navReady.load()) {
                        int32_t px = 0, py = 0;
                        if (!g_nav.ReadPlayerAnimPos(px, py)) { Sleep(10); continue; }
                        idle.x = px;
                        idle.y = py;
                        idle.state = NAV_IDLE;
                        idle.remaining = g_nav.QueueDepth();
                    }
                }
                NavStatusMessage msg(idle);
                g_PipeServer->PublishPackage((void*)&msg);
            }
        }

        // 把 Positioned 地址同步给调试 hook（没注入 walkhook2 时是空操作）
        if (g_navReady.load()) {
            PushPosToHook(g_nav.PositionedAddr());
        }

        // 导航活跃时高频 tick，空闲时低频
        Sleep(changed ? 10 : 50);
    }

    return 0;
}


// ------------------------------------------------------------
// 键盘驱动线程：把角色走向指定坐标（WASD 老路径）
// ------------------------------------------------------------
static void KeyDriveThreadProc()
{
    std::string reason;
    int32_t tx = g_keyTargetX;
    int32_t ty = g_keyTargetY;

    // 读坐标的回调：直接问 NavDriver 要当前世界坐标
    // （+0x444 / +0x448 的 int32 本身就是世界像素，不做换算）
    auto readPos = [](int32_t& x, int32_t& y) {
        if (!g_nav.ReadPlayerAnimPos(x, y)) {
            x = 0;
            y = 0;
        }
    };

    const bool ok = g_keyDriver.DriveTo(tx, ty, readPos, reason);
    LOGI("[Key] 驱动结束：%s（目标 %d,%d）", reason.c_str(), tx, ty);

    g_keyRunning = false;
}

static void HandleKeyGo(const NavGo& go)
{
    // 首次使用时初始化（绑定窗口 + 切前台）。
    // 放在这里而不是 DllMain，是因为 DllMain 里调 SetForegroundWindow
    // 风险较高，而且窗口句柄此刻还不一定有效。
    //
    // ★ 窗口标题从配置取（g_cfg.gameWindowTitle），不再写死字符串：
    //   游戏改名 / 换区域，只要改 config.ini 的 [game] windowTitle，
    //   这里和 NavDriver 的 nudge 就会同时跟着变。
    static bool inited = false;
    if (!inited) {
        std::string err;
        if (!g_keyDriver.Initialize(nullptr, g_cfg.gameWindowTitle.c_str(), g_keyCfg, err)) {
            LOGI("[Key] 初始化失败: %s", err.c_str());
            return;
        }
        inited = true;
        LOGI("[Key] 驱动已初始化，窗口标题=%s", g_cfg.gameWindowTitle.c_str());
    }

    // 已有驱动在跑：先请求停
    if (g_keyRunning) {
        g_keyStopReq = true;
        for (int i = 0; i < 30 && g_keyRunning; ++i) {
            Sleep(50);
        }
        g_keyDriver.ReleaseAll();
    }

    g_keyTargetX = go.x;
    g_keyTargetY = go.y;
    g_keyStopReq = false;
    g_keyRunning = true;

    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        KeyDriveThreadProc();
        return 0;
    }, nullptr, 0, nullptr);
}

static void HandleKeyStop()
{
    g_keyStopReq = true;
    g_keyDriver.ReleaseAll();
    LOGI("[Key] 已请求停止");
}

// ------------------------------------------------------------
// 把 Positioned 组件地址推给 walkhook2（调试用的取样 hook）
// ------------------------------------------------------------
static void PushPosToHook(uint64_t positioned)
{
    static uint64_t lastPushed = 0;
    static uint64_t lastTry = 0;
    const uint64_t now = GetTickCount64();

    if (positioned == lastPushed && (now - lastTry) < 500) {
        return;
    }
    lastTry = now;

    HMODULE hook = GetModuleHandleA("walkhook2.dll");
    if (!hook) {
        return;
    }

    using Fn = void(__cdecl*)(uint64_t);
    static Fn fn = nullptr;
    if (!fn) {
        fn = (Fn)(void*)GetProcAddress(hook, "SetPosComp");
    }
    if (!fn) {
        return;
    }
    if (positioned != lastPushed) {
        fn(positioned);
        lastPushed = positioned;
    }
}

// Called outside the loader lock before unloading the module. Returns nonzero
// only after the navigation loop, all pipe workers, and queued work have stopped.
extern "C" __declspec(dllexport) DWORD WINAPI MonitorShutdown(LPVOID)
{
    std::lock_guard<std::mutex> shutdown(g_shutdownMutex);
    g_threadRunning=false;
    if(g_startThread) {
        if(WaitForSingleObject(g_startThread,30000)!=WAIT_OBJECT_0) return 0;
        CloseHandle(g_startThread);g_startThread=nullptr;
    }
    if(g_PipeServer){g_PipeServer->Uninitialize();delete g_PipeServer;g_PipeServer=nullptr;}
    if (!packetservice::Shutdown()) {
        LOGI("[Packet] hooks still active; module must remain loaded");
        return 0;
    }
    {
        std::lock_guard<std::mutex> nav(g_navMutex);
        g_idSkill.Cancel();
        g_navReady=false;g_skillReady=false;g_nav.Stop();
    }
    g_source.reset();
    LOGI("[Managed] orderly shutdown complete");
    return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID reserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);

        // ★ 顺序很重要：必须先算出 DLL 所在目录，再读配置。
        //
        //   ★★ 这里修过一个真 bug（2026-11-08）★★
        //   MonitorConfig::detail::ModuleDir() 用的是
        //       GetModuleFileNameA(ThisModule(), ...)
        //   而 ThisModule() 只有通过 SetModule(hModule) 才会被填上。
        //   原来只调了 SetModuleDir()（设的是另一个变量 g_dllDir），
        //   ThisModule() 一直是 nullptr —— 于是 ModuleDir() 拿到的其实是
        //   **宿主进程（游戏 exe）的目录**，程序会去游戏安装目录找 config.ini。
        //   症状：config.ini 明明在 DLL 同目录，日志却打印游戏目录路径，
        //         管道名 / 日志文件名等配置项全部退化成默认值。
        //   现在 SetModule + SetModuleDir 两个都设，两条路径都正确。
        mcfg::detail::SetModule(hModule);
        {
            char dllPath[MAX_PATH] = { 0 };
            GetModuleFileNameA(hModule, dllPath, MAX_PATH);
            std::string full(dllPath);
            const size_t slash = full.find_last_of("\\/");
            mcfg::detail::SetModuleDir(
                (slash == std::string::npos) ? std::string() : full.substr(0, slash + 1));
        }

        // 再读配置，DLL 日志文件名也要从配置里取
        mcfg::LoadFromFile(g_cfg);

        DllLog::Init(hModule, g_cfg.logFileName.c_str());
        LOGI("[Config] 配置文件路径 = %s", mcfg::ConfigPath().c_str());
        LOGI("[Config] packSwap = %d（已废弃，不参与计算；r8 仍是 (y<<32)|x 打包）",
             g_cfg.packSwap);
        LOGI("[DllMain] Monitor.dll 已加载 (PID=%lu)", (unsigned long)GetCurrentProcessId());
        g_threadRunning=true;
        g_startThread=CreateThread(nullptr,0,StartThread,nullptr,0,nullptr);
        break;
    }
    case DLL_PROCESS_DETACH: {
        // Process termination already stops other threads. Dynamic unload must
        // use MonitorShutdown first; no waits or joins under the loader lock.
        g_threadRunning=false;
        if(!reserved && g_PipeServer) {
            LOGI("[Managed] unload without MonitorShutdown is unsupported");
        }
        if(!reserved) DllLog::Close();
        break;
    }
    }
    return TRUE;
}
