#pragma once
// ============================================================
// AppConfig.h —— 配置项的唯一来源
//
// 全工程所有「资源名字」都从这里取：
//   游戏窗口标题、模块名、管道名、BMP 输出路径、日志路径、OLA 凭据。
//
// 规矩：
//   1. 任何 .cpp 里都不准再出现 L"Lmy" / "map_cost.bmp" /
//      "Path of Exile 2" / "PathOfExile.exe" 这类字面量。
//   2. 密钥不再硬编码在 main.cpp 函数体里，走配置文件或环境变量。
//   3. 找不到配置文件就用结构体默认值，程序照常跑（不报错退出）。
//
// 本文件是纯 ASCII，谁 include 都安全。
// ============================================================
#include <string>
#include "GameTarget.h"

namespace app {

// OLA（欧拉）插件注册凭据
struct OlaCredentials {
    std::string account;
    std::string password;
    std::string appTag;
    // OLA 插件 DLL 路径（默认用带版本号的文件名；SDK 自身只会找不带版本号的）
    std::string dllPath;

        // OLA 注册超时（秒）。sdk.Reg() 在网络不通时不返回也不报错，
        // 所以外面套一层超时，避免整个进程挂死。
        int regTimeoutSec = 15;
};

// 运行期配置
struct AppConfig {
    // Immutable target identity for this panel and all subprocesses it starts.
    DWORD targetPid = 0;
    uint64_t targetStart = 0;
    HWND targetWindow = nullptr;
    std::wstring sessionDirectory;
    bool BindTarget(DWORD pid, uint64_t start, HWND window, std::string& error);
    bool TargetAlive() const;
    HWND TargetWindow() const;
    std::wstring RuntimeFileW(const wchar_t* file) const;
    std::string RuntimeFile(const char* file) const;
    // ---- 目标游戏 ----
    std::string gameWindowTitle = "Path of Exile 2";
    std::string gameModuleName  = "PathOfExile.exe";

    // 优先用本地 ReadProcessMemory 读内存，不依赖 OLA 授权。
    // OLA 授权过期时 Reg() 会卡死，本地 RPM 没这个风险。
    // 设为 false 可强制回到 OLA 那条路。
    bool useLocalRpm = true;

    // ---- 进程间通信 ----
    // 管道短名（不含 \\.\pipe\ 前缀），两端共用
    std::string pipeName = "Lmy";

    // ---- 输出 ----
    // 相对路径相对 exe 所在目录解析（..\..\output\ = plug\output\），实际使用请调 BmpPath()/LogPath()，
    // 它们会把相对路径解析成"exe 目录 + 路径"，不依赖进程当前工作目录。
    std::string bmpPath = "..\\..\\output\\map_cost.bmp";
    std::string logPath = "monitor_log.txt";

    // ---- 凭据 ----
    OlaCredentials ola;

    // 从 ini 文件加载（不存在则保留默认值，返回 false 但不报错）
    // 支持的键：
    //   [game]   windowTitle / moduleName
    //   [pipe]   name
    //   [output] bmpPath / logPath
    //   [ola]    account / password / appTag / dllPath
    bool LoadFromFile(const std::string& iniPath);

    // 环境变量覆盖（优先级高于 ini）：
    //   POE2_PIPE_NAME / POE2_LOG_PATH / POE2_BMP_PATH
    //   POE2_OLA_ACCOUNT / POE2_OLA_PASSWORD / POE2_OLA_APPTAG
    void ApplyEnvironmentOverrides();

    // ---- 解析后的绝对路径 ----
    // 为什么需要：BmpWriter 用 std::ofstream 按"进程当前工作目录"打开文件，
    // 从快捷方式 / 资源管理器 / VS 调试器启动时工作目录各不相同，
    // 相对路径会让图落到随机位置。这里统一按 exe 目录解析，
    // 并自动创建缺失的父目录（如 output\）。
    std::string BmpPath() const;
    std::string LogPath() const;

    // 宽字符版管道全名：\\.\pipe\<pipeName>
    std::wstring PipeFullName() const;
};

// 全局唯一实例（首次调用时构造，避免静态初始化顺序问题）
AppConfig& Config();

} // namespace app
