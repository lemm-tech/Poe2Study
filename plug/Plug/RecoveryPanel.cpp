#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "RecoveryPanel.h"
#include "UiTheme.h"
#include "NamedPipeClie.h"
#include "../Common/AppConfig.h"
#include "../Common/PacketProtocol.h"
#include "../Common/Recovery.h"
#include "../Common/Logger.h"
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cstring>
#include <thread>
#include <utility>

namespace recoveryui {
namespace {
HWND panel = nullptr, edits[3]{}, choices[3]{}, intervals[3]{}, toggle = nullptr;
HFONT font = nullptr, small = nullptr, bold = nullptr;
std::function<bool(std::string&)> mayEnable;
std::function<void(const std::wstring&)> report;
recovery::Config config{};
recovery::Snapshot state{};
std::wstring message = L"请先连接当前角色的新版 Monitor.dll";
bool loading = false, visible = false, shuttingDown = false, configuring = false, uncertain = true;
uint64_t nextPoll = 0;
struct WorkResult {
    bool received = false, configure = false;
    packet::Reply reply{};
    pipe::ConnectionSnapshot connection;
    std::wstring error;
} result;
std::thread worker;
std::atomic_bool busy{false}, done{false};
enum : int { Toggle = 9300, EditFirst = 9310, ChoiceFirst = 9320, IntervalFirst = 9330 };
std::wstring Wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring output(size_t((std::max)(0, size)), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), output.data(), size);
    return output;
}
std::wstring Path() { return app::Config().RuntimeFileW(L"recovery.ini"); }
bool ReadSettings(recovery::Config& value) {
    value = config;
    for (int i = 0; i < 3; ++i) {
        wchar_t text[32]{}; GetWindowTextW(edits[i], text, _countof(text));
        uint32_t percent = 0;
        for (const auto* ch = text; *ch; ++ch) {
            if (*ch < L'0' || *ch > L'9') { message = L"百分比请填写 0–100，0 表示关闭该项"; return false; }
            percent = percent * 10 + uint32_t(*ch - L'0');
            if (percent > 100) { message = L"百分比请填写 0–100，0 表示关闭该项"; return false; }
        }
        const auto flask = SendMessageW(choices[i], CB_GETCURSEL, 0, 0);
        if (flask < 0 || flask > 1) { message = L"请选择血瓶或蓝瓶"; return false; }
        GetWindowTextW(intervals[i], text, _countof(text));
        uint32_t interval = 0;
        for (const auto* ch = text; *ch; ++ch) {
            if (*ch < L'0' || *ch > L'9' || interval > 60000) {
                message = L"频率(ms)请填写 500–60000"; return false;
            }
            interval = interval * 10 + uint32_t(*ch - L'0');
        }
        if (interval < 500 || interval > 60000) { message = L"频率(ms)请填写 500–60000"; return false; }
        value.intervalMs[i] = interval;
        value.threshold[i] = percent; value.flask[i] = uint32_t(flask);
    }
    value.enabled = 0;
    return recovery::Valid(value);
}
bool SaveSettings() {
    recovery::Config edited;
    if (!ReadSettings(edited)) return false;
    const auto path = Path();
    const wchar_t* keys[] = {L"life", L"mana", L"shield"};
    for (int i = 0; i < 3; ++i) {
        const auto threshold = std::to_wstring(edited.threshold[i]), flask = std::to_wstring(edited.flask[i]);
        const auto flaskKey = std::wstring(keys[i]) + L"Flask";
        const auto intervalKey = std::wstring(keys[i]) + L"IntervalMs";
        if (!WritePrivateProfileStringW(L"recovery", keys[i], threshold.c_str(), path.c_str()) ||
            !WritePrivateProfileStringW(L"recovery", flaskKey.c_str(), flask.c_str(), path.c_str()) ||
            !WritePrivateProfileStringW(L"recovery", intervalKey.c_str(),
                std::to_wstring(edited.intervalMs[i]).c_str(), path.c_str())) {
            message = L"恢复设置保存失败，请检查配置目录权限"; return false;
        }
    }
    config = edited;
    return true;
}
bool SameController(const pipe::ConnectionSnapshot& a, const pipe::ConnectionSnapshot& b) {
    return a.ready && b.ready && a.gamePid == b.gamePid && a.sessionId == b.sessionId && a.controllerId == b.controllerId;
}
bool SetTextIfChanged(HWND window, const std::wstring& value) {
    if (!window) return false;
    const int length = GetWindowTextLengthW(window);
    std::wstring current(size_t((std::max)(0, length)) + 1, L'\0');
    GetWindowTextW(window, current.data(), int(current.size()));
    current.resize(size_t((std::max)(0, length)));
    if (current == value) return false;
    SetWindowTextW(window, value.c_str());
    return true;
}
bool EnableIfChanged(HWND window, bool enabled) {
    if (!window || (IsWindowEnabled(window) != FALSE) == enabled) return false;
    EnableWindow(window, enabled);
    return true;
}
void RefreshControls() {
    const bool configuringBusy = busy && configuring;
    SetTextIfChanged(toggle, configuringBusy ? L"处理中…" : state.enabled ? L"停止自动恢复" : L"启用自动恢复");
    // A background status query must not make the idle toggle blink disabled.
    EnableIfChanged(toggle, !shuttingDown && !uncertain && (!busy || !configuring));
    ui::SetButtonSelected(toggle, state.enabled != 0);
    for (int i = 0; i < 3; ++i) {
        EnableIfChanged(edits[i], !state.enabled && !configuringBusy);
        EnableIfChanged(choices[i], !state.enabled && !configuringBusy);
        EnableIfChanged(intervals[i], !state.enabled && !configuringBusy);
    }
    if (panel) InvalidateRect(panel, nullptr, FALSE);
}
void StartRequest(bool configure, const recovery::Config& requested = {}) {
    if (busy || done || shuttingDown) return;
    const auto connection = pipe::GetConnectionSnapshot();
    if (!connection.ready || connection.gamePid != app::Config().targetPid || !app::Config().TargetAlive()) {
        state = {}; uncertain = true; message = L"未连接当前角色的新版 Monitor.dll"; RefreshControls(); return;
    }
    if (worker.joinable()) worker.join();
    configuring = configure; if (configure) uncertain = true; busy = true; RefreshControls();
    try {
        worker = std::thread([configure, requested, connection] {
            WorkResult output; output.configure = configure; output.connection = connection;
            try {
                pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
                if (!client.Initialize() || client.SessionId() != connection.sessionId || client.ClientId() != connection.controllerId ||
                    !SameController(connection, pipe::GetConnectionSnapshot())) {
                    output.error = L"通信会话变化，自动恢复需要重新启用";
                } else {
                    packet::Request request{};
                    request.operation = configure ? packet::Operation::ConfigureRecovery : packet::Operation::RecoveryStatus;
                    if (configure) { request.length = sizeof(requested); std::memcpy(request.bytes, &requested, sizeof(requested)); }
                    packet::Message command(request); std::vector<uint8_t> body;
                    if (!client.Request(&command, MT_PacketReply, body, 2500) || body.size() != sizeof(output.reply))
                        output.error = L"恢复回执未收到，等待重新查询状态";
                    else {
                        std::memcpy(&output.reply, body.data(), sizeof(output.reply));
                        output.received = output.reply.version == packet::kVersion && output.reply.objectLength == sizeof(recovery::Snapshot);
                        if (!output.received) output.error = L"恢复回执格式无效，请使用配套 DLL";
                    }
                }
            } catch (const std::exception& error) { output.error = Wide(error.what()); }
            catch (...) { output.error = L"自动恢复通信出现异常"; }
            result = std::move(output); done = true;
        });
    } catch (...) { busy = false; message = L"无法创建恢复通信线程"; RefreshControls(); }
}
void Consume() {
    if (!done.exchange(false)) return;
    if (worker.joinable()) worker.join();
    busy = false;
    if (!SameController(result.connection, pipe::GetConnectionSnapshot())) {
        state = {}; uncertain = true; message = L"通信会话变化，自动恢复已停止，请重新启用";
    } else if (!result.received) {
        // Retain enabled/unknown state until a query confirms a lost configure reply.
        state.valid = 0; uncertain = true; message = result.error;
    } else {
        std::memcpy(&state, result.reply.object, sizeof(state)); uncertain = false;
        message = Wide(result.reply.detail);
        if (result.configure) LOGI("[RecoveryUI] result=%u enabled=%u detail=%s",
            unsigned(result.reply.result), state.enabled, result.reply.detail);
    }
    if (result.configure && report && !message.empty()) report(message);
    nextPoll = GetTickCount64() + 500;
    RefreshControls();
}
void Layout() {
    if (!panel || !edits[0]) return;
    RECT rect{}; GetClientRect(panel, &rect);
    const int width = rect.right;
    SetWindowPos(toggle, nullptr, 8, 138, width - 16, 34, SWP_NOZORDER | SWP_NOACTIVATE);
    const int label = 100, editWidth = 38, suffix = 44;
    for (int i = 0; i < 3; ++i) {
        const int y = 34 + i * 30;
        SetWindowPos(edits[i], nullptr, label, y, editWidth, 26, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(choices[i], nullptr, label + editWidth + suffix, y, 66, 160,
            SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(intervals[i], nullptr, 326, y, (std::min)(84, width - 336), 26, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (ui::HandleMessage(hwnd, msg, wp, lp, themed)) return themed;
    switch (msg) {
    case WM_CREATE: {
        panel = hwnd; font = ui::Font(14); small = ui::Font(12); bold = ui::Font(14, FW_SEMIBOLD);
        loading = true;
        const wchar_t* keys[] = {L"life", L"mana", L"shield"};
        const auto path = Path();
        const auto legacyInterval = GetPrivateProfileIntW(L"recovery", L"intervalMs", 1500, path.c_str());
        for (int i = 0; i < 3; ++i) {
            const auto percent = GetPrivateProfileIntW(L"recovery", keys[i], config.threshold[i], path.c_str());
            const auto key = std::wstring(keys[i]) + L"Flask";
            const auto flask = GetPrivateProfileIntW(L"recovery", key.c_str(), config.flask[i], path.c_str());
            if (percent <= 100) config.threshold[i] = percent;
            if (flask <= 1) config.flask[i] = flask;
            edits[i] = CreateWindowExW(0, L"EDIT", std::to_wstring(config.threshold[i]).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER, 0, 0, 44, 26, hwnd, HMENU(INT_PTR(EditFirst + i)), nullptr, nullptr);
            ui::StyleControl(edits[i], font); SendMessageW(edits[i], EM_SETLIMITTEXT, 3, 0);
            choices[i] = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 0, 0, 100, 160, hwnd, HMENU(INT_PTR(ChoiceFirst + i)), nullptr, nullptr);
            ui::StyleControl(choices[i], font);
            for (const auto* text : {L"血瓶", L"蓝瓶"}) SendMessageW(choices[i], CB_ADDSTRING, 0, LPARAM(text));
            SendMessageW(choices[i], CB_SETCURSEL, config.flask[i], 0);
            const auto intervalKey = std::wstring(keys[i]) + L"IntervalMs";
            const auto interval = GetPrivateProfileIntW(L"recovery", intervalKey.c_str(), legacyInterval, path.c_str());
            if (interval >= 500 && interval <= 60000) config.intervalMs[i] = interval;
            intervals[i] = CreateWindowExW(0, L"EDIT", std::to_wstring(config.intervalMs[i]).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER, 0, 0, 70, 26, hwnd,
                HMENU(INT_PTR(IntervalFirst + i)), nullptr, nullptr);
            ui::StyleControl(intervals[i], font); SendMessageW(intervals[i], EM_SETLIMITTEXT, 5, 0);
        }
        toggle = CreateWindowExW(0, L"BUTTON", L"启用自动恢复", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 142, 26, hwnd, HMENU(Toggle), nullptr, nullptr);
        ui::StyleControl(toggle, font, ui::ButtonStyle::Primary);
        loading = false; Layout(); SetTimer(hwnd, 1, 100, nullptr); return 0;
    }
    case WM_SIZE: Layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto target = BeginPaint(hwnd, &paint); RECT rect{}; GetClientRect(hwnd, &rect);
        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = buffer && rect.right > 0 && rect.bottom > 0 ? CreateCompatibleBitmap(target, rect.right, rect.bottom) : nullptr;
        HGDIOBJ previous = bitmap ? SelectObject(buffer, bitmap) : nullptr;
        const auto dc = bitmap ? buffer : target;
        ui::Card(dc, rect);
        ui::Text(dc, L"自动恢复", {8, 2, rect.right - 8, 28}, bold);
        const wchar_t* labels[] = {L"血量低于", L"蓝量低于", L"能量护盾低于"};
        const int labelWidth = 100;
        for (int i = 0; i < 3; ++i) {
            const int y = 34 + i * 30;
            ui::Text(dc, labels[i], {8, y, labelWidth, y + 26}, font);
            ui::Text(dc, L"% 使用", {labelWidth + 43, y, labelWidth + 88, y + 26}, font);
            ui::Text(dc, L"频率(ms):", {256, y, 324, y + 26}, font);
        }
        if (bitmap) {
            BitBlt(target, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous); DeleteObject(bitmap);
        }
        if (buffer) DeleteDC(buffer);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_TIMER:
        Consume();
        if (!busy && !done && GetTickCount64() >= nextPoll && (visible || state.enabled)) {
            nextPoll = GetTickCount64() + 500; StartRequest(false);
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp), notification = HIWORD(wp);
        if (loading) return 0;
        if (id == Toggle && notification == BN_CLICKED && !busy) {
            if (state.enabled) { recovery::Config stop = config; stop.enabled = 0; StartRequest(true, stop); }
            else {
                std::string error;
                if (mayEnable && !mayEnable(error)) message = Wide(error);
                else if (SaveSettings()) {
                    auto requested = config; requested.enabled = 1;
                    if (!recovery::Valid(requested)) message = L"请至少设置一项大于 0% 的恢复规则";
                    else StartRequest(true, requested);
                }
                if (!busy && report && !message.empty()) report(message);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (!state.enabled && ((id >= EditFirst && id < EditFirst + 3 && notification == EN_KILLFOCUS) ||
            (id >= IntervalFirst && id < IntervalFirst + 3 && notification == EN_KILLFOCUS) ||
            (id >= ChoiceFirst && id < ChoiceFirst + 3 && notification == CBN_SELCHANGE))) {
            if (SaveSettings()) message = L"恢复规则已保存，点击启用后生效";
            else if (report) report(message);
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        break;
    }
    case WM_DESTROY: KillTimer(hwnd, 1); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace
void CreatePanel(HWND parent, std::function<bool(std::string&)> canEnable, std::function<void(const std::wstring&)> log) {
    mayEnable = std::move(canEnable);
    report = std::move(log);
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Procedure;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"Poe2RecoveryPanel";
    RegisterClassW(&cls);
    CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 348, kHeight, parent, nullptr, cls.hInstance, nullptr);
}
void ShowPanel(bool show) {
    visible = show;
    if (panel) ShowWindow(panel, show ? SW_SHOW : SW_HIDE);
}
void MovePanel(int x, int y, int width) {
    if (panel) SetWindowPos(panel, nullptr, x, y, width, kHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}
void Shutdown() {
    if (shuttingDown) return;
    shuttingDown = true;
    if (panel) KillTimer(panel, 1);
    if (worker.joinable()) worker.join();
    // Stop before closing the Controller. A lost reply is still protected by
    // the server's ownership check once the managed connection is closed.
    try {
        const auto connection = pipe::GetConnectionSnapshot();
        if (connection.ready && connection.gamePid == app::Config().targetPid) {
            pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
            if (client.Initialize() && client.SessionId() == connection.sessionId) {
                auto stopped = config; stopped.enabled = 0;
                packet::Request request{}; request.operation = packet::Operation::ConfigureRecovery;
                request.length = sizeof(stopped); std::memcpy(request.bytes, &stopped, sizeof(stopped));
                packet::Message command(request); std::vector<uint8_t> reply;
                client.Request(&command, MT_PacketReply, reply, 1500);
            }
        }
    } catch (...) {}
    for (const auto handle : {font, small, bold}) if (handle) DeleteObject(handle);
    panel = nullptr;
}
} // namespace recoveryui
