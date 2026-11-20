#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <tlhelp32.h>

namespace gametarget {
inline std::vector<DWORD> PidsByImage(const std::wstring& imageName) {
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        if (!_wcsicmp(entry.szExeFile, imageName.c_str())) result.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}
inline uint64_t CreationTime(HANDLE process) {
    FILETIME start{}, end{}, kernel{}, user{};
    if (!GetProcessTimes(process, &start, &end, &kernel, &user)) return 0;
    return (uint64_t(start.dwHighDateTime) << 32) | start.dwLowDateTime;
}
inline uint64_t CreationTime(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    const auto time = CreationTime(p);
    CloseHandle(p);
    return time;
}
inline bool Alive(DWORD pid, uint64_t start) {
    if (!pid || !start) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!p) return false;
    const bool valid = CreationTime(p) == start && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return valid;
}
inline HWND Window(DWORD pid, HWND preferred = nullptr) {
    DWORD owner = 0;
    if (preferred && IsWindow(preferred) && GetWindowThreadProcessId(preferred, &owner) && owner == pid)
        return preferred;
    struct Context { DWORD pid; HWND window = nullptr; LONG area = -1; } c{pid};
    EnumWindows([](HWND h, LPARAM arg) -> BOOL {
        auto& c = *reinterpret_cast<Context*>(arg);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != c.pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r{}; GetClientRect(h, &r);
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        if (area > c.area) { c.area = area; c.window = h; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    return c.window;
}
inline std::wstring ModuleDirectory(HMODULE module = nullptr) {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(module, path, DWORD(_countof(path)));
    if (!n || n >= _countof(path)) return {};
    const std::wstring full(path, n);
    return full.substr(0, full.find_last_of(L"\\/") + 1);
}
inline std::wstring SessionDirectory(const std::wstring& base, DWORD pid, uint64_t start) {
    return base + L"profiles\\" + std::to_wstring(pid) + L"_" + std::to_wstring(start) + L"\\";
}
inline std::string PipeName(const std::string& base, DWORD pid) {
    return base + "_" + std::to_string(pid);
}
inline std::string Ansi(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_ACP, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    if (n) WideCharToMultiByte(CP_ACP, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
inline std::wstring PanelTitle(DWORD pid, uint64_t start) {
    return L"POE2 配置面板 · PID " + std::to_wstring(pid) + L" · " + std::to_wstring(start);
}
inline std::wstring PanelLockName(DWORD pid, uint64_t start) {
    return L"Local\\Poe2NavPanel_" + std::to_wstring(pid) + L"_" + std::to_wstring(start);
}
} // namespace gametarget
