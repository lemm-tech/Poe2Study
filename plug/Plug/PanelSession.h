#pragma once
#include "SessionLauncher.h"
#include <atomic>
#include <thread>

namespace sessions {
// Owns presentation and lifetime only. Hiding never touches feature runners,
// the controller connection or monitoring of the bound game and owning launcher.
class PanelSession {
    HWND window_ = nullptr;
    PanelGuard* lifetime_ = nullptr;
    Selection target_;
    std::wstring title_, nameResult_;
    std::thread nameReader_;
    std::atomic_bool stopping_{false};
    ULONGLONG nextNameRead_ = 0;
    UINT taskbarCreated_ = 0;
    bool trayAdded_ = false;
    bool trayVersion4_ = false;
    bool UpdateTray();
    void SetTitle(const std::wstring& title);
    void Hide();
    void ShowMenu();
public:
    ~PanelSession();
    bool Initialize(HWND window, PanelGuard& lifetime);
    void Tick();
    bool HandleMessage(UINT message, WPARAM wp, LPARAM lp);
    void Shutdown();
};
} // namespace sessions
