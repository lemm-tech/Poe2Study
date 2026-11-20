#include "PanelSession.h"
#include "UiTheme.h"
#include "../Common/AppConfig.h"
#include "../Common/Logger.h"
#include <shellapi.h>
#include <algorithm>

#pragma comment(lib, "shell32.lib")

namespace sessions {
namespace {
constexpr UINT kTrayMessage = WM_APP + 81, kNameReady = WM_APP + 82;
constexpr UINT kTrayId = 1, kShow = 1, kExit = 2;
constexpr WPARAM kSystemExit = 0x1F10;
constexpr UINT_PTR kLifetimeTimer = 0x1F11;
}
PanelSession::~PanelSession() { Shutdown(); }
bool PanelSession::Initialize(HWND window, PanelGuard& lifetime) {
    window_ = window;
    lifetime_ = &lifetime;
    const auto& config = app::Config();
    target_ = {config.targetPid, config.targetStart, config.targetWindow};
    if (!RegisterPanel(window_, target_)) {
        LOGE("[Panel] Cannot register session window: %lu", GetLastError());
        window_ = nullptr; lifetime_ = nullptr; return false;
    }
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (HMENU menu = GetSystemMenu(window_, FALSE)) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSystemExit, L"停止功能并退出此面板");
    }
    SetTitle(L"等待角色名 · PID " + std::to_wstring(target_.pid));
    // Independent of drawing/name polling, including nested dialog loops and tray hiding.
    if (!SetTimer(window_, kLifetimeTimer, 250, nullptr)) {
        LOGE("[Panel] Cannot create lifetime timer: %lu", GetLastError());
        Shutdown(); return false;
    }
    Tick();
    return true;
}
bool PanelSession::UpdateTray() {
    if (!window_) return false;
    NOTIFYICONDATAW icon{}; icon.cbSize = sizeof(icon);
    icon.hWnd = window_; icon.uID = kTrayId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = ui::AppIcon(true);
    const auto tip = title_ + L" · PID " + std::to_wstring(target_.pid) + L"\n双击显示面板，右键停止并退出";
    std::copy_n(tip.c_str(), (std::min)(tip.size(), _countof(icon.szTip) - 1), icon.szTip);
    if (trayAdded_ && Shell_NotifyIconW(NIM_MODIFY, &icon)) return true;
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    if (trayAdded_) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        trayVersion4_ = Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
    }
    return trayAdded_;
}
void PanelSession::SetTitle(const std::wstring& title) {
    if (!window_ || title == title_) return;
    title_ = title;
    SetWindowTextW(window_, title_.c_str());
    UpdateTray();
}
void PanelSession::Tick() {
    if (!window_ || stopping_ || nameReader_.joinable() || GetTickCount64() < nextNameRead_) return;
    nextNameRead_ = GetTickCount64() + 5000;
    const auto module = app::Config().gameModuleName;
    try {
        nameReader_ = std::thread([this, module] {
            try { nameResult_ = ReadCharacterName(target_, module); }
            catch (...) { nameResult_.clear(); }
            if (!stopping_) PostMessageW(window_, kNameReady, 0, 0);
        });
    } catch (...) {
        LOGE("[Panel] Cannot start character name reader; retrying in 5s");
    }
}
void PanelSession::Hide() {
    // If the shell has no tray, keep a minimized taskbar entry as a recovery path.
    const bool haveTray = UpdateTray();
    ShowOwnedPopups(window_, FALSE);
    ShowWindow(window_, haveTray ? SW_HIDE : SW_MINIMIZE);
    LOGI("[Panel] Window hidden; feature runners and connections remain active");
}
void PanelSession::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kShow, L"显示面板");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExit, L"停止功能并退出此面板");
    SetMenuDefaultItem(menu, kShow, FALSE);
    POINT point{}; GetCursorPos(&point);
    SetForegroundWindow(window_);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (command == kShow) RestorePanel(window_);
    else if (command == kExit) DestroyWindow(window_);
}
bool PanelSession::HandleMessage(UINT message, WPARAM wp, LPARAM lp) {
    if (!window_) return false;
    if (message == WM_TIMER && wp == kLifetimeTimer) {
        if (!stopping_ && lifetime_ && !lifetime_->Active()) {
            LOGI("[Panel] Closing session: %s", lifetime_->ExitReason());
            // WM_CLOSE intentionally hides; lifetime exits must run full teardown.
            DestroyWindow(window_);
        }
        return true;
    }
    if (message == WM_CLOSE) { Hide(); return true; }
    if (message == WM_SYSCOMMAND && (wp & 0xFFF0) == kSystemExit) {
        DestroyWindow(window_); return true;
    }
    if (message == kNameReady) {
        if (nameReader_.joinable()) nameReader_.join();
        if (!stopping_ && !nameResult_.empty() && gametarget::Alive(target_.pid, target_.start))
            SetTitle(nameResult_);
        nameResult_.clear();
        return true;
    }
    if (message == kTrayMessage) {
        const UINT event = trayVersion4_ ? LOWORD(lp) : UINT(lp);
        if (event == WM_LBUTTONDBLCLK || event == NIN_SELECT || event == NIN_KEYSELECT) RestorePanel(window_);
        else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) ShowMenu();
        return true;
    }
    if (taskbarCreated_ && message == taskbarCreated_) {
        trayAdded_ = false;
        if (!UpdateTray() && !IsWindowVisible(window_)) RestorePanel(window_);
        return true;
    }
    return false;
}
void PanelSession::Shutdown() {
    stopping_.store(true);
    lifetime_ = nullptr;
    if (window_) KillTimer(window_, kLifetimeTimer);
    if (nameReader_.joinable()) nameReader_.join();
    if (!window_) return;
    if (trayAdded_) {
        NOTIFYICONDATAW icon{}; icon.cbSize = sizeof(icon);
        icon.hWnd = window_; icon.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &icon);
    }
    RemovePropW(window_, gametarget::PanelLockName(target_.pid, target_.start).c_str());
    window_ = nullptr;
    trayAdded_ = false;
}
} // namespace sessions
