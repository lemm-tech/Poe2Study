#pragma once
// ============================================================
// LocateWindow.h —— 按标题找窗口（宽字符，带枚举兜底）
//
// 为什么需要单独一个头：
//   FindWindowA 匹配的是 ANSI 转换后的标题，而游戏窗口标题是
//   UTF-16（实测 "Path of Exile 2"，每字符 2 字节）。
//   某些系统区域设置下 FindWindowA 直接返回 0 —— 游戏明明在跑，
//   所有工具却报"未找到游戏窗口"。
//
//   正确路径是 FindWindowW（宽字符），再枚举兜底。
//   所有工具（probe_base / diag_chain / diag_ola / probe_costcall）
//   原来各自写 FindWindowA，现在统一走这里。
//
// 纯头文件 + inline，无 .cpp 依赖。
// ============================================================
#include <windows.h>
#include <string>

namespace winutil {

// UTF-8 -> 宽字符
inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (need <= 0) {
        return std::wstring(s.begin(), s.end());
    }
    std::wstring w((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], need);
    return w;
}

// 按标题找可见顶层窗口
//   1) FindWindowW（宽字符，正确路径）
//   2) 枚举顶层窗口，用 GetWindowTextW 前缀比较（兜底）
inline HWND FindWindowByTitle(const std::string& titleUtf8)
{
    const std::wstring titleW = Utf8ToWide(titleUtf8);
    if (titleW.empty()) return nullptr;

    if (HWND h = FindWindowW(nullptr, titleW.c_str())) {
        return h;
    }

    struct Ctx {
        const std::wstring* title;
        HWND                found;
    } ctx{ &titleW, nullptr };

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            Ctx* c = reinterpret_cast<Ctx*>(lp);

            wchar_t buf[512] = { 0 };
            if (GetWindowTextW(hwnd, buf, 512) <= 0) return TRUE;

            // 前缀比较：容忍标题后面多了补丁号之类的后缀
            if (c->title->compare(0, c->title->size(), buf) == 0 &&
                IsWindowVisible(hwnd)) {
                c->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    return ctx.found;
}

// 按标题找窗口并取 PID；失败返回 0
inline DWORD FindPidByWindowTitle(const std::string& titleUtf8, HWND* outHwnd = nullptr)
{
    HWND h = FindWindowByTitle(titleUtf8);
    if (outHwnd) *outHwnd = h;
    if (!h) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}


// ------------------------------------------------------------
// 把窗口强制切到前台（可靠版）
//
// 为什么需要它：
//   SendInput 注入的输入会发到**前台窗口**。如果游戏在后台，
//   所有合成点击都会落到别的窗口上，表现为"点了没反应"。
//   SetForegroundWindow 单独调通常会被 Windows 的前台锁拦掉，
//   必须配合 AttachThreadInput 才可靠。
// ------------------------------------------------------------
inline bool ForceForeground(HWND target, bool restoreIfMinimized = true)
{
    if (!target) {
        return false;
    }

    if (restoreIfMinimized && IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
    }

    if (GetForegroundWindow() == target) {
        return true;   // 已经在前台，什么都不用做
    }

    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD myThread = GetCurrentThreadId();

    bool attached = false;
    if (fgThread != 0 && fgThread != myThread) {
        attached = (AttachThreadInput(myThread, fgThread, TRUE) != 0);
    }

    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetActiveWindow(target);
    SetFocus(target);

    if (attached) {
        AttachThreadInput(myThread, fgThread, FALSE);
    }

    Sleep(60);
    return GetForegroundWindow() == target;
}
} // namespace winutil
