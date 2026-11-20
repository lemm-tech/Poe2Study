#include "SessionLauncher.h"
#include "../Common/AppConfig.h"
#include "GameLauncher.h"
#include "../Common/InstanceMutex.h"
#include "../Common/LocateWindow.h"
#include "UiTheme.h"
#include "SkillSlots.h"
#include "../Common/GameBuild.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cerrno>
#include <set>
#include <map>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace sessions {
namespace {
constexpr int kList = 5100, kRefresh = 5101, kRelease = 5102, kOpen = 5103, kLocate = 5104, kAuto = 5105;
constexpr int kGamePath = 5106, kBrowse = 5107, kLaunch = 5108;
struct Game { Selection target; bool loaded = false; };
using ProcessKey = std::pair<DWORD,uint64_t>;
ProcessKey Key(const Selection& s) { return {s.pid, s.start}; }
std::wstring ReadName(const Selection& target, const std::string& moduleName) {
    if (!gametarget::Alive(target.pid, target.start)) return {};
    ola::OlaDriver driver;
    if (!driver.InitializeLocal(target.pid)) return {};
    mem::GameMemory memory(driver);
    const auto base = memory.ModuleBase(moduleName);
    std::string name, error;
    if (!base || !gamebuild::Verify(base, [&memory](uint64_t address, void* data, size_t length) {
            return memory.ReadInto(address, data, length);
        }, error) || !poe::ReadCharacterName(memory, base, name, error) ||
        !gametarget::Alive(target.pid, target.start)) return {};
    return winutil::Utf8ToWide(name);
}
bool FocusPanel(const Selection& s) {
    HWND panel = FindPanel(s);
    if (!panel) return false;
    RestorePanel(panel);
    return true;
}
std::vector<Game> Enumerate() {
    std::vector<Game> games;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return games;
    const auto wanted = winutil::Utf8ToWide(app::Config().gameModuleName);
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snapshot, &pe)) do {
        if (_wcsicmp(pe.szExeFile, wanted.c_str())) continue;
        const auto start = gametarget::CreationTime(pe.th32ProcessID);
        if (!start) continue;
        Game game; game.target = {pe.th32ProcessID, start, gametarget::Window(pe.th32ProcessID)};
        HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
        if (modules != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{}; me.dwSize = sizeof(me);
            if (Module32FirstW(modules, &me)) do {
                if (!_wcsicmp(me.szModule, L"Monitor.dll")) { game.loaded = true; break; }
            } while (Module32NextW(modules, &me));
            CloseHandle(modules);
        }
        games.push_back(std::move(game));
    } while (Process32NextW(snapshot, &pe));
    CloseHandle(snapshot);
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return a.target.pid < b.target.pid; });
    return games;
}
HWND Label(HWND parent, int id, const wchar_t* text, int height = 14, bool muted = false) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    ui::StyleLabel(h, ui::Font(height, height > 16 ? FW_SEMIBOLD : FW_NORMAL), muted);
    return h;
}
HWND Button(HWND parent, int id, const wchar_t* text, ui::ButtonStyle style = ui::ButtonStyle::Secondary) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    ui::StyleControl(h, ui::Font(), style);
    return h;
}
struct Launcher {
    HWND window = nullptr, list = nullptr, status = nullptr, automatic = nullptr, gamePath = nullptr;
    std::wstring settingsFile = gametarget::ModuleDirectory() + L"launcher.ini";
    std::wstring savedPath;
    std::vector<Game> games;
    std::set<std::pair<DWORD,uint64_t>> unlocked;
    // Only the UI thread accesses the cache. The worker owns its result batch
    // until FinishNames joins it. A successful process identity is never read again.
    std::map<ProcessKey, std::wstring> characterNames;
    std::vector<std::pair<ProcessKey, std::wstring>> nameResults;
    std::atomic_bool stopNames{false};
    bool namesBusy = false;
    std::thread nameReader;
    std::wstring notice;
    void QueueNames() {
        if (namesBusy || stopNames) return;
        std::vector<Selection> pending;
        for (const auto& game : games) if (!characterNames.count(Key(game.target))) pending.push_back(game.target);
        if (pending.empty()) return;
        namesBusy = true;
        nameResults.clear();
        const auto module = app::Config().gameModuleName;
        nameReader = std::thread([this, pending = std::move(pending), module] {
            for (const auto& target : pending) {
                if (stopNames) break;
                try {
                    auto name = ReadName(target, module);
                    if (!name.empty()) nameResults.emplace_back(Key(target), std::move(name));
                } catch (...) { /* An unavailable name is retried on the next 5s tick. */ }
            }
            if (!stopNames) PostMessageW(window, WM_APP + 72, 0, 0);
        });
    }
    void FinishNames() {
        if (nameReader.joinable()) nameReader.join();
        namesBusy = false;
        for (const auto& result : nameResults) {
            for (size_t i = 0; i < games.size(); ++i) {
                if (Key(games[i].target) != result.first) continue;
                characterNames.emplace(result.first, result.second);
                ListView_SetItemText(list, int(i), 2, const_cast<wchar_t*>(result.second.c_str()));
                break;
            }
        }
        nameResults.clear();
    }
    void StopNames() {
        stopNames.store(true);
        if (nameReader.joinable()) nameReader.join();
    }
    ~Launcher() { StopNames(); }
    int Selected() const { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
    void SetNotice(const std::wstring& text) { notice = text; SetWindowTextW(status, text.c_str()); }
    std::wstring EditedPath() const {
        const int length = GetWindowTextLengthW(gamePath);
        std::wstring text(size_t(length) + 1, L'\0');
        GetWindowTextW(gamePath, text.data(), length + 1); text.resize(length);
        return game::NormalizePath(text);
    }
    bool SavePath() {
        KillTimer(window, 2);
        const auto path = EditedPath();
        if (path == savedPath) return true;
        DWORD error = 0;
        if (!game::SavePath(settingsFile, path, error)) {
            SetNotice(L"游戏路径未保存（" + game::ErrorText(error) + L"）；仍可使用当前路径启动。"); return false;
        }
        savedPath = path;
        return true;
    }
    void Browse() {
        std::vector<wchar_t> path(32768);
        const auto current = EditedPath();
        if (current.size() < path.size()) std::copy(current.begin(), current.end(), path.begin());
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
        dialog.lpstrFilter = L"游戏可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
        dialog.lpstrFile = path.data(); dialog.nMaxFile = DWORD(path.size());
        dialog.lpstrTitle = L"选择游戏可执行文件";
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
            SetWindowTextW(gamePath, path.data());
            if (SavePath()) SetNotice(L"游戏路径已保存，点击“启动游戏”即可打开新的游戏进程。");
        } else if (const auto error = CommDlgExtendedError()) {
            SetNotice(L"打开文件选择窗口失败（对话框错误 " + std::to_wstring(error) + L"）");
        }
    }
    void LaunchGame() {
        Refresh();
        const auto result = game::Start(EditedPath(), gametarget::ModuleDirectory());
        if (!result.pid) { SetNotice(result.error); return; }
        SetWindowTextW(gamePath, result.path.c_str());
        const bool saved = SavePath();
        const auto saveWarning = saved ? std::wstring() : L" " + notice;
        Refresh();
        SetNotice(L"已启动游戏进程，PID " + std::to_wstring(result.pid) +
                  L"；列表每 5 秒自动更新，游戏窗口就绪后可打开独立面板。" + saveWarning);
    }
    void Refresh(bool releaseAll = false) {
        Selection selected;
        int index = Selected();
        if (index >= 0 && size_t(index) < games.size()) selected = games[index].target;
        games = Enumerate();
        std::set<ProcessKey> active;
        for (const auto& game : games) active.insert(Key(game.target));
        for (auto it = characterNames.begin(); it != characterNames.end();) {
            if (!active.count(it->first)) it = characterNames.erase(it); else ++it;
        }
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        unsigned closed = 0; std::wstring errors;
        const bool automaticRelease = SendMessageW(automatic, BM_GETCHECK, 0, 0) == BST_CHECKED;
        for (size_t i = 0; i < games.size(); ++i) {
            const auto& game = games[i]; const auto& s = game.target;
            const auto key = std::make_pair(s.pid, s.start);
            if (releaseAll || (automaticRelease && !unlocked.count(key))) {
                const auto released = instance::Release(s.pid, s.start);
                closed += released.closed;
                if (released.ok) unlocked.insert(key);
                else errors += L"PID " + std::to_wstring(s.pid) + L"：" + released.error + L"；";
            }
            auto locks = instance::Inspect(s.pid, s.start);
            std::wstring pid = std::to_wstring(s.pid);
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = int(i); item.pszText = pid.data();
            ListView_InsertItem(list, &item);
            wchar_t hwndText[40]{}; swprintf_s(hwndText, L"0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.window)));
            ListView_SetItemText(list, int(i), 1, hwndText);
            const auto name = characterNames.find(key);
            const std::wstring displayName = name == characterNames.end() ? L"等待角色名（每5秒只读获取）" : name->second;
            ListView_SetItemText(list, int(i), 2, const_cast<wchar_t*>(displayName.c_str()));
            const HWND panel = FindPanel(s);
            const std::wstring state = panel ? (IsWindowVisible(panel) ? L"独立面板已启动" : L"后台运行（双击恢复）") :
                game.loaded ? L"已加载 DLL（打开时校验）" : L"未注入";
            ListView_SetItemText(list, int(i), 3, const_cast<wchar_t*>(state.c_str()));
            const std::wstring lockState = !locks.ok ? L"读取失败" : locks.matches.empty() ? L"已解除" :
                L"待解除（" + std::to_wstring(locks.matches.size()) + L"）";
            ListView_SetItemText(list, int(i), 4, const_cast<wchar_t*>(lockState.c_str()));
            if (s.pid == selected.pid && s.start == selected.start) {
                ListView_SetItemState(list, int(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
        if (Selected() < 0 && !games.empty()) ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, FALSE);
        if (!errors.empty()) SetNotice(errors);
        else if (closed) SetNotice(L"已关闭 " + std::to_wstring(closed) + L" 个多开互斥体，可以继续启动游戏。选中窗口后点击“注入并打开面板”。");
        else if (notice.empty() || releaseAll) SetNotice(games.empty() ?
            L"未找到游戏进程。可在上方填写游戏路径并启动，列表会自动更新。" :
            L"找到 " + std::to_wstring(games.size()) + L" 个游戏进程。每个 PID 使用独立面板、配置目录和通信管道。");
        EnableWindow(GetDlgItem(window, kOpen), Selected() >= 0);
    }
    void Open() {
        const int index = Selected();
        if (index < 0 || size_t(index) >= games.size()) return;
        const Selection s = games[index].target;
        if (!gametarget::Alive(s.pid, s.start)) {
            SetNotice(L"所选游戏已退出，请刷新列表。"); return;
        }
        if (FocusPanel(s)) { SetNotice(L"已恢复该游戏的独立面板，后台功能保持原状态。"); return; }
        if (!gametarget::Window(s.pid, s.window)) {
            SetNotice(L"所选游戏窗口尚未就绪，请稍后刷新列表。"); return;
        }
        wchar_t exe[32768]{};
        if (!GetModuleFileNameW(nullptr, exe, DWORD(_countof(exe)))) return;
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --target-pid " + std::to_wstring(s.pid) +
            L" --target-start " + std::to_wstring(s.start) + L" --target-hwnd " +
            std::to_wstring(reinterpret_cast<uintptr_t>(gametarget::Window(s.pid, s.window))) +
            L" --launcher-pid " + std::to_wstring(GetCurrentProcessId()) +
            L" --launcher-start " + std::to_wstring(gametarget::CreationTime(GetCurrentProcess())) +
            L" --launcher-hwnd " + std::to_wstring(reinterpret_cast<uintptr_t>(window));
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            gametarget::ModuleDirectory().c_str(), &startup, &child)) {
            SetNotice(L"启动面板失败（错误 " + std::to_wstring(GetLastError()) + L"）"); return;
        }
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        SetNotice(L"正在连接 PID " + std::to_wstring(s.pid) + L"；注入和身份校验成功后将打开主面板。");
    }
    void Layout() {
        RECT r{}; GetClientRect(window, &r); const int w = r.right, h = r.bottom;
        MoveWindow(GetDlgItem(window, 5200), 24, 20, w - 48, 36, TRUE);
        MoveWindow(GetDlgItem(window, 5201), 24, 66, w - 48, 44, TRUE);
        MoveWindow(GetDlgItem(window, 5203), 24, 128, 76, 28, TRUE);
        MoveWindow(gamePath, 104, 120, w - 356, 36, TRUE);
        MoveWindow(GetDlgItem(window, kBrowse), w - 240, 120, 88, 36, TRUE);
        MoveWindow(GetDlgItem(window, kLaunch), w - 140, 120, 116, 36, TRUE);
        MoveWindow(list, 24, 172, w - 48, h - 306, TRUE);
        MoveWindow(GetDlgItem(window, kRefresh), 24, h - 116, 100, 36, TRUE);
        MoveWindow(GetDlgItem(window, kRelease), 136, h - 116, 180, 36, TRUE);
        MoveWindow(GetDlgItem(window, kLocate), 328, h - 116, 132, 36, TRUE);
        MoveWindow(GetDlgItem(window, kOpen), w - 252, h - 116, 228, 36, TRUE);
        MoveWindow(automatic, 24, h - 72, 295, 24, TRUE);
        MoveWindow(status, 24, h - 44, w - 48, 32, TRUE);
    }
};
LRESULT CALLBACK LauncherProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = reinterpret_cast<Launcher*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window = h; SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    LRESULT themed = 0;
    if (ui::HandleMessage(h, msg, wp, lp, themed)) return themed;
    switch (msg) {
    case WM_CREATE: {
        ui::ApplyWindow(h, false);
        Label(h, 5200, L"游戏窗口 · 多开管理", 24);
        Label(h, 5201, L"选择要控制的游戏窗口 → 注入并验证通信 → 打开独立配置面板。\r\n自动解除仅处理已确认的三个多开互斥体；同一游戏只允许一个控制面板。", 14, true);
        Label(h, 5203, L"游戏路径");
        self->gamePath = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, h, reinterpret_cast<HMENU>(INT_PTR(kGamePath)), GetModuleHandleW(nullptr), nullptr);
        ui::StyleControl(self->gamePath, ui::Font());
        SendMessageW(self->gamePath, EM_SETLIMITTEXT, 32760, 0);
        SendMessageW(self->gamePath, EM_SETCUEBANNER, TRUE, LPARAM(L"选择或填写游戏 .exe 的完整路径（自动保存）"));
        self->savedPath = game::LoadPath(self->settingsFile);
        SetWindowTextW(self->gamePath, self->savedPath.c_str());
        Button(h, kBrowse, L"浏览…"); Button(h, kLaunch, L"启动游戏", ui::ButtonStyle::Primary);
        self->list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, h, reinterpret_cast<HMENU>(INT_PTR(kList)), GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(self->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ui::StyleControl(self->list, ui::Font());
        const wchar_t* names[]{L"PID", L"窗口 HWND", L"角色名称", L"注入 / 面板状态", L"多开限制"};
        const int widths[]{72,125,240,210,140};
        for (int i = 0; i < 5; ++i) {
            LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.cx = widths[i]; c.pszText = const_cast<wchar_t*>(names[i]);
            ListView_InsertColumn(self->list, i, &c);
        }
        Button(h, kRefresh, L"刷新列表"); Button(h, kRelease, L"解除全部多开限制");
        Button(h, kLocate, L"定位游戏窗口"); Button(h, kOpen, L"注入并打开独立面板", ui::ButtonStyle::Primary);
        self->automatic = CreateWindowExW(0, L"BUTTON", L"自动解除新游戏进程的多开限制", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, h, reinterpret_cast<HMENU>(INT_PTR(kAuto)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(self->automatic, WM_SETFONT, WPARAM(ui::Font()), TRUE);
        SendMessageW(self->automatic, BM_SETCHECK, BST_CHECKED, 0);
        self->status = Label(h, 5202, L"正在枚举游戏窗口…", 13, true);
        self->Layout(); SetTimer(h, 1, 5000, nullptr); PostMessageW(h, WM_APP + 70, 0, 0); return 0;
    }
    case WM_APP + 70: self->Refresh(); return 0;
    case WM_TIMER:
        if (wp == 2) self->SavePath();
        else if (wp == 1) { self->Refresh(); self->QueueNames(); }
        return 0;
    case WM_APP + 72: self->FinishNames(); return 0;
    case WM_SIZE: self->Layout(); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {940, 500}; return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kGamePath:
            if (HIWORD(wp) == EN_CHANGE) SetTimer(h, 2, 600, nullptr);
            else if (HIWORD(wp) == EN_KILLFOCUS) self->SavePath();
            break;
        case kBrowse: self->Browse(); break;
        case kLaunch: self->LaunchGame(); break;
        case kRefresh: self->Refresh(); break;
        case kRelease: self->Refresh(true); break;
        case kAuto: if (SendMessageW(self->automatic, BM_GETCHECK, 0, 0) == BST_CHECKED) self->Refresh(true); break;
        case kOpen: self->Open(); break;
        case kLocate: {
            const int index = self->Selected();
            if (index >= 0 && size_t(index) < self->games.size()) {
                const auto& s = self->games[index].target;
                if (gametarget::Alive(s.pid, s.start)) winutil::ForceForeground(gametarget::Window(s.pid, s.window));
            }
            break;
        }} return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->idFrom == kList && reinterpret_cast<NMHDR*>(lp)->code == NM_DBLCLK) self->Open();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT p{}; HDC dc = BeginPaint(h, &p); FillRect(dc, &p.rcPaint, ui::Background()); EndPaint(h, &p); return 0; }
    case WM_CLOSE: self->SavePath(); DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h, 1); KillTimer(h, 2); self->StopNames(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
struct Connecting {
    HWND window = nullptr;
    PanelGuard* lifetime = nullptr;
    std::atomic_bool cancelled{false};
    std::thread thread;
    std::wstring error;
    bool success = false;
    void CancelAndJoin() {
        cancelled.store(true);
        if (thread.joinable()) thread.join();
    }
};
constexpr UINT_PTR kConnectingLifetimeTimer = 0x1F11;
LRESULT CALLBACK ConnectProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* c = reinterpret_cast<Connecting*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        c = reinterpret_cast<Connecting*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c)); c->window = h;
    }
    LRESULT themed = 0;
    if (ui::HandleMessage(h, msg, wp, lp, themed)) return themed;
    switch (msg) {
    case WM_CREATE: {
        if (!c->lifetime || !SetTimer(h, kConnectingLifetimeTimer, 250, nullptr)) return -1;
        ui::ApplyWindow(h, false);
        MoveWindow(Label(h, 1, L"正在连接所选游戏", 22), 24, 20, 650, 36, TRUE);
        const auto detail = L"正在检查 DLL、注入并验证专属通信通道。目标 PID：" + std::to_wstring(app::Config().targetPid) +
            L"\r\n成功后自动打开主面板；关闭窗口可取消等待。";
        MoveWindow(Label(h, 2, detail.c_str(), 14, true), 24, 70, 650, 60, TRUE);
        MoveWindow(Button(h, IDCANCEL, L"取消等待"), 530, 150, 140, 34, TRUE); return 0;
    }
    case WM_COMMAND: if (LOWORD(wp) != IDCANCEL) break; [[fallthrough]];
    case WM_CLOSE:
        c->cancelled.store(true);
        SetWindowTextW(GetDlgItem(h, 2), L"正在等待当前加载步骤结束并断开通信，主面板不会打开。");
        EnableWindow(GetDlgItem(h, IDCANCEL), FALSE); return 0;
    case WM_TIMER:
        if (wp != kConnectingLifetimeTimer) break;
        if (!c->lifetime->Active()) {
            c->CancelAndJoin();
            DestroyWindow(h);
        }
        return 0;
    case WM_APP + 71:
        if (c->thread.joinable()) c->thread.join();
        if (!c->success && !c->cancelled && c->lifetime->Active())
            MessageBoxW(h, c->error.c_str(), L"连接未完成", MB_OK | MB_ICONERROR);
        if (IsWindow(h)) DestroyWindow(h);
        return 0;
    case WM_DESTROY: KillTimer(h, kConnectingLifetimeTimer); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT p{}; HDC dc = BeginPaint(h, &p); FillRect(dc, &p.rcPaint, ui::Background()); EndPaint(h, &p); return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace
bool RegisterPanel(HWND panel, const Selection& selection) {
    return SetPropW(panel, gametarget::PanelLockName(selection.pid, selection.start).c_str(),
                    reinterpret_cast<HANDLE>(INT_PTR(1))) != FALSE;
}
HWND FindPanel(const Selection& selection) {
    struct Search { std::wstring key; HWND panel = nullptr; } search{
        gametarget::PanelLockName(selection.pid, selection.start)};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(data);
        wchar_t cls[64]{}; GetClassNameW(window, cls, int(_countof(cls)));
        if (!wcscmp(cls, L"Poe2NavGui") && GetPropW(window, search.key.c_str())) {
            search.panel = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (search.panel) return search.panel;
    // A connecting dialog or older panel still has the original session title.
    return FindWindowW(nullptr, gametarget::PanelTitle(selection.pid, selection.start).c_str());
}
void RestorePanel(HWND panel) {
    ShowWindow(panel, SW_RESTORE);
    ShowOwnedPopups(panel, TRUE);
    SetForegroundWindow(panel);
    const HWND popup = GetLastActivePopup(panel);
    if (popup != panel && IsWindowVisible(popup)) SetForegroundWindow(popup);
}
std::wstring ReadCharacterName(const Selection& selection, const std::string& moduleName) {
    return ReadName(selection, moduleName);
}
bool ParseSelection(Selection& s, std::wstring& error) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { error = L"无法解析启动参数"; return false; }
    bool pid = false, start = false, window = false, valid = true;
    bool launcherPid = false, launcherStart = false, launcherWindow = false;
    for (int i = 1; i < argc && valid; ++i) {
        const std::wstring key = argv[i];
        if (i + 1 >= argc || (key != L"--target-pid" && key != L"--target-start" && key != L"--target-hwnd" &&
            key != L"--launcher-pid" && key != L"--launcher-start" && key != L"--launcher-hwnd")) { valid = false; break; }
        const std::wstring raw = argv[++i];
        if (raw.empty() || raw.find_first_not_of(L"0123456789") != std::wstring::npos) { valid = false; break; }
        errno = 0; wchar_t* end = nullptr; const auto value = _wcstoui64(raw.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end || !value) { valid = false; break; }
        if (key == L"--target-pid") { if (pid || value > MAXDWORD) valid = false; s.pid = DWORD(value); pid = true; }
        if (key == L"--target-start") { if (start) valid = false; s.start = value; start = true; }
        if (key == L"--target-hwnd") { if (window) valid = false; s.window = reinterpret_cast<HWND>(uintptr_t(value)); window = true; }
        if (key == L"--launcher-pid") { if (launcherPid || value > MAXDWORD) valid = false; s.launcherPid = DWORD(value); launcherPid = true; }
        if (key == L"--launcher-start") { if (launcherStart) valid = false; s.launcherStart = value; launcherStart = true; }
        if (key == L"--launcher-hwnd") { if (launcherWindow) valid = false; s.launcherWindow = reinterpret_cast<HWND>(uintptr_t(value)); launcherWindow = true; }
    }
    LocalFree(argv);
    if (!valid || (argc > 1 && (!pid || !start || !window)) ||
        ((launcherPid || launcherStart || launcherWindow) && !(launcherPid && launcherStart && launcherWindow))) {
        error = L"启动参数无效。请直接运行程序，在游戏窗口列表中选择目标。"; return false;
    }
    return true;
}
int RunLauncher(HINSTANCE instance, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = LauncherProc;
    cls.lpszClassName = L"Poe2SessionLauncher"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = ui::AppIcon(); cls.hIconSm = ui::AppIcon(true); RegisterClassExW(&cls);
    Launcher state;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"POE2 · 游戏窗口与多开管理", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 600, nullptr, nullptr, instance, &state);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return int(message.wParam);
}
bool RunConnectDialog(HINSTANCE instance, PanelGuard& guard, const Connect& connect) {
    if (!guard.Active()) return false;
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = ConnectProc;
    cls.lpszClassName = L"Poe2SessionConnecting"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = ui::AppIcon(); RegisterClassExW(&cls);
    Connecting state;
    state.lifetime = &guard;
    const auto title = gametarget::PanelTitle(app::Config().targetPid, app::Config().targetStart);
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 250, nullptr, nullptr, instance, &state);
    if (!h) return false;
    ShowWindow(h, SW_SHOW); UpdateWindow(h);
    if (!IsWindow(h) || !guard.Active()) {
        if (IsWindow(h)) DestroyWindow(h);
        return false;
    }
    state.thread = std::thread([&] {
        try { state.success = connect(state.cancelled, state.error); }
        catch (...) { state.error = L"连接过程发生异常，请检查该进程的独立日志。"; }
        PostMessageW(h, WM_APP + 71, 0, 0);
    });
    MSG message{};
    while (IsWindow(h)) {
        if (!guard.WaitForMessage()) { state.CancelAndJoin(); break; }
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) continue;
        if (message.message == WM_QUIT) { state.cancelled = true; break; }
        if (IsDialogMessageW(h, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (state.thread.joinable()) state.CancelAndJoin();
    if (IsWindow(h)) DestroyWindow(h);
    return state.success && !state.cancelled && guard.Active();
}
PanelGuard::~PanelGuard() {
    if (launcher) CloseHandle(launcher);
    if (process) CloseHandle(process);
    if (lock) CloseHandle(lock);
}
bool PanelGuard::Acquire(const Selection& s, std::wstring& error) {
    selection_ = s;
    lock = CreateMutexW(nullptr, FALSE, gametarget::PanelLockName(s.pid, s.start).c_str());
    const DWORD status = GetLastError();
    if (!lock) { error = L"无法创建独立面板锁"; return false; }
    if (status == ERROR_ALREADY_EXISTS) { FocusPanel(s); error = L"该游戏的配置面板已经打开或正在连接。"; return false; }
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, s.pid);
    if (!process || gametarget::CreationTime(process) != s.start || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        error = L"所选游戏已退出，请回到列表重新选择。"; return false;
    }
    if (s.launcherPid) {
        launcher = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, s.launcherPid);
        DWORD owner = 0;
        if (!launcher || gametarget::CreationTime(launcher) != s.launcherStart ||
            WaitForSingleObject(launcher, 0) != WAIT_TIMEOUT || !IsWindow(s.launcherWindow) ||
            !GetWindowThreadProcessId(s.launcherWindow, &owner) || owner != s.launcherPid) {
            error = L"启动此面板的多开管理已退出，请重新打开多开管理。"; return false;
        }
    }
    return true;
}
bool PanelGuard::Active() {
    if (!process || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        exitReason_ = "bound game process exited"; return false;
    }
    if (launcher) {
        DWORD owner = 0;
        if (WaitForSingleObject(launcher, 0) != WAIT_TIMEOUT || !IsWindow(selection_.launcherWindow) ||
            !GetWindowThreadProcessId(selection_.launcherWindow, &owner) || owner != selection_.launcherPid) {
            exitReason_ = "owning launcher closed"; return false;
        }
    }
    // Window recreation is allowed, minimizing/hiding is not an exit. A game
    // that has destroyed its last window may still linger before process exit.
    const HWND window = gametarget::Window(selection_.pid, selection_.window);
    if (window) { selection_.window = window; windowMissingAt_ = 0; }
    else {
        const auto now = GetTickCount64();
        if (!windowMissingAt_) windowMissingAt_ = now;
        if (now - windowMissingAt_ >= 2000) { exitReason_ = "bound game window closed"; return false; }
    }
    return true;
}
bool PanelGuard::WaitForMessage() {
    const HANDLE handles[] = {process, launcher};
    const DWORD count = launcher ? 2 : 1;
    while (Active()) {
        const DWORD result = MsgWaitForMultipleObjectsEx(count, handles, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0 + count) return Active();
        if (result == WAIT_TIMEOUT || result < WAIT_OBJECT_0 + count) continue;
        exitReason_ = "panel dependency wait failed"; return false;
    }
    return false;
}
} // namespace sessions
