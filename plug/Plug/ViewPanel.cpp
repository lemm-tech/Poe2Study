#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ViewPanel.h"
#include "UiTheme.h"
#include "NamedPipeClie.h"
#include "../Common/AppConfig.h"
#include "../Common/PacketProtocol.h"
#include "../Common/ViewControls.h"
#include <atomic>
#include <cwchar>
#include <thread>
#include <utility>

namespace viewui {
namespace {
HWND panel = nullptr, toggle = nullptr, edit = nullptr, reveal = nullptr;
HFONT font = nullptr;
std::function<bool(std::string&)> mayChange;
std::function<void(const std::string&)> report;
viewcontrols::Config config;
viewcontrols::Snapshot state;
bool visible = false, stopping = false, loading = false, uncertain = true, applyPending = false;
uint64_t nextPoll = 0;
std::thread worker;
std::atomic_bool busy{false}, done{false};
packet::Operation pending = packet::Operation::ViewStatus;
enum : int { Toggle = 9400, Zoom = 9401, Reveal = 9402 };
struct Result {
    bool received = false;
    packet::Operation operation = packet::Operation::ViewStatus;
    packet::Reply reply;
    pipe::ConnectionSnapshot connection;
    std::string error;
} result;
bool Same(const pipe::ConnectionSnapshot& a, const pipe::ConnectionSnapshot& b) {
    return a.ready && b.ready && a.gamePid == b.gamePid && a.sessionId == b.sessionId && a.controllerId == b.controllerId;
}
void Enabled(HWND window, bool value) {
    if ((IsWindowEnabled(window) != FALSE) != value) EnableWindow(window, value);
}
void Refresh() {
    const bool changing = busy && pending != packet::Operation::ViewStatus;
    const auto check = state.enabled ? BST_CHECKED : BST_UNCHECKED;
    if (SendMessageW(toggle, BM_GETCHECK, 0, 0) != check) SendMessageW(toggle, BM_SETCHECK, check, 0);
    Enabled(toggle, !stopping && !uncertain && !changing);
    Enabled(edit, !stopping && !changing);
    Enabled(reveal, !stopping && !uncertain && !changing);
}
bool ReadZoom(viewcontrols::Config& out) {
    wchar_t text[32]{}, *end = nullptr;
    GetWindowTextW(edit, text, _countof(text));
    out = config; out.maximum = std::wcstof(text, &end);
    if (!text[0] || !end || *end || !viewcontrols::Valid(out)) {
        if (report) report("缩放上限请填写 0.25–3.00");
        return false;
    }
    return true;
}
bool Save(const viewcontrols::Config& value) {
    wchar_t text[32]{}; swprintf_s(text, L"%.2f", value.maximum);
    if (!WritePrivateProfileStringW(L"view", L"maximum", text, app::Config().RuntimeFileW(L"view.ini").c_str())) {
        if (report) report("缩放上限保存失败");
        return false;
    }
    config.maximum = value.maximum;
    return true;
}
void Start(packet::Operation operation, viewcontrols::Config requested = {}) {
    if (busy || done || stopping) return;
    const auto connection = pipe::GetConnectionSnapshot();
    if (!connection.ready || connection.gamePid != app::Config().targetPid || !app::Config().TargetAlive()) {
        state = {}; uncertain = true; Refresh();
        if (operation != packet::Operation::ViewStatus && report) report("请先连接当前角色的新版 Monitor.dll");
        return;
    }
    if (worker.joinable()) worker.join();
    pending = operation; busy = true; Refresh();
    try {
        worker = std::thread([operation, requested, connection] {
            Result output; output.operation = operation; output.connection = connection;
            try {
                pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
                if (!client.Initialize() || client.SessionId() != connection.sessionId || client.ClientId() != connection.controllerId ||
                    !Same(connection, pipe::GetConnectionSnapshot())) output.error = "视角通信会话已变化，请重新操作";
                else {
                    packet::Request request; request.operation = operation;
                    if (operation == packet::Operation::ConfigureView) {
                        request.length = sizeof(requested); std::memcpy(request.bytes, &requested, sizeof(requested));
                    }
                    packet::Message command(request); std::vector<uint8_t> body;
                    if (!client.Request(&command, MT_PacketReply, body, 2500) || body.size() != sizeof(output.reply))
                        output.error = "视角回执未收到，将重新查询状态";
                    else {
                        std::memcpy(&output.reply, body.data(), sizeof(output.reply));
                        output.received = output.reply.version == packet::kVersion && output.reply.objectLength == sizeof(state);
                        if (!output.received) output.error = "视角回执格式无效，请使用配套的新版 DLL";
                    }
                }
            } catch (const std::exception& error) { output.error = error.what(); }
            catch (...) { output.error = "视角通信出现异常"; }
            result = std::move(output); done = true;
        });
    } catch (...) { busy = false; Refresh(); if (report) report("无法创建视角通信线程"); }
}
void Consume() {
    if (!done.exchange(false)) return;
    if (worker.joinable()) worker.join();
    busy = false;
    if (!Same(result.connection, pipe::GetConnectionSnapshot())) {
        state = {}; uncertain = true;
        if (result.operation != packet::Operation::ViewStatus && report) report("通信会话已变化，请重新操作视角设置");
    } else if (!result.received) {
        uncertain = true;
        if (result.operation != packet::Operation::ViewStatus && report) report(result.error);
    } else {
        std::memcpy(&state, result.reply.object, sizeof(state)); uncertain = false;
        if (result.operation != packet::Operation::ViewStatus && report) report(result.reply.detail);
    }
    nextPoll = GetTickCount64() + 1000;
    Refresh();
}
void ApplyEditedLimit() {
    if (!applyPending || busy || done || uncertain || stopping) return;
    applyPending = false;
    if (state.enabled) {
        auto requested = config; requested.enabled = 1;
        Start(packet::Operation::ConfigureView, requested);
    }
}
void Layout() {
    if (!panel || !edit) return;
    RECT rect{}; GetClientRect(panel, &rect);
    SetWindowPos(toggle, nullptr, 12, 10, rect.right - 24, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(edit, nullptr, 12, 48, 82, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(reveal, nullptr, 12, 100, rect.right - 24, 40, SWP_NOZORDER | SWP_NOACTIVATE);
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (ui::HandleMessage(hwnd, message, wp, lp, themed)) return themed;
    switch (message) {
    case WM_CREATE: {
        panel = hwnd; loading = true; font = ui::Font(14);
        wchar_t saved[32]{};
        const auto path = app::Config().RuntimeFileW(L"view.ini");
        wchar_t legacy[32]{};
        GetPrivateProfileStringW(L"view", L"zoom", L"1.00", legacy, _countof(legacy), path.c_str());
        GetPrivateProfileStringW(L"view", L"maximum", legacy, saved, _countof(saved), path.c_str());
        wchar_t* end = nullptr; config.maximum = std::wcstof(saved, &end);
        if (!end || *end || !viewcontrols::Valid(config)) config.maximum = 1.0f;
        wchar_t text[32]{}; swprintf_s(text, L"%.2f", config.maximum);
        toggle = CreateWindowExW(0, L"BUTTON", L"修改缩放上限", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 1, 1, hwnd, HMENU(Toggle), nullptr, nullptr);
        edit = CreateWindowExW(0, L"EDIT", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER,
            0, 0, 1, 1, hwnd, HMENU(Zoom), nullptr, nullptr);
        reveal = CreateWindowExW(0, L"BUTTON", L"小地图全显", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 1, 1, hwnd, HMENU(Reveal), nullptr, nullptr);
        for (const auto control : {toggle, edit, reveal}) ui::StyleControl(control, font);
        SendMessageW(edit, EM_SETLIMITTEXT, 8, 0);
        loading = false; Layout(); Refresh(); SetTimer(hwnd, 1, 100, nullptr); return 0;
    }
    case WM_SIZE: Layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(hwnd, &paint); RECT rect{}; GetClientRect(hwnd, &rect);
        ui::Card(dc, rect); ui::Text(dc, L"倍", {104, 48, rect.right - 12, 78}, font);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_TIMER:
        Consume();
        ApplyEditedLimit();
        if (!busy && !done && (visible || state.enabled) && GetTickCount64() >= nextPoll) {
            nextPoll = GetTickCount64() + 1000; Start(packet::Operation::ViewStatus);
        }
        return 0;
    case WM_COMMAND: {
        if (loading) return 0;
        const int id = LOWORD(wp), notification = HIWORD(wp);
        if ((id == Toggle || id == Reveal) && notification == BN_CLICKED) {
            if (busy || done) { Refresh(); return 0; }
            const bool disabling = id == Toggle && state.enabled;
            std::string error;
            if (!disabling && mayChange && !mayChange(error)) { if (report) report(error); Refresh(); return 0; }
            if (id == Reveal) { Start(packet::Operation::RevealMinimap); return 0; }
            auto requested = config; requested.enabled = disabling ? 0 : 1;
            if (disabling || (ReadZoom(requested) && Save(requested))) {
                requested.enabled = disabling ? 0 : 1;
                Start(packet::Operation::ConfigureView, requested);
            }
            Refresh(); return 0;
        }
        if (id == Zoom && notification == EN_KILLFOCUS) {
            auto edited = config;
            if (ReadZoom(edited) && Save(edited)) {
                applyPending = state.enabled != 0;
                ApplyEditedLimit();
            }
            return 0;
        }
        break;
    }
    case WM_DESTROY: KillTimer(hwnd, 1); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}
void CreatePanel(HWND parent, std::function<bool(std::string&)> canChange, std::function<void(const std::string&)> log) {
    mayChange = std::move(canChange); report = std::move(log);
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Procedure;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"Poe2ViewPanel"; RegisterClassW(&cls);
    CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 320, 194, parent, nullptr, cls.hInstance, nullptr);
}
void MovePanel(int x, int y, int width, int height) {
    if (panel) SetWindowPos(panel, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void ShowPanel(bool show) { visible = show; if (panel) ShowWindow(panel, show ? SW_SHOW : SW_HIDE); }
void Shutdown() {
    stopping = true;
    if (panel) KillTimer(panel, 1);
    if (worker.joinable()) worker.join();
    try {
        const auto connection = pipe::GetConnectionSnapshot();
        if (connection.ready && connection.gamePid == app::Config().targetPid) {
            pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
            if (client.Initialize() && client.SessionId() == connection.sessionId && client.ClientId() == connection.controllerId) {
                auto stopped = config; stopped.enabled = 0;
                packet::Request request; request.operation = packet::Operation::ConfigureView;
                request.length = sizeof(stopped); std::memcpy(request.bytes, &stopped, sizeof(stopped));
                packet::Message command(request); std::vector<uint8_t> body;
                client.Request(&command, MT_PacketReply, body, 1500);
            }
        }
    } catch (...) {}
    if (font) DeleteObject(font);
    panel = nullptr;
}
}
