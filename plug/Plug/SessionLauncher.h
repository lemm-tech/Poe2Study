#pragma once
#include "../Common/GameTarget.h"
#include <atomic>
#include <functional>

namespace sessions {
struct Selection {
    DWORD pid = 0;
    uint64_t start = 0;
    HWND window = nullptr;
    DWORD launcherPid = 0;
    uint64_t launcherStart = 0;
    HWND launcherWindow = nullptr;
};
// Panel identity must survive title changes, duplicate names and hidden windows.
bool RegisterPanel(HWND panel, const Selection& selection);
HWND FindPanel(const Selection& selection);
void RestorePanel(HWND panel);
std::wstring ReadCharacterName(const Selection& selection, const std::string& moduleName);
// No arguments means launcher. Malformed/partial target arguments are errors.
bool ParseSelection(Selection& selection, std::wstring& error);
int RunLauncher(HINSTANCE instance, int show);
using Connect = std::function<bool(std::atomic_bool&, std::wstring&)>;
class PanelGuard {
    HANDLE lock = nullptr;
    HANDLE process = nullptr;
    HANDLE launcher = nullptr;
    Selection selection_;
    ULONGLONG windowMissingAt_ = 0;
    const char* exitReason_ = "panel closed";
public:
    ~PanelGuard();
    PanelGuard() = default;
    PanelGuard(const PanelGuard&) = delete;
    PanelGuard& operator=(const PanelGuard&) = delete;
    bool Acquire(const Selection& selection, std::wstring& error);
    bool Active();
    bool WaitForMessage();
    const char* ExitReason() const { return exitReason_; }
};
bool RunConnectDialog(HINSTANCE instance, PanelGuard& guard, const Connect& connect);
} // namespace sessions
