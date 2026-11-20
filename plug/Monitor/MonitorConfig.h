#pragma once
// ============================================================
// MonitorConfig.h —— Monitor.dll 侧的最小配置读取
//
// 为什么不用 Common\AppConfig：
//   AppConfig.cpp 依赖 Logger.cpp（Plug 侧的控制台日志），
//   而 Monitor 是 SubSystem=Windows 的 DLL，不想要这套依赖。
//   这里只读 Monitor 真正需要的 3 个值，纯 fstream，零依赖。
//
// 与 AppConfig 共用同一个 config.ini（在 DLL 同目录找），
// 键名保持一致，避免两套配置格式。
//
// 本文件是纯头文件 + inline。
// ============================================================
#include <windows.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include <cstring>
#include "../Common/GameTarget.h"

namespace mcfg {

struct MonitorConfig {
    std::string gameModuleName = "PathOfExile.exe";   // [game] moduleName
    std::string gameWindowTitle = "Path of Exile 2";  // [game] windowTitle
    std::string pipeName       = "Lmy";               // [pipe] name
    std::string logFileName    = "monitor_log.txt";   // [output] logPath

    // [nav] packSwap：旧版走路 call 的 r8 打包顺序开关（0 = (y<<32)|x）。
    //
    // ★ 已废弃（保留字段只为兼容老配置文件，读到也**不参与任何计算**）。
    //
    //   ★ 这里以前写着"现在坐标是传指针，不再打包"，那是错的：
    //     ActionWalker::WalkTo 一直走的是 PackWalkPos(x,y) = (y<<32)|x，
    //     rdx 是常数 0x400 而非指针（见 GameOffsets.h 的三组消融实测）。
    //     实现从来没改成传指针，只有这句注释被误改过。
    //     改坐标语义时请同时改 GameOffsets.h 的实测记录，别再单独改注释。
    int packSwap = 0;

    // ---- [skill] 技能 call 的熔断参数 ----
    //
    // minIntervalMs：两次真实 Cast 之间的最小间隔（毫秒）。
    //
    //   ★ 旧版实测"间隔 5 秒 / 6 秒 -> ret 变成大负数并卡死，32 秒才成功"，
    //     当时据此把默认值压到 30000，并下结论"0x18C940 有 10~30 秒恢复期"。
    //     但引用计数被扣到 0 的 bug 修掉之后，那个结论很可能是假象：
    //     第二次调用时 _Uses 已经是 1，函数内部一次 DecRef 就归零析构，
    //     表现出来就是"返回值古怪 + 卡死"，跟间隔长短没有因果关系。
    //   ★ 引用计数被扣到 0 的 bug 修掉之后，那个结论被证明是假象：
    //     第二次调用时 _Uses 已经是 1，函数内部一次 DecRef 就归零析构，
    //     表现出来就是"返回值古怪 + 卡死"，跟间隔长短没有因果关系。
    //   ★ 修好后的实测（2026-11-09）：30000 / 8000 / 3000 三种间隔都能
    //     连续成功，每次引用计数配平。现在默认 3000，与 config.ini 的
    //     [skill] minIntervalMs 是所有技能调用共用的后端最小间隔。
    //     别再往 2000 以下压：maxPerSecond 是 1，会被每秒上限直接拒掉。
    int skillMinIntervalMs = 3000;

