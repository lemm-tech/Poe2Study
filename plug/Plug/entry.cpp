// ============================================================
// entry.cpp —— Plug 统一入口
//
// 这里只做三件事：初始化配置/日志 -> 解析命令行 -> 调用例。
//
// 用法（默认模式现在就是 navigate —— 这个项目的用途就是自动寻路）：
//   Plug.exe                 = navigate
//   Plug.exe navigate [...]  自动寻路（GPS 重规划）
//   Plug.exe snapshot        背包 + 周围一次输出
//   Plug.exe inventory       只读背包
//   Plug.exe around          只读周围实体
//   Plug.exe map             地图客户端（收图出 BMP）
//   Plug.exe replay ...      地图回放（离线，不需要游戏）
//   Plug.exe dump            请 Monitor 在注入端把代价表存成 BMP
//   Plug.exe help            帮助
// ============================================================
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Inventory.h"
#include "Navigator.h"
#include "NamedPipeClie.h"
#include "Surroundings.h"
#include "UseCases.h"

#include "../Common/AppConfig.h"
#include "../Common/Logger.h"
#include "../Common/GameOffsets.h"

namespace {

void PrintUsage()
{
    POUT("===== Plug 入口 =====\n");
    POUT("用法: Plug.exe [模式] [选项]\n");
    POUT("  (无参数)   自动寻路（同 navigate）\n");
    POUT("  navigate   自动寻路：算路 + 驱动角色（需先注入 Monitor.dll）\n");
    POUT("  snapshot   背包 + 周围实体，一次输出\n");
    POUT("  inventory  只读背包\n");
    POUT("  skills     只读当前角色技能槽（ID / 内部标识 / 繁体名）\n");
    POUT("  around     只读周围实体（玩家/怪物/掉落物/传送门...）\n");
    POUT("  map        地图客户端：经命名管道接收地图数据并出图\n");
    POUT("  replay     地图回放：从 .bin 文件还原并出图（不需要游戏）\n");
    POUT("  dump       请 Monitor 在注入端把代价表存成 BMP（诊断用）\n");
    POUT("  walkscan   扫描走路 call 的 vtable 槽位（游戏更新后重新定位用）\n");
    POUT("  skill      释放技能诊断（新战斗模块）：-slot= -r8= -x= -y= -rdx= -r9= -dry\n");
    POUT("  skilldump  只解技能 shared_ptr 静态链，不打游戏（只读）\n");
    POUT("  help       显示本帮助\n");


    POUT("\nwalkscan 用法:\n");
    POUT("  Plug.exe walkscan [-obj=N] [-arg=N] [-from=0x198] [-to=0x1A0] [-objaddr=0x..] [-force]\n");
    POUT("  -obj=0  全局链对象（默认）  1 玩家 Actor 组件  2 玩家 Pathfinding 组件\n");
    POUT("  -arg=0  rdx:0x400 + r8 打包（默认）  1 rdx/r8 都传指针\n");
    POUT("          2 rdx:0 + r8 打包           3 rdx 打包 + r8:0\n");
    POUT("  -from=/-to=  槽位偏移区间，按 8 递增\n");
    POUT("               ★ 默认只试已验证的 0x198；超出这个范围需要 -force，\n");
    POUT("                 盲调未知虚函数会把游戏搞崩（实测发生过）\n");
    POUT("  命中槽位写在 monitor_log.txt 的 [Scan] 行里\n");


    POUT("\nskill 用法（释放技能诊断）:\n");
    POUT("  Plug.exe skill [-slot=2] [-r8=0x401] [-x=X] [-y=Y] [-dry]\n");
    POUT("                 [-rdx=0|1] [-r9=0|1] [-sp28=0] [-sp30=0x400]\n");
    POUT("  -slot  技能栏下标（右键 = 2）\n");
    POUT("  -r8    0x400 = 左键/键盘/翻滚，0x401 = 右键技能\n");
    POUT("  -x/-y  释放坐标（世界像素，0 = 用玩家当前位置）\n");
    POUT("  -rdx   0 = 地图对象本体(+0x8F8)  1 = 实测链末尾(+0x78)\n");
    POUT("  -r9    0 = 静态链  1 = 先走懒加载虚调用再读静态链\n");
    POUT("  -sp28  原反汇编里的 al 值（含义待定，默认 0）\n");
    POUT("  -sp30  原反汇编里的 r8d 值（默认 0x400）\n");
    POUT("  -dry   只解链打日志，不真正调用\n");
    POUT("  结果看 monitor_log.txt 的 [Skill] 行\n");
    POUT("\nnavigate 用法:\n");
    POUT("  -x= -y=    目标世界坐标；必须指定 x 和 y\n");
    POUT("  -dry       只算路 + 存图，不驱动角色\n");
    POUT("  -step=N    下发距离上限（世界坐标距离，默认 126）\n");
    POUT("  -replan=N  规划失败后的重试间隔（毫秒，默认 800）\n");
    POUT("  -tol=N     到达半径（世界坐标距离，默认 13）\n");
    POUT("  -stall=N   多少秒没有实际位移算卡住（默认 30）\n");
    POUT("  -walkto=N  整条路超时秒数（默认 600）\n");
    POUT("  -bmp=PATH  代价图/路径图落盘路径\n");

    POUT("\nreplay 用法:\n");
    POUT("  Plug.exe replay <file.bin> <gridW> <gridH> [-out=x.bmp] [-showcase=grade]\n");
    POUT("  例: Plug.exe replay recievedata.bin 72 72 -out=replay.bmp\n");

    POUT("\ndump 用法:\n");
    POUT("  Plug.exe dump [-layer=N] [-style=raw|grade|walkable]\n");
    POUT("  文件落在 Monitor.dll 同目录：dump_layerN_style.bmp\n");

    POUT("\n着色策略（-showcase=）：binary / grade / walkable\n");
    POUT("配置: 同目录 config.ini，或环境变量 POE2_* 覆盖\n");
}

// 从形如 "-key=value" 的参数里取值；没有则返回空串
std::string GetOption(int argc, char* argv[], const char* key)
{
    const std::string prefix = std::string("-") + key + "=";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind(prefix, 0) == 0) {
            return a.substr(prefix.size());
        }
    }
    return std::string();
}

