#pragma once
// ============================================================
// UseCases.h —— 用例层：一个函数 = 一个完整用例
//
// 每个用例只负责「取数据 -> 返回结果」，不负责打印。
// 这样换输出格式（JSON / HTTP / UI 覆层）不用动算法。
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "Inventory.h"
#include "Surroundings.h"
#include "SkillSlots.h"
#include "PartyData.h"
#include "WorldArea.h"

#include "../Common/Protocal.h"
#include "../Common/PacketProtocol.h"


namespace usecase {

// 一次快照的全部结果
struct ScanResult {
    std::vector<poe::InventoryItem>  inventory;
    std::vector<poe::EntitySnapshot> surroundings;
};

// 背包遍历。返回 false 时 err 里是原因
bool ScanInventory(std::vector<poe::InventoryItem>& out, std::string& err);

// 当前角色技能槽，只读外部内存，不依赖 Monitor。
bool ScanSkillSlots(poe::SkillSlotsSnapshot& out, std::string& err);

// Party roster + member maps; read-only and independent of Monitor/nearby entities.
bool ScanParty(poe::PartySnapshot& out, std::string& err, const std::atomic_bool* stop = nullptr);

// Re-read membership, bind to the chosen Controller session, send once; never retry.
// True means a valid Monitor reply was received; inspect reply.result for the outcome.
// submitted is conservative on lost replies; optional pre-send validation runs after re-reading the roster.
bool TeleportToPartyMember(const poe::PartyMember& member, uint32_t expectedPid,
    uint64_t expectedSession, uint64_t expectedController, packet::Reply& reply,
    std::string& err, const std::atomic_bool* stop = nullptr, bool* submitted = nullptr,
    bool requireSameMap = false, const std::function<bool()>& validateBeforeSend = {});

// 周围实体遍历
bool ScanSurroundings(std::vector<poe::EntitySnapshot>& out, std::string& err);

struct RitualScanResult {
    poe::WorldAreaSnapshot area;
    std::vector<poe::EntitySnapshot> candidates;
    size_t visited = 0;
};
// Read the unfiltered tree, including unknown entity kinds; return positioned keyword candidates only.
bool ScanRitualCandidates(RitualScanResult& out, std::string& err,
                         const std::atomic_bool* stop = nullptr);

// Re-scan and bind a nearby RitualRuneInteractable to the expected map point,
// then submit one targeted packet. A lost reply is ambiguous and never retried.
bool InteractRitual(const poe::EntitySnapshot& interactable,
    const poe::EntitySnapshot& expectedObject, const poe::WorldAreaSnapshot& expectedArea,
    uint32_t expectedPid, uint64_t expectedSession, uint64_t expectedController,
    bool appendEnd, packet::Reply& reply, std::string& err,
    const std::atomic_bool* stop = nullptr, bool* submitted = nullptr,
    const std::function<bool()>& validateBeforeSend = {});

// One submission bound to the source map/player and current Controller.
// submitted remains true after a lost reply; observe the game before proceeding.
bool SummonPortal(const poe::WorldAreaSnapshot& source, uint64_t player, uint32_t expectedPid,
    uint64_t expectedSession, uint64_t expectedController, packet::Reply& reply, std::string& err,
    const std::atomic_bool* stop, bool* submitted, const std::function<bool()>& validateBeforeSend);
bool EnterHideoutPortal(const poe::EntitySnapshot& portal, const poe::WorldAreaSnapshot& source,
    uint64_t player, uint32_t expectedPid, uint64_t expectedSession, uint64_t expectedController,
    packet::Reply& reply, std::string& err, const std::atomic_bool* stop, bool* submitted,
    const std::function<bool()>& validateBeforeSend);

// 一次采集背包 + 周围（只初始化一次驱动）。
// 目前不做定时刷新：采集完就返回；以后要刷新在外面套循环即可。
bool ScanSnapshot(ScanResult& out, std::string& err);

// 地图客户端：连 Monitor.dll 收代价表并出图（阻塞直到收到完整地图）
//   showcaseName: "binary"（默认） / "grade" / "walkable"
int FetchMap(const std::string& showcaseName = "binary");

// 地图回放：从 .bin 文件还原并出图（不需要游戏、不需要管道）
//   binPath     : nibble 打包流 或 逐点字节流
//   gridW/gridH : 网格尺寸（世界像素 = 网格 * 0x17）
//   outPath     : 为空则用 config.ini 的 bmpPath
int ReplayMap(const std::string& binPath, uint32_t gridW, uint32_t gridH,
              const std::string& showcaseName, const std::string& outPath);

// 扫图落盘：请 Monitor 在注入端直接把当前代价表写成 BMP（诊断用）
//   style : 0=raw 1=grade 2=walkable
// 生成的文件在 DLL 同目录：dump_layer0_<style>.bmp / dump_walkmask.bmp
int DumpMapOnMonitor(uint8_t layer, uint8_t style, bool markPlayer);

// ------------------------------------------------------------
// 走路 call 槽位扫描（诊断用：游戏更新后重新定位 kActionWalkVtblOff）
//
// 让 Monitor 在游戏进程内，对「对象 × vtable 槽位 × 参数形态」逐个试调用，
// 用玩家坐标是否变化判断哪个才是真正的走路函数。
//
//   objMode  0=全局链对象  1=玩家 Actor 组件  2=玩家 Pathfinding 组件
//   argMode  0=rdx:0x400+r8打包  1=rdx/r8都传指针  2=rdx:0 r8打包  3=rdx打包 r8:0
//   slotStart/slotEnd  槽位偏移区间（按 8 递增）
//
// 返回 0 = 扫描跑完（命中槽位看日志）；1 = 管道连不上
int ScanWalkSlot(uint8_t objMode, uint8_t argMode,
                 uint32_t slotStart, uint32_t slotEnd,
                 int32_t tx, int32_t ty, uint32_t settleMs = 400,
                 uint64_t objAddr = 0);

// ------------------------------------------------------------
// 技能释放诊断（新战斗模块）
//
// 完整参数直通 Monitor，允许逐字段消融：一次只改一个参数，
// 看技能到底有没有放出去。dryRun=1 时只解链打日志、不动游戏。
//
// hygiene（会话卫生开关，位掩码）：
//   bit0 = 调用后把 [r14+0xE8] 状态位写回调用前的值
//   bit1 = 控制块引用计数增量还原
//   bit2 = 调用前深度校验（[[_Ptr]+0x520] 必须是模块内函数起点）
// 默认 0x03（前两条开）。传 0 退回旧行为，用于 A/B 对比"第二次崩不崩"。
//
// 返回 0 = 请求已发送（结果看 monitor_log.txt 的 [Skill] 行）
//      1 = 管道连不上
// ------------------------------------------------------------
int SkillProbe(uint32_t slot, uint64_t r8, int32_t posX, int32_t posY,
               uint8_t rdxMode, uint8_t r9Mode,
               uint64_t stack28, uint64_t stack30, bool dryRun,
               uint32_t stopAfterMs = 0,
               uint8_t posMode = 1, int32_t aheadDistance = 60,
               uint8_t hygiene = 0x03);

// 只读诊断：把技能 shared_ptr 静态链的每一级解出来打日志（不需要真实调用）
int SkillDump(uint32_t slot);

// ------------------------------------------------------------
// GUI 用：释放一次技能并返回 Monitor 的真实回执
//
// ★ 和 SkillProbe 的区别：SkillProbe 是"命令行诊断"，只往日志里打；
//   这个是给界面用的，要把结果（成败 / 冷却 / 额度）带回来给按钮显示，
//   否则用户点了按钮只会看到一条模糊的"失败"，不知道是管道断了、
//   在冷却中，还是额度用完了。
//
// 参数：
//   slot           技能栏下标（右键 = 2）
//   r8             0x400 左键/键盘/翻滚、0x401 右键技能
//   SkillCall 回执后立即发送 StopSkill，不做固定毫秒延时
//   out            回执；可为空
//   allowDuringMotionTest  true 时只用于手动边走边放诊断（hygiene bit3）；
//                          常规单次施法必须保持 false。
//
// 返回（这是**技能层**的结论，不是管道层）：
//   0  = 成功（Monitor 回 Ok，ret 是正常返回值）
//   1  = 管道连不上（Monitor.dll 注入了吗）
//   2  = Monitor 8 秒内没回执
//   3  = 被熔断/忙闸拒绝（RateLimited，等冷却再点）
//   4  = 额度用完了（CastBudgetExhausted，要重新注入）
//   5  = 调用崩了（Crashed，已 SEH 兜底）
//   6  = 其它失败（NoActionSystem / BadEntry / NoSkillPtr / PreflightFailed）
//   7  = SkillCall 成功，但立即 StopSkill 失败
// ------------------------------------------------------------
// aimX / aimY：释放坐标（世界像素）。
//   · 都给 >= 0 的值 = 直接用这个坐标（posMode=0）。
//   · 留默认 -1 = 沿用旧行为（posMode=1，Monitor 实时读玩家坐标算"前方
//     aheadDistance 像素"）。
int SkillCastOnce(uint32_t slot, uint64_t r8,
                  SkillAck* out = nullptr, bool allowDuringMotionTest = false,
                  int32_t aimX = -1, int32_t aimY = -1);

// 只停止施法（不释放技能）—— 走 0x211F0A0，返回 0 = 成功
int SkillStop();

// ------------------------------------------------------------
// 裸走路 call 标定（GUI 常驻测试功能）
//
// 只做两件事：真实调用一次走路 call（g_nav.Walker().WalkTo），
// 然后把调用前后的角色坐标 + 到目标的距离带回来。
//
// ★ 为什么不复用 MT_NavGo / Navigate：
//   NavGo 走 NavDriver 的路径状态机（m_pts/m_idx、skipNear 跳过身边点、
//   到位判定、无进展续推、卡住跳过），标定"一次 call 能走多远"时这些
//   判定会污染结果 —— 测出来的不是裸 call 的行为，是状态机 + call 的
//   混合行为。所以单开一条通道，不经过任何判定。
//
// 用法：给一个目标坐标，点一次，看返回的距离。连点会连发，不做去重。
//
// 返回：
//   0 = 调用成功（ac 里是坐标和距离）
//   1 = 管道连不上（Monitor.dll 注入了吗）
//   2 = 走机器未就绪（角色还没进场景 / 动作系统解链失败）
//   3 = 走路 call 本身失败（看 ac.result 和 monitor_log.txt 的 [WalkCall]）
//   4 = 3 秒内没收到回执
// ------------------------------------------------------------
int WalkCallOnce(int32_t x, int32_t y, WalkCallAck* ac = nullptr);

} // namespace usecase