    // maxCasts：单次注入允许的真实 Cast 次数上限，0 = 不限制。
    //   兜底用，防止脚本跑飞把游戏打崩。
    int skillMaxCasts = 500;
};

namespace detail {

inline std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::string ToLower(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}


// 本 DLL 的模块句柄，由 DllMain 通过 SetModule 设置。
//
// ★ 为什么需要它：GetModuleFileNameA(nullptr, ...) 在 DLL 里返回的是
//   **宿主进程 exe** 的路径（游戏安装目录），不是 DLL 自己的路径。
//   之前就是踩了这个坑：config.ini 明明放在 DLL 同目录，
//   却一直去游戏目录找，找不到就用默认值，导致配置项永远不生效。
inline HMODULE& ThisModule()
{
    static HMODULE mod = nullptr;
    return mod;
}

inline void SetModule(HMODULE mod)
{
    ThisModule() = mod;
}

// 本 DLL 所在目录（extern 全局，定义在 MonitorConfig.cpp）
// 用 extern 而不是 inline 里的 static：后者每个编译单元各一份，
// dllmain.cpp 设的值 ModuleDir() 看不到，会一直读宿主进程目录。
extern std::string g_dllDir;

inline void SetModuleDir(const std::string& dir)
{
    g_dllDir = dir;
}

// 取当前 DLL 所在目录（带尾部分隔符）
inline std::string ModuleDir()
{
    char mod[MAX_PATH] = { 0 };
    GetModuleFileNameA(ThisModule(), mod, MAX_PATH);
    std::string full(mod);
    const size_t pos = full.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : full.substr(0, pos + 1);
}

} // namespace detail

// 暴露实际使用的配置文件完整路径（诊断用：确认到底读的哪个文件）
inline std::string ConfigPath(const char* fileName = "config.ini")
{
    const auto directory = gametarget::SessionDirectory(gametarget::ModuleDirectory(detail::ThisModule()),
        GetCurrentProcessId(), gametarget::CreationTime(GetCurrentProcess()));
    const std::wstring profile = directory + std::wstring(fileName, fileName + strlen(fileName));
    if (GetFileAttributesW(profile.c_str()) != INVALID_FILE_ATTRIBUTES) return gametarget::Ansi(profile);
    return detail::ModuleDir() + fileName;
}

// 从 DLL 同目录的 ini 加载。文件不存在就用默认值（不报错）。
inline void LoadFromFile(MonitorConfig& cfg, const char* fileName = "config.ini")
{
    const std::string path = ConfigPath(fileName);

    std::ifstream ifs(path);

    std::string section;
    std::string line;
    while (std::getline(ifs, line)) {
        line = detail::Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = detail::ToLower(detail::Trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = detail::ToLower(detail::Trim(line.substr(0, eq)));
        std::string value = detail::Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (value.empty()) continue;

        if (section == "game" && key == "modulename") {
            cfg.gameModuleName = value;
        }
        else if (section == "game" && key == "windowtitle") {
            cfg.gameWindowTitle = value;
        }
        else if (section == "pipe" && key == "name") {
            cfg.pipeName = value;
        }
        else if (section == "output" && key == "logpath") {
            cfg.logFileName = value;
        }
        else if (section == "nav" && key == "packswap") {
            cfg.packSwap = atoi(value.c_str());
        }
        else if (section == "skill" && key == "minintervalms") {
            cfg.skillMinIntervalMs = atoi(value.c_str());
        }
        else if (section == "skill" && key == "maxcasts") {
            cfg.skillMaxCasts = atoi(value.c_str());
        }
    }
    // Always isolate, including when config.ini is absent. Never listen on a shared base name.
    cfg.pipeName = gametarget::PipeName(cfg.pipeName, GetCurrentProcessId());
    const std::wstring base = gametarget::ModuleDirectory(detail::ThisModule());
    const std::wstring directory = gametarget::SessionDirectory(base, GetCurrentProcessId(),
        gametarget::CreationTime(GetCurrentProcess()));
    CreateDirectoryW((base + L"profiles").c_str(), nullptr);
    CreateDirectoryW(directory.c_str(), nullptr);
    cfg.logFileName = gametarget::Ansi(directory.substr(base.size()) + L"monitor_log.txt");
}

// 管道短名 -> 宽字符（纯 ASCII 场景直接扩）
inline std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w;
    if (need > 0) {
        w.resize((size_t)need);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], need);
    }
    else {
        w.assign(s.begin(), s.end());
    }
    return w;
}

} // namespace mcfg