// 形如 "-key" 的开关
bool HasFlag(int argc, char* argv[], const char* key)
{
    const std::string want = std::string("-") + key;
    for (int i = 2; i < argc; ++i) {
        if (want == argv[i]) return true;
    }
    return false;
}

void PrintInventory(const std::vector<poe::InventoryItem>& items)
{
    POUT("===== Inventory =====\n");
    for (const auto& item : items) {
        POUT("key:0x%X  entity:0x%llX  品级:%s  name:",
            (unsigned)item.key, (unsigned long long)item.entity,
            poe::GradeName(item.grade));
        if (!item.prefix.empty()) {
            POUT("%s", item.prefix.c_str());
        }
        POUT("%s *%u\n", item.name.c_str(), item.num);
    }
    POUT("===================\n");
    POUT("Total: %zu items\n", items.size());
}

void PrintSurroundings(const std::vector<poe::EntitySnapshot>& list)
{
    POUT("===== Surroundings =====\n");
    for (const auto& snap : list) {
        const std::string text = poe::SnapshotToText(snap);
        POUT("%s", text.c_str());
    }
    POUT("===================\n");
    POUT("Total: %zu entities\n", list.size());
}

// 等待按键（用 ReadFile 读一行，避免 cin 在老代码里的各种坑）
void WaitForExit()
{
    POUT("\n按回车退出...\n");
    char buf[8] = { 0 };
    DWORD read = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in != INVALID_HANDLE_VALUE) {
        ReadFile(in, buf, sizeof(buf) - 1, &read, nullptr);
    }
}

int RunInventory()
{
    std::vector<poe::InventoryItem> items;
    std::string err;
    if (!usecase::ScanInventory(items, err)) {
        LOGE("[背包] 失败: %s", err.c_str());
        WaitForExit();
        return 1;
    }
    PrintInventory(items);
    WaitForExit();
    return 0;
}

int RunAround()
{
    std::vector<poe::EntitySnapshot> list;
    std::string err;
    if (!usecase::ScanSurroundings(list, err)) {
        LOGE("[周围] 失败: %s", err.c_str());
        WaitForExit();
        return 1;
    }
    PrintSurroundings(list);
    WaitForExit();
    return 0;
}

