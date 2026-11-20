#include "MyOLAFunction.h"
#include "../Common/Logger.h"
#include "../Common/LocateWindow.h"
#include "../Common/GameBuild.h"

#include <windows.h>
#include <string>

// ============================================================
// MyOLAFunction.cpp —— 只做胶水，不再有任何偏移与类型码
// ============================================================

bool LocateGame(GameContext& out)
{
    const app::AppConfig& cfg = app::Config();

    // 用宽字符查找（FindWindowA 在 UTF-16 标题上会失败，见 LocateWindow.h）
    if (cfg.targetPid) out.hwnd = cfg.TargetWindow();
    else {
        const auto pids = gametarget::PidsByImage(winutil::Utf8ToWide(cfg.gameModuleName));
        if (pids.size() != 1) return false;
        out.hwnd = gametarget::Window(pids.front());
    }
    out.pid = 0;
    if (out.hwnd == nullptr) {
        return false;
    }
    GetWindowThreadProcessId(out.hwnd, &out.pid);
    if (cfg.targetPid && out.pid != cfg.targetPid) return false;
    if (out.pid != 0) {
        LOGI("[定位] 窗口 0x%p  pid=%lu", (void*)out.hwnd, (unsigned long)out.pid);
    }
    return out.pid != 0;
}

// ============================================================
bool InitializeSession(GameSession& session, std::string* errOut)
{
    auto fail = [errOut](const char* msg) {
        if (errOut) {
            *errOut = msg;
        }
        return false;
    };

    const app::AppConfig& cfg = app::Config();

    // 1) 定位游戏
    if (!LocateGame(session.game)) {
        return fail("未找到游戏窗口：请确认游戏已启动，或检查 config.ini 的 [game] windowTitle");
    }

    // 2) 打开进程（ReadProcessMemory，不需要任何授权）
    if (!session.driver.InitializeLocal(session.game.pid)) {
        return fail("OpenProcess 失败：请用管理员身份运行");
    }

    // 3) 模块基址
    session.moduleBase = session.memory.ModuleBase(cfg.gameModuleName);
    if (session.moduleBase == 0) {
        session.driver.Detach();
        return fail("取不到游戏模块基址：请确认 config.ini 的 [game] moduleName");
    }

    std::string buildError;
    if (!gamebuild::Verify(session.moduleBase,
        [&session](uint64_t address, void* out, size_t size) {
            return session.memory.ReadInto(address, out, size);
        }, buildError)) {
        session.driver.Detach();
        session.moduleBase = 0;
        return fail(buildError.c_str());
    }

    LOGI("[初始化] pid=%lu  moduleBase=0x%llX",
         (unsigned long)session.game.pid,
         (unsigned long long)session.moduleBase);
    return true;
}

uint64_t ReadChainValue(const mem::GameMemory& memory, uint64_t moduleBase,
                        const uint64_t* offsets, int count, bool tailIsDeref)
{
    if (offsets == nullptr || count <= 0) {
        return 0;
    }
    return memory.FollowChain(moduleBase, offsets, count, tailIsDeref);
}

uint64_t ResolveInventorySentinel(const GameSession& session)
{
    return mem::PointerChain(InventoryChainOffsets(), kInventoryChainDepth, mem::TailOp::Deref)
        .Resolve(session.memory, session.moduleBase);
}
