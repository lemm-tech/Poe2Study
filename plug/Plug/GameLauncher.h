#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace sessions::game {
std::wstring NormalizePath(const std::wstring& path);
std::wstring LoadPath(const std::wstring& settingsFile);
bool SavePath(const std::wstring& settingsFile, const std::wstring& path, DWORD& error);
std::wstring ErrorText(DWORD error);
struct LaunchResult {
    DWORD pid = 0;
    std::wstring path;
    std::wstring error;
};
// Relative paths resolve against the panel directory, never the caller's CWD.
// The child uses its executable's directory and inherits no process handles.
LaunchResult Start(const std::wstring& path, const std::wstring& panelDirectory);
} // namespace sessions::game