// 快照模式：一次采集，背包 + 周围一起输出（只初始化一次驱动）
int RunSnapshot()
{
    usecase::ScanResult result;
    std::string err;
    const bool ok = usecase::ScanSnapshot(result, err);

    if (!ok && result.inventory.empty() && result.surroundings.empty()) {
        LOGE("[快照] 失败: %s", err.c_str());
        WaitForExit();

        // ★ OLA 的 sdk.Reg() 会拉起一堆内部线程，
        //   注册失败时它们不退，正常 return 会让进程永远退不掉。
        //   这里打完日志直接强制结束进程。
        poelog::Shutdown();
        ExitProcess(1);
    }

    PrintInventory(result.inventory);
    POUT("\n");
    PrintSurroundings(result.surroundings);

    if (!ok) {
        LOGE("[快照] 部分失败: %s", err.c_str());
    }

    WaitForExit();
    return ok ? 0 : 1;
}

// navigate 模式：解析参数 -> 调用例
int RunNavigate(int argc, char* argv[])
{
    usecase::NavigateOptions opt;
    usecase::LoadNavOptionsFromIni(opt);
    auto value=[&](const char* k){return GetOption(argc,argv,k);};
    auto x=value("x"),y=value("y"),step=value("step"),tol=value("tol"),delay=value("replan"),timeout=value("walkto"),stall=value("stall"),bmp=value("bmp");
    if(!x.empty())opt.goalX=(int32_t)strtol(x.c_str(),nullptr,10);
    if(!y.empty())opt.goalY=(int32_t)strtol(y.c_str(),nullptr,10);
    if(!step.empty())opt.stepDistance=(int32_t)strtol(step.c_str(),nullptr,10);
    if(!tol.empty())opt.arriveTolerance=(int16_t)strtol(tol.c_str(),nullptr,10);
    if(!delay.empty())opt.replanMs=(uint32_t)strtoul(delay.c_str(),nullptr,10);
    if(!timeout.empty())opt.walkTimeoutSec=(uint32_t)strtoul(timeout.c_str(),nullptr,10);
    if(!stall.empty())opt.stallSec=(uint32_t)strtoul(stall.c_str(),nullptr,10);
    for(const char* k:{"gx","gy","wall","solid","center","skip","backoff"})
        if(!value(k).empty()){LOGE("[Entry] removed grid option: -%s; use world-coordinate -x/-y/-step",k);return 2;}
    if(HasFlag(argc,argv,"griddiag")||HasFlag(argc,argv,"norelax")){LOGE("[Entry] grid diagnostics removed");return 2;}
    opt.dryRun=HasFlag(argc,argv,"dry")||HasFlag(argc,argv,"dryrun");
    opt.singleShot=HasFlag(argc,argv,"single");
    opt.mapBmpPath=bmp.empty()?app::Config().BmpPath():bmp;
    return usecase::Navigate(opt);
}

} // namespace

