#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <mutex>
#include <thread>
#include "ComboPanel.h"
#include "ComboConfig.h"
#include "ComboRunner.h"
#include "UseCases.h"
#include "UiTheme.h"
#pragma comment(lib, "comctl32.lib")

namespace combo {
namespace {
enum Control { Refresh = 7201, Skills, UsageBox, EnvironmentBox, Interval, Charge, Distance,
    Add, Update, Remove, Up, Down, Save, Rows };
HWND window = nullptr, list = nullptr, skillBox = nullptr, usageBox = nullptr, environmentBox = nullptr;
HWND intervalEdit = nullptr, chargeEdit = nullptr, distanceEdit = nullptr, statusLabel = nullptr, skillsLabel = nullptr;
HFONT font = nullptr;
HFONT titleFont = nullptr;
HWND titleLabel = nullptr;
std::vector<HWND> editingControls, labels;
std::vector<Row> rows;
std::vector<poe::SkillSlot> choices;
std::atomic_bool reading{false}, dirty{false};
std::atomic_bool navigationTestActive{false};
std::mutex scanMutex;
poe::SkillSlotsSnapshot scanned;
std::string scanError;
std::thread scanThread;
bool populating = false;
std::string lastRunnerText;
uint64_t lastRunnerCasts = UINT64_MAX;
std::wstring localStatus = L"正在获取当前技能槽…";
bool EnableIfChanged(HWND control, bool enabled) {
    if (!control || (IsWindowEnabled(control) != FALSE) == enabled) return false;
    EnableWindow(control, enabled);
    return true;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w;
}
void Say(const std::wstring& text) { localStatus = text; if (statusLabel) SetWindowTextW(statusLabel, text.c_str()); }
int Selected() { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
bool SaveRows() {
    std::string error;
    if (!SaveConfig(DefaultConfigPath(), rows, error)) { Say(L"保存失败：" + Wide(error)); return false; }
    Say(L"配置已保存到程序目录的 combo.ini"); return true;
}
void SetNumber(HWND edit, uint32_t value) { SetWindowTextW(edit, std::to_wstring(value).c_str()); }
void FillEditor(int index) {
    if (index < 0 || size_t(index) >= rows.size()) return;
    const auto& r = rows[size_t(index)];
    int selected = -1;
    for (size_t i = 0; i < choices.size(); ++i)
        if (choices[i].internalId == r.internalId) {
            if (selected == -1) selected = int(i);
            if (choices[i].slotId == r.slotHint) { selected = int(i); break; }
        }
    SendMessageW(skillBox, CB_SETCURSEL, selected, 0);
    SendMessageW(usageBox, CB_SETCURSEL, WPARAM(r.usage), 0);
    SendMessageW(environmentBox, CB_SETCURSEL, WPARAM(r.environment), 0);
    SetNumber(intervalEdit, r.intervalMs); SetNumber(chargeEdit, r.chargeMs); SetNumber(distanceEdit, r.distance);
}
void Populate(int selection) {
    populating = true; ListView_DeleteAllItems(list);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        std::wstring text = std::to_wstring(i + 1);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = int(i); item.pszText = text.data();
        ListView_InsertItem(list, &item);
        std::wstring cells[] = {Wide(r.name), r.usage == Usage::Once ? L"单次执行" : L"全局使用",
            r.environment == Environment::Encounter ? L"遇敌使用" : L"自行循环",
            std::to_wstring(r.intervalMs), std::to_wstring(r.chargeMs),
            r.environment == Environment::Encounter ? std::to_wstring(r.distance) : L"—"};
        for (int c = 0; c < 6; ++c) ListView_SetItemText(list, int(i), c + 1, cells[c].data());
    }
    if (selection >= 0 && size_t(selection) < rows.size()) {
        ListView_SetItemState(list, selection, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selection, FALSE);
    }
    populating = false; FillEditor(selection);
}
void ReadSkills() {
    if (reading.exchange(true)) return;
    if (scanThread.joinable()) scanThread.join();
    SetWindowTextW(skillsLabel, L"正在读取当前技能槽…");
    scanThread = std::thread([] {
        poe::SkillSlotsSnapshot result; std::string error;
        try { usecase::ScanSkillSlots(result, error); }
        catch (const std::exception& e) { error = e.what(); }
        { std::lock_guard<std::mutex> lock(scanMutex); scanned = std::move(result); scanError = std::move(error); }
        reading = false; dirty = true;
    });
}
void ApplySkills() {
    std::lock_guard<std::mutex> lock(scanMutex);
    SendMessageW(skillBox, CB_RESETCONTENT, 0, 0); choices.clear();
    for (const auto& skill : scanned.slots) if (skill.occupied && !skill.movement) {
        auto text = Wide(skill.localizedName.empty() ? skill.internalId : skill.localizedName) +
            L"〔槽位 " + std::to_wstring(skill.slotId) + L"〕";
        SendMessageW(skillBox, CB_ADDSTRING, 0, LPARAM(text.c_str())); choices.push_back(skill);
    }
    if (!choices.empty()) SendMessageW(skillBox, CB_SETCURSEL, 0, 0);
    const auto text = scanError.empty() ? L"当前角色：" + Wide(scanned.characterName) + L"；可选技能 " +
        std::to_wstring(choices.size()) + L" 项" : L"获取失败：" + Wide(scanError);
    SetWindowTextW(skillsLabel, text.c_str());
    FillEditor(Selected());
}
bool Number(HWND edit, uint32_t max, uint32_t& out) {
    wchar_t text[32]{}; GetWindowTextW(edit, text, 32);
    const std::wstring value(text);
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    try { const auto n = std::stoull(value); if (n > max) return false; out = uint32_t(n); return true; }
    catch (...) { return false; }
}
bool EditorRow(Row& row) {
    const auto selected = SendMessageW(skillBox, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR || size_t(selected) >= choices.size()) { Say(L"请先获取并选择技能名称"); return false; }
    const auto& choice = choices[size_t(selected)];
    row.internalId = choice.internalId; row.name = choice.localizedName.empty() ? choice.internalId : choice.localizedName;
    row.slotHint = choice.slotId;
    row.usage = Usage(SendMessageW(usageBox, CB_GETCURSEL, 0, 0));
    row.environment = Environment(SendMessageW(environmentBox, CB_GETCURSEL, 0, 0));
    if (!Number(intervalEdit, kMaxTimeMs, row.intervalMs) || !Number(chargeEdit, kMaxTimeMs, row.chargeMs) ||
        !Number(distanceEdit, 0x1000 * 23, row.distance)) {
        Say(L"时间请填写 0–600000ms，距离请填写 0–94208 的整数"); return false;
    }
    return true;
}
HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, bool editing = false) {
    HWND h = CreateWindowExW(0, cls, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 50, 26, parent, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    if (!wcscmp(cls, L"STATIC")) ui::StyleLabel(h, font, true, true);
    else ui::StyleControl(h, font, id == Save ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary);
    if (editing) editingControls.push_back(h);
    return h;
}
void Layout(HWND hwnd) {
    RECT r{}; GetClientRect(hwnd, &r); const int width = r.right, height = r.bottom;
    const int bottom = height - 264;
    MoveWindow(titleLabel, 28, 18, width - 220, 34, TRUE);
    MoveWindow(labels[0], 28, 58, width - 56, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, Refresh), width - 174, 24, 146, 34, TRUE);
    MoveWindow(skillsLabel, 28, 94, width - 56, 22, TRUE);
    MoveWindow(list, 28, 132, width - 56, (std::max)(140, height - 420), TRUE);
    const int ws[] = {width - 734, 124, 128, 122, 122, 122};
    const HWND fields[] = {skillBox, usageBox, environmentBox, intervalEdit, chargeEdit, distanceEdit};
    int x = 28;
    for (int i = 0; i < 6; ++i) {
        MoveWindow(labels[size_t(i) + 1], x, bottom + 18, ws[i], 22, TRUE);
        MoveWindow(fields[i], x, bottom + 44, ws[i], i < 3 ? 250 : 32, TRUE);
        x += ws[i] + 12;
    }
    const int ids[] = {Add, Update, Remove, Up, Down, Save};
    const int buttonWidths[] = {90, 98, 98, 68, 68, 100}; x = 28;
    for (int i = 0; i < 6; ++i) {
        MoveWindow(GetDlgItem(hwnd, ids[i]), x, bottom + 96, buttonWidths[i], 34, TRUE);
        x += buttonWidths[i] + 8;
    }
    MoveWindow(labels[7], 28, bottom + 146, width - 56, 42, TRUE);
    MoveWindow(statusLabel, 28, height - 44, width - 56, 26, TRUE);
    const int fixed[] = {52, 0, 112, 118, 102, 102, 110};
    for (int i = 0; i < 7; ++i) ListView_SetColumnWidth(list, i, i == 1 ? width - 56 - 596 : fixed[i]);
    InvalidateRect(hwnd, nullptr, FALSE);
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (ui::HandleMessage(hwnd, message, wp, lp, themed)) return themed;
    switch (message) {
    case WM_CREATE: {
        ui::ApplyWindow(hwnd);
        editingControls.clear(); labels.clear(); lastRunnerText.clear(); lastRunnerCasts = UINT64_MAX;
        font = ui::Font(14); titleFont = ui::Font(22, FW_SEMIBOLD);
        titleLabel = Make(hwnd, L"STATIC", L"技能连招", 0, 0);
        ui::StyleLabel(titleLabel, titleFont, false);
        labels.push_back(Make(hwnd, L"STATIC", L"按列表顺序循环 · 每次启动测试重置单次技能 · 施法时寻路继续运行", 0, 0));
        ui::StyleLabel(labels[0], font, true);
        Make(hwnd, L"BUTTON", L"刷新当前技能", BS_PUSHBUTTON | WS_TABSTOP, Refresh, true);
        skillsLabel = Make(hwnd, L"STATIC", L"正在获取技能…", 0, 0);
        ui::StyleLabel(skillsLabel, font, true);
        list = Make(hwnd, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, Rows, true);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* headers[] = {L"顺序", L"技能名称", L"使用次数", L"使用环境", L"间隔 ms", L"蓄力 ms", L"怪物距离"};
        const int widths[] = {55, 270, 120, 120, 125, 125, 130};
        for (int i = 0; i < 7; ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(headers[i]); column.cx = widths[i]; column.iSubItem = i;
            ListView_InsertColumn(list, i, &column);
        }
        const wchar_t* names[] = {L"技能名称", L"使用次数", L"使用环境", L"使用间隔 ms", L"蓄力时间 ms", L"使用距离"};
        for (auto name : names) {
            HWND label = Make(hwnd, L"STATIC", name, 0, 0);
            labels.push_back(label);
        }
        const DWORD comboStyle = CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP;
        skillBox = Make(hwnd, L"COMBOBOX", L"", comboStyle, Skills, true);
        usageBox = Make(hwnd, L"COMBOBOX", L"", comboStyle, UsageBox, true);
        environmentBox = Make(hwnd, L"COMBOBOX", L"", comboStyle, EnvironmentBox, true);
        for (auto text : {L"单次执行", L"全局使用"}) SendMessageW(usageBox, CB_ADDSTRING, 0, LPARAM(text));
        for (auto text : {L"遇敌使用", L"自行循环"}) SendMessageW(environmentBox, CB_ADDSTRING, 0, LPARAM(text));
        SendMessageW(usageBox, CB_SETCURSEL, 1, 0); SendMessageW(environmentBox, CB_SETCURSEL, 0, 0);
        intervalEdit = Make(hwnd, L"EDIT", L"1000", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, Interval, true);
        chargeEdit = Make(hwnd, L"EDIT", L"0", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, Charge, true);
        distanceEdit = Make(hwnd, L"EDIT", L"100", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, Distance, true);
        for (auto edit : {intervalEdit, chargeEdit, distanceEdit}) SendMessageW(edit, EM_SETLIMITTEXT, 6, 0);
        const int ids[] = {Add, Update, Remove, Up, Down, Save};
        const wchar_t* buttons[] = {L"新增技能", L"更新选中", L"删除选中", L"上移", L"下移", L"保存配置"};
        for (int i = 0; i < 6; ++i) Make(hwnd, L"BUTTON", buttons[i], BS_PUSHBUTTON | WS_TABSTOP, ids[i], true);
        labels.push_back(Make(hwnd, L"STATIC", L"松开后等待本行间隔，再执行下一项；无怪直接跳过。开场技能设为单次，并置于列表最前。\n在主界面第二排启动/停止寻路+技能循环；运行中需先停止再编辑，关闭此窗口不停止测试。", 0, 0));
        statusLabel = Make(hwnd, L"STATIC", L"就绪", 0, 0);
        ui::StyleLabel(statusLabel, font);
        std::string error;
        if (!LoadConfig(DefaultConfigPath(), rows, error)) Say(L"读取配置失败：" + Wide(error));
        Populate(-1); ReadSkills(); SetTimer(hwnd, 1, 100, nullptr); Layout(hwnd); return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {1080, 730}; return 0;
    case WM_SIZE: if (list) Layout(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint); RECT rect{}; GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, ui::Background());
        ui::Card(dc, {16, 120, rect.right - 16, rect.bottom - 276});
        ui::Card(dc, {16, rect.bottom - 264, rect.right - 16, rect.bottom - 62});
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_TIMER: {
        if (dirty.exchange(false)) ApplySkills();
        const auto state = MainRunner().Status();
        const bool locked = navigationTestActive.load() || state.running;
        for (auto h : editingControls) EnableIfChanged(h, !locked);
        EnableIfChanged(GetDlgItem(hwnd, Refresh), !locked && !reading);
        EnableIfChanged(distanceEdit, !locked && SendMessageW(environmentBox, CB_GETCURSEL, 0, 0) == 0);
        if ((state.running || state.text != "未启动") &&
            (lastRunnerText != state.text || lastRunnerCasts != state.casts)) {
            const auto text = Wide(state.text) + L"；本次已发起 " + std::to_wstring(state.casts) + L" 次";
            SetWindowTextW(statusLabel, text.c_str());
        }
        lastRunnerText = state.text; lastRunnerCasts = state.casts;
        return 0;
    }
    case WM_NOTIFY: {
        auto header = reinterpret_cast<NMHDR*>(lp);
        if (header->idFrom == Rows && header->code == LVN_ITEMCHANGED && !populating) FillEditor(Selected());
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDCANCEL) { SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (navigationTestActive.load() || MainRunner().Status().running) return 0;
        if (id == Refresh) { ReadSkills(); return 0; }
        if (id == Save) { SaveRows(); return 0; }
        const auto index = Selected();
        if (id == Add || id == Update) {
            if (id == Add && rows.size() >= kMaxRows) { Say(L"最多添加 256 项技能"); return 0; }
            if (id == Update && index < 0) { Say(L"请先选择需要更新的行"); return 0; }
            Row row;
            if (!EditorRow(row)) return 0;
            if (id == Add) {
                FILETIME time{}; GetSystemTimeAsFileTime(&time);
                row.key = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                while (std::any_of(rows.begin(), rows.end(), [&](const Row& r) { return r.key == row.key; })) ++row.key;
                rows.push_back(row); Populate(int(rows.size()) - 1);
            } else { row.key = rows[size_t(index)].key; rows[size_t(index)] = row; Populate(index); }
            SaveRows(); return 0;
        }
        if (index >= 0 && size_t(index) < rows.size()) {
            if (id == Remove) { rows.erase(rows.begin() + index); Populate((std::min)(index, int(rows.size()) - 1)); SaveRows(); }
            if (id == Up && index > 0) { std::swap(rows[size_t(index)], rows[size_t(index) - 1]); Populate(index - 1); SaveRows(); }
            if (id == Down && size_t(index) + 1 < rows.size()) { std::swap(rows[size_t(index)], rows[size_t(index) + 1]); Populate(index + 1); SaveRows(); }
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1); window = nullptr; list = nullptr; statusLabel = nullptr;
        if (font) { DeleteObject(font); font = nullptr; }
        if (titleFont) { DeleteObject(titleFont); titleFont = nullptr; } return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

void ShowSettings(HWND owner) {
    if (window && IsWindow(window)) { ShowWindow(window, SW_SHOWNORMAL); SetForegroundWindow(window); return; }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = Procedure;
    wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ui::Background(); wc.lpszClassName = L"Poe2ComboSettings";
    wc.hIcon = ui::AppIcon(); wc.hIconSm = ui::AppIcon(true);
    RegisterClassExW(&wc);
    window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"技能连招设置",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1160, 790,
        owner, nullptr, wc.hInstance, nullptr);
    if (window) { ui::CenterWindow(window, owner); ShowWindow(window, SW_SHOW); UpdateWindow(window); }
}
void SetNavigationTestActive(bool active) { navigationTestActive = active; }
void ShutdownCombo() {
    MainRunner().Stop(); MainRunner().Join();
    if (scanThread.joinable()) scanThread.join();
}
} // namespace combo
