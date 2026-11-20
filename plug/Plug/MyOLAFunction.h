#pragma once
// ============================================================
// MyOLAFunction.h —— Plug 侧门面：定位游戏 + 驱动初始化 + 链读取
//
// 重构后这里只剩"跟游戏进程打交道"的胶水，具体实现全部下沉：
//   注册 / 驱动回退 / 读内存  -> Common\OlaDriver
//   地址校验 / SSO / 链        -> Common\GameMemory + Common\PointerChain
//   配置（窗口名/模块名）      -> Common\AppConfig
//
// 所以本文件不再定义 kIntTypeU8 / kStringTypeUnicode 之类的码值，
// 也不再包含 SDK 头 —— 上层永远不需要认识 OLA。
//
// 本文件是纯 ASCII。
// ============================================================
#include <windows.h>
#include <cstdint>
#include <string>

#include "../Common/AppConfig.h"
#include "../Common/OlaDriver.h"
#include "../Common/GameMemory.h"
#include "../Common/PointerChain.h"
#include "../Common/GameOffsets.h"

// ==================== 游戏进程上下文 ====================
struct GameContext {
    HWND   hwnd = nullptr;
    DWORD  pid  = 0;
};

// ==================== 全局运行环境 ====================
// 一个用例需要的东西全在这里，避免每个函数都传 4 个参数
struct GameSession {
    ola::OlaDriver      driver;
    mem::GameMemory     memory;
    GameContext         game;
    uint64_t            moduleBase = 0;

    GameSession() : memory(driver) {}
};

// ==================== 函数 ====================

// 定位游戏窗口与进程（窗口名取自 AppConfig）
bool LocateGame(GameContext& out);

// 完整初始化：注册 OLA -> 定位游戏 -> 开驱动 -> 取模块基址
// 返回 false 时 errOut 里是人类可读的失败原因
bool InitializeSession(GameSession& session, std::string* errOut = nullptr);

// 沿链读值（兼容旧调用点；新代码建议直接用 mem::PointerChain）
uint64_t ReadChainValue(const mem::GameMemory& memory, uint64_t moduleBase,
                        const uint64_t* offsets, int count, bool tailIsDeref);

// 背包链 -> 哨兵节点（末级解引用）
uint64_t ResolveInventorySentinel(const GameSession& session);