int main(int argc, char* argv[])
{
    struct PipeCleanup { ~PipeCleanup(){pipe::StopManagedConnections();} } pipeCleanup;
    // Config loading logs status lines, so initialize the logger first. This also
    // ensures the very first redirected line carries the UTF-8 BOM.
    poelog::Init(poelog::Target::Console);

    // 1) 配置：同目录 config.ini（没有就用默认值）+ 环境变量覆盖
    app::AppConfig& cfg = app::Config();
    cfg.LoadFromFile("config.ini");
    cfg.ApplyEnvironmentOverrides();
    const std::string targetPid = GetOption(argc, argv, "pid");
    const std::string targetStart = GetOption(argc, argv, "start");
    if (!targetPid.empty() || !targetStart.empty()) {
        char* pidEnd = nullptr; char* startEnd = nullptr;
        const auto pid = strtoull(targetPid.c_str(), &pidEnd, 10);
        const auto start = strtoull(targetStart.c_str(), &startEnd, 10);
        std::string error;
        if (targetPid.empty() || targetStart.empty() || !pid || pid > MAXDWORD || !start ||
            !pidEnd || *pidEnd || !startEnd || *startEnd ||
            !cfg.BindTarget(DWORD(pid), start, nullptr, error)) {
            LOGE("[Target] Invalid/expired target identity: %s", error.c_str());
            poelog::Shutdown(); return 2;
        }
        pipe::SetExpectedGameIdentity(cfg.targetPid, cfg.targetStart);
    }

    // 默认模式就是 navigate：这个项目现在的用途就是自动寻路。
    // 想看背包/周围快照仍然可以显式写 `Plug.exe snapshot`。
    const std::string mode = (argc > 1) ? argv[1] : "navigate";
    if (!cfg.targetPid && mode != "help" && mode != "-h" && mode != "--help") {
        const auto pids = gametarget::PidsByImage(std::wstring(cfg.gameModuleName.begin(), cfg.gameModuleName.end()));
        if (pids.size() != 1) {
            LOGE("[Target] Found %zu game processes; specify -pid=<PID> -start=<creation FILETIME> or use NavGUI.", pids.size());
            poelog::Shutdown(); return 2;
        }
        std::string error;
        if (!cfg.BindTarget(pids.front(), gametarget::CreationTime(pids.front()), nullptr, error)) {
            LOGE("[Target] %s", error.c_str()); poelog::Shutdown(); return 2;
        }
        pipe::SetExpectedGameIdentity(cfg.targetPid, cfg.targetStart);
    }

    int rc = 0;

    if (mode == "help" || mode == "-h" || mode == "--help") {
        PrintUsage();
        poelog::Shutdown();
        return 0;
    }

    if (mode == "skills") {
        poe::SkillSlotsSnapshot snapshot;
        std::string error;
        if (!usecase::ScanSkillSlots(snapshot, error)) {
            LOGE("[Skills] %s", error.c_str());
            poelog::Shutdown();
            return 1;
        }
        LOGI("[Skills] PID=%u character=%s slots=%zu", snapshot.pid,
             snapshot.characterName.c_str(), snapshot.slots.size());
        const auto text = poe::SkillSlotsToTsv(snapshot);
        POUT("%s", text.c_str());
        poelog::Shutdown();
        return 0;
    }

    if (mode == "snapshot") {
        LOGI("[Entry] Snapshot (inventory + surroundings)");
        rc = RunSnapshot();
        poelog::Shutdown();
        return rc;
    }

    if (mode == "inventory" || mode == "entity" || mode == "traverse") {
        LOGI("[Entry] Inventory traversal");
        rc = RunInventory();
        poelog::Shutdown();
        return rc;
    }

    if (mode == "around" || mode == "surroundings") {
        LOGI("[Entry] Surroundings traversal");
        rc = RunAround();
        poelog::Shutdown();
        return rc;
    }

    if (mode == "map") {
        LOGI("[Entry] Map client");
        const std::string showcase = GetOption(argc, argv, "showcase");
        rc = usecase::FetchMap(showcase.empty() ? "binary" : showcase);
        poelog::Shutdown();
        return rc;
    }

    if (mode == "replay") {
        // Plug.exe replay <file.bin> <gridW> <gridH> [-out=x.bmp] [-showcase=grade]
        if (argc < 5) {
            LOGE("replay 需要 3 个参数: <file.bin> <gridW> <gridH>");
            PrintUsage();
            poelog::Shutdown();
            return 1;
        }
        const std::string bin = argv[2];
        const uint32_t gw = (uint32_t)strtoul(argv[3], nullptr, 0);
        const uint32_t gh = (uint32_t)strtoul(argv[4], nullptr, 0);
        const std::string out = GetOption(argc, argv, "out");
        const std::string showcase = GetOption(argc, argv, "showcase");

        LOGI("[Entry] Map replay");
        rc = usecase::ReplayMap(bin, gw, gh,
                                showcase.empty() ? "binary" : showcase,
                                out);
        poelog::Shutdown();
        return rc;
    }

    if (mode == "dump") {
        // Plug.exe dump [-layer=N] [-style=raw|grade|walkable]
        const std::string layerS = GetOption(argc, argv, "layer");
        const std::string styleS = GetOption(argc, argv, "style");

        uint8_t layer = 0;
        uint8_t style = 1;   // 默认 grade：最能看清减速带
        if (!layerS.empty()) {
            layer = (uint8_t)strtoul(layerS.c_str(), nullptr, 0);
        }
        if (!styleS.empty()) {
            if (styleS == "raw")           style = 0;
            else if (styleS == "walkable") style = 2;
            else                            style = 1;
        }

        LOGI("[Entry] Dump map on Monitor (layer=%u style=%u)",
             (unsigned)layer, (unsigned)style);
        rc = usecase::DumpMapOnMonitor(layer, style, true);
        poelog::Shutdown();
        return rc;
    }

    if (mode == "walkscan") {
        // Plug.exe walkscan [-obj=N] [-arg=N] [-from=0x198] [-to=0x1A0] [-settle=MS] [-objaddr=0x..] [-force]
        //
        // ★ 安全约束：默认只试"已验证过的那一个槽位"（kActionWalkVtblOff）。
        //
        //   原来默认范围是 0x190~0x250 —— 不带参数就是一口气盲调 24 个
        //   未知虚函数。实测踩过两次：调 slot 0x190 直接把游戏搞崩
        //   （日志最后一行停在 "试 slot 0x190 entry=..." 就没了）。
        //   虚函数槽位不是"猜错了顶多没反应"，它可能走析构、改状态、
        //   解引用空指针。所以宽范围必须显式 -force，且仍然会打警告。
        const std::string objS   = GetOption(argc, argv, "obj");
        const std::string argS   = GetOption(argc, argv, "arg");
        const std::string fromS  = GetOption(argc, argv, "from");
        const std::string toS    = GetOption(argc, argv, "to");
        const std::string setS   = GetOption(argc, argv, "settle");
        const std::string addrS  = GetOption(argc, argv, "objaddr");
        const bool force = HasFlag(argc, argv, "force");

        uint8_t  objMode = objS.empty()  ? 0 : (uint8_t)strtoul(objS.c_str(), nullptr, 0);
        uint8_t  argMode = argS.empty()  ? 0 : (uint8_t)strtoul(argS.c_str(), nullptr, 0);
        uint32_t settle   = setS.empty()  ? 400   : (uint32_t)strtoul(setS.c_str(), nullptr, 0);
        uint64_t objAddr  = addrS.empty() ? 0ULL  : (uint64_t)_strtoui64(addrS.c_str(), nullptr, 0);

        const uint32_t safeFrom = (uint32_t)kActionWalkVtblOff;
        const uint32_t safeTo   = (uint32_t)kActionWalkVtblOff + 8;

        uint32_t slotFrom = fromS.empty() ? safeFrom : (uint32_t)strtoul(fromS.c_str(), nullptr, 0);
        uint32_t slotTo   = toS.empty()   ? safeTo   : (uint32_t)strtoul(toS.c_str(), nullptr, 0);

        // 超出"唯一已验证槽位"就要求 -force
        const bool outOfSafe = (slotFrom < safeFrom) || (slotTo > safeTo);
        if (outOfSafe && !force) {
            LOGW("[Entry] 槽位范围 0x%X~0x%X 超出自验证区 0x%X~0x%X，已强制收敛到安全范围。",
                 slotFrom, slotTo, safeFrom, safeTo);
            LOGW("[Entry] 盲调未知虚函数会把游戏搞崩（实测发生过）。确实要试请加 -force。");
            slotFrom = safeFrom;
            slotTo   = safeTo;
        }
        else if (outOfSafe) {
            LOGW("[Entry] ★ 已加 -force：将盲调 0x%X~0x%X 之间的虚函数槽位，"
                 "有把游戏搞崩的风险，崩了不要怪工具。", slotFrom, slotTo);
        }

        LOGI("[Entry] Walk scan obj=%u arg=%u slot 0x%X~0x%X settle=%ums objaddr=0x%llX force=%d",
             (unsigned)objMode, (unsigned)argMode, slotFrom, slotTo, settle,
             (unsigned long long)objAddr, (int)force);
        rc = usecase::ScanWalkSlot(objMode, argMode, slotFrom, slotTo, 0, 0, settle, objAddr);
        poelog::Shutdown();
        return rc;
    }

    // ---- 技能释放诊断（新战斗模块）----
    //
    // 只是把参数原样发给 Monitor，由它在游戏进程内走技能 call。
    // 逐字段暴露出来，是为了让诊断阶段"改一个变量看一次效果"，
    // 不用每验证一个假设就重编 DLL。
    // ---- 只停止施法（不释放技能）----
    //
    // 和 skill 分开成独立命令：
    //   skill 走 0x18C940（释放），状态依赖强、风险高；
    //   skillstop 走 0x211F0A0（停止），只需 rcx、每次都能生效。
    //   实测停止 call 能真正打断正在播的施法。
    if (mode == "skillstop") {
        LOGI("[Entry] SkillStop");
        return usecase::SkillStop();
    }

    if (mode == "skill" || mode == "skilldump") {
        const std::string slotS  = GetOption(argc, argv, "slot");
        const std::string r8S    = GetOption(argc, argv, "r8");
        const std::string sp28S  = GetOption(argc, argv, "sp28");
        const std::string sp30S  = GetOption(argc, argv, "sp30");
        const std::string rdxS   = GetOption(argc, argv, "rdx");
        const std::string r9S    = GetOption(argc, argv, "r9");
        const std::string stopS  = GetOption(argc, argv, "stop");

        const uint32_t slot = slotS.empty() ? (uint32_t)kSkillSlotRightMouse
                                            : (uint32_t)strtoul(slotS.c_str(), nullptr, 0);
        const uint64_t r8   = r8S.empty() ? kSkillArgR8RightMouse
                                          : (uint64_t)_strtoui64(r8S.c_str(), nullptr, 0);
        const uint64_t sp28 = sp28S.empty() ? kSkillArgStack28
                                            : (uint64_t)_strtoui64(sp28S.c_str(), nullptr, 0);
        const uint64_t sp30 = sp30S.empty() ? kSkillArgStack30Default
                                            : (uint64_t)_strtoui64(sp30S.c_str(), nullptr, 0);
        const uint8_t  rdxMode = rdxS.empty() ? 0 : (uint8_t)strtoul(rdxS.c_str(), nullptr, 0);
        const uint8_t  r9Mode  = r9S.empty()  ? 0 : (uint8_t)strtoul(r9S.c_str(), nullptr, 0);
        const uint32_t stopMs = stopS.empty() ? 0u
                                : (uint32_t)strtoul(stopS.c_str(), nullptr, 0);

        const std::string xs = GetOption(argc, argv, "x");
        const std::string ys = GetOption(argc, argv, "y");
        const int32_t px = xs.empty() ? 0 : (int32_t)strtol(xs.c_str(), nullptr, 0);
        const int32_t py = ys.empty() ? 0 : (int32_t)strtol(ys.c_str(), nullptr, 0);

        // -posmode=0 用 -x/-y 指定的坐标；1（默认）= 角色前方。
        // 方便对照"技能落在怪身上"和"落在角色前方"两种行为的差别。
        const std::string posModeS = GetOption(argc, argv, "posmode");
        const uint8_t posMode = posModeS.empty()
            ? (uint8_t)1 : (uint8_t)strtoul(posModeS.c_str(), nullptr, 0);

        // skilldump = 强制 dryRun 的只读诊断
        const bool dry = (mode == "skilldump") || HasFlag(argc, argv, "dry");

        LOGI("[Entry] Skill slot=%u r8=0x%llX rdxMode=%u r9Mode=%u dry=%d stopMs=%u posMode=%u",
             slot, (unsigned long long)r8, (unsigned)rdxMode, (unsigned)r9Mode,
             (int)dry, stopMs, (unsigned)posMode);
        rc = usecase::SkillProbe(slot, r8, px, py, rdxMode, r9Mode,
                                 sp28, sp30, dry, stopMs, posMode);
        poelog::Shutdown();
        return rc;
    }

    if (mode == "navigate" || mode == "nav") {
        rc = RunNavigate(argc, argv);
        poelog::Shutdown();
        return rc;
    }

    LOGE("未知模式: %s", mode.c_str());
    PrintUsage();
    poelog::Shutdown();
    return 1;
}
