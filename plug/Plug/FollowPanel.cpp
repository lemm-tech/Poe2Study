#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "FollowPanel.h"
#include "FollowRunner.h"
#include "NamedPipeClie.h"
#include "UiTheme.h"
#include "UseCases.h"
#include <algorithm>
#include <array>
#include <cwchar>
#include <unordered_set>
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "shell32.lib")

namespace follow {
namespace {
enum { FirstEdit = 7600, LootNames = 7606, ModeChoice = 7607, LootDistance = 7608, Start = 7610, Stop, Save, Refresh, OpenLog,
       PartyRefresh, PartyList, PartyStatus, PartyTeleport, PartyActionStatus };
HWND panel = nullptr;
HWND lootEdit = nullptr, lootLabel = nullptr;
HWND lootDistanceEdit = nullptr, lootDistanceLabel = nullptr;
HWND modeChoice = nullptr, modeLabel = nullptr;
HWND partyList = nullptr, partyStatus = nullptr;
HWND partyActionStatus = nullptr;
std::array<HWND, 6> edits{}, labels{};
HFONT font = nullptr, titleFont = nullptr;
std::function<bool(std::string&)> canStart;
std::wstring localMessage;
Options options;
uint64_t playerListRevision = 0;
int scrollOffset = 0, contentHeight = 0;
bool layingOut = false, choicesDirty = false;
RECT settingsCard{}, partyCard{}, statusCard{};
poe::PartySnapshot partySnapshot;
std::vector<poe::PartyMember> teammates;
struct PartyWork {
    std::thread thread;
    std::atomic_bool busy{false}, ready{false}, stop{false};
    std::mutex mutex;
    poe::PartySnapshot result;
    std::string error;
    ~PartyWork() { stop = true; if (thread.joinable()) thread.join(); }
} partyWork;
struct TeleportWork {
    std::thread thread;
    std::atomic_bool busy{false}, ready{false}, stop{false};
    std::mutex mutex;
    packet::Reply reply{};
    std::wstring name;
    std::string error;
    bool received = false;
    ~TeleportWork() { stop = true; if (thread.joinable()) thread.join(); }
} teleportWork;
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n); return out;
}
std::string Utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0); if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr); return out;
}
std::wstring Text(HWND window) {
    std::wstring out(size_t(GetWindowTextLengthW(window)) + 1, 0);
    const int n = GetWindowTextW(window, out.data(), int(out.size())); out.resize(n); return out;
}
void EnableIfChanged(HWND window, bool enabled) {
    if (window && (IsWindowEnabled(window) != FALSE) != enabled) EnableWindow(window, enabled);
}
void CaptionIfChanged(HWND window, const wchar_t* caption) {
    if (window && Text(window) != caption) SetWindowTextW(window, caption);
}
struct StatusView {
    std::array<std::wstring, 6> lines;
    COLORREF color = ui::color::kText;
    bool warning = false;
    bool Same(const StatusView& other) const { return lines == other.lines && color == other.color && warning == other.warning; }
} statusView;
void RefreshStatusDisplay() {
    const auto status = MainRunner().GetStatus();
    StatusView next;
    next.lines[0] = Wide(status.text);
    next.color = status.navigation.state == State::Failed ? ui::color::kBad : status.running ? ui::color::kGood : ui::color::kText;
    wchar_t line[512]{};
    if (status.target.valid) {
        const auto now = GetTickCount64();
        const auto age = now >= status.target.sampledAt ? now - status.target.sampledAt : 0;
        swprintf_s(line, L"队长 (%d, %d)    自身 (%d, %d)    距离 %.1f    样本年龄 %llums",
            status.target.position.x, status.target.position.y, status.navigation.player.x,
            status.navigation.player.y, status.navigation.distance, age);
        next.lines[1] = line;
    } else next.lines[1] = L"等待队长位置或跨图条件，切图完成后自动恢复跟随。";
    swprintf_s(line, L"待拾取 %u · 已处理 %u · 已跳过 %u · 采样 %llu · 规划 %u · 移动 %u",
        status.navigation.lootRemaining, status.navigation.lootResolved, status.navigation.lootSkipped,
        status.samples, status.navigation.plans, status.navigation.calls);
    next.lines[2] = line;
    next.lines[3] = Wide(ModeText(options.mode)) + L" · 队友传送 " + std::to_wstring(status.teleportAttempts) + L" / 5";
    next.lines[4] = L"自身：" + Wide(status.selfMap.empty() ? "待读取" : status.selfMap) +
        L"\n队长：" + Wide(status.leaderMap.empty() ? "待读取" : status.leaderMap);
    next.warning = !localMessage.empty();
    next.lines[5] = next.warning ? localMessage : L"传送最多尝试 5 次；确认队长换图或手动重启跟随后重置。";
    if (statusView.Same(next)) return;
    statusView = std::move(next);
    if (panel && IsWindowVisible(panel)) {
        RECT dirty = statusCard; InflateRect(&dirty, -1, -1); OffsetRect(&dirty, 0, -scrollOffset);
        InvalidateRect(panel, &dirty, FALSE);
    }
}
// Paint the complete page off screen; BeginPaint clips the final copy to dirty
// pixels and excludes child controls, so no cleared intermediate frame is shown.
class PaintBuffer {
public:
    PaintBuffer(HDC target, const RECT& bounds) : target_(target), bounds_(bounds) {
        if (bounds.right <= 0 || bounds.bottom <= 0) return;
        memory_ = CreateCompatibleDC(target);
        if (!memory_) return;
        bitmap_ = CreateCompatibleBitmap(target, bounds.right, bounds.bottom);
        if (bitmap_) previous_ = SelectObject(memory_, bitmap_);
        if (!bitmap_ || !previous_ || previous_ == HGDI_ERROR) {
            if (bitmap_) DeleteObject(bitmap_);
            DeleteDC(memory_); memory_ = nullptr; bitmap_ = nullptr;
        }
    }
    ~PaintBuffer() {
        if (memory_) { SelectObject(memory_, previous_); DeleteObject(bitmap_); DeleteDC(memory_); }
    }
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;
    HDC Dc() const { return memory_ ? memory_ : target_; }
    void Present() const {
        if (memory_) BitBlt(target_, 0, 0, bounds_.right, bounds_.bottom, memory_, 0, 0, SRCCOPY);
    }
private:
    HDC target_ = nullptr, memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    RECT bounds_{};
};
bool ReadEditor(Options& out) {
    out = options;
    out.mode = Mode(SendMessageW(modeChoice, CB_GETCURSEL, 0, 0));
    auto name = Text(edits[0]);
    const auto begin = name.find_first_not_of(L" \t");
    name = begin == std::wstring::npos ? L"" : name.substr(begin, name.find_last_not_of(L" \t") - begin + 1);
    out.leaderName = Utf8(name);
    std::vector<std::string> lootNames; std::string lootError;
    if (!ParseLootNames(Utf8(Text(lootEdit)), lootNames, lootError)) { localMessage = Wide(lootError); return false; }
    out.lootNames = JoinLootNames(lootNames);
    uint32_t values[5]{};
    for (size_t i = 1; i < edits.size(); ++i) {
        const auto text = Text(edits[i]);
        try {
            if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos) throw 0;
            const auto number = std::stoull(text); if (number > 1000000) throw 0;
            values[i - 1] = uint32_t(number);
        } catch (...) { localMessage = L"参数需要填写范围内的整数"; return false; }
    }
    out.distance = int32_t(values[0]); out.sampleMs = values[1]; out.replanMs = values[2];
    out.moveThreshold = int32_t(values[3]); out.searchMs = values[4];
    const auto distanceText = Text(lootDistanceEdit);
    try {
        if (distanceText.empty() || distanceText.find_first_not_of(L"0123456789") != std::wstring::npos) throw 0;
        const auto distance = std::stoull(distanceText);
        if (distance < 1 || distance > 94208) throw 0;
        out.lootDistance = int32_t(distance);
    } catch (...) { localMessage = L"拾取距离请填写 1–94208 的整数（世界像素）"; return false; }
    std::string error;
    if (!Validate(out, error)) { localMessage = Wide(error); return false; }
    return true;
}
void RefreshPlayerList() {
    std::string error;
    if (!MainRunner().RefreshPlayers(error)) localMessage = Wide(error);
    else localMessage = L"正在遍历周围玩家，完成后展开队长名称下拉框选择…";
    RefreshStatusDisplay();
}
void RebuildPlayerChoices(const PlayerListStatus& list) {
    const auto name = Text(edits[0]);
    DWORD selectionStart = 0, selectionEnd = 0;
    SendMessageW(edits[0], CB_GETEDITSEL, WPARAM(&selectionStart), LPARAM(&selectionEnd));
    const bool visible = IsWindowVisible(edits[0]) != FALSE;
    if (visible) SendMessageW(edits[0], WM_SETREDRAW, FALSE, 0);
    SendMessageW(edits[0], CB_RESETCONTENT, 0, 0);
    std::unordered_set<std::string> names;
    for (const auto& player : list.players) {
        if (names.insert(player.name).second) {
            const auto value = Wide(player.name);
            SendMessageW(edits[0], CB_ADDSTRING, 0, LPARAM(value.c_str()));
        }
    }
    for (const auto& player : teammates) if (names.insert(player.name).second) {
        const auto value = Wide(player.name);
        SendMessageW(edits[0], CB_ADDSTRING, 0, LPARAM(value.c_str()));
    }
    SetWindowTextW(edits[0], name.c_str());
    SendMessageW(edits[0], CB_SETEDITSEL, 0, MAKELPARAM(selectionStart, selectionEnd));
    if (visible) {
        SendMessageW(edits[0], WM_SETREDRAW, TRUE, 0);
        RedrawWindow(edits[0], nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    choicesDirty = false;
}
void UpdatePlayerChoices() {
    const auto list = MainRunner().GetPlayers();
    if (!list.refreshing && playerListRevision != list.revision) {
        playerListRevision = list.revision; choicesDirty = true;
        if (!list.text.empty()) localMessage = Wide(list.text);
    }
    if (choicesDirty && !SendMessageW(edits[0], CB_GETDROPPEDSTATE, 0, 0)) RebuildPlayerChoices(list);
}
void ClearParty() {
    partySnapshot = {}; teammates.clear(); choicesDirty = true;
    ListView_DeleteAllItems(partyList);
}
void RefreshPartyList() {
    if (teleportWork.busy) return;
    if (partyWork.busy.exchange(true)) return;
    if (partyWork.thread.joinable()) partyWork.thread.join();
    partyWork.stop = false; partyWork.ready = false;
    ClearParty();
    SetWindowTextW(partyActionStatus, L"单击队友可填入队长名称；选中队友后可传送。");
    SetWindowTextW(partyStatus, L"正在获取队友及其所在地图…");
    EnableWindow(GetDlgItem(panel, PartyRefresh), FALSE);
    try {
        partyWork.thread = std::thread([] {
            poe::PartySnapshot result; std::string error;
            try {
                if (!usecase::ScanParty(result, error, &partyWork.stop) && error.empty()) error = "队友列表读取失败";
            } catch (const std::exception& e) { error = e.what(); }
            if (!error.empty()) result = {};
            LogDiagnostic("[PARTY_LIST] pid=%lu members=%zu error=%s", result.pid, result.members.size(), error.c_str());
            {
                std::lock_guard<std::mutex> lock(partyWork.mutex);
                partyWork.result = std::move(result); partyWork.error = std::move(error);
            }
            partyWork.ready = true;
        });
    } catch (const std::exception& e) {
        partyWork.busy = false;
        SetWindowTextW(partyStatus, (L"获取失败：" + Wide(e.what())).c_str());
    }
}
const wchar_t* MapStatus(poe::PartyMapStatus status) {
    switch (status) {
    case poe::PartyMapStatus::ProfileMissing: return L"成员资料暂不可用";
    case poe::PartyMapStatus::ProfileAmbiguous: return L"成员资料尚未确认";
    default: return L"地图暂不可用";
    }
}
void UpdatePartyList() {
    if (!partyWork.ready.exchange(false)) return;
    if (partyWork.thread.joinable()) partyWork.thread.join();
    std::string error;
    {
        std::lock_guard<std::mutex> lock(partyWork.mutex);
        partySnapshot = std::move(partyWork.result); error = std::move(partyWork.error);
    }
    partyWork.busy = false;
    if (!error.empty() || !app::Config().TargetAlive()) {
        ClearParty();
        SetWindowTextW(partyStatus, (L"获取失败：" + (error.empty() ? L"目标游戏已退出" : Wide(error))).c_str());
        return;
    }
    for (const auto& member : partySnapshot.members) if (!member.self) teammates.push_back(member);
    SendMessageW(partyList, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < teammates.size(); ++i) {
        const auto& member = teammates[i];
        auto name = Wide(member.name);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = int(i); item.pszText = name.data();
        ListView_InsertItem(partyList, &item);
        auto map = member.mapStatus == poe::PartyMapStatus::Resolved ? Wide(member.mapName) : MapStatus(member.mapStatus);
        auto id = member.mapStatus == poe::PartyMapStatus::Resolved ? Wide(member.mapId) : L"—";
        ListView_SetItemText(partyList, int(i), 1, map.data());
        ListView_SetItemText(partyList, int(i), 2, id.data());
    }
    SendMessageW(partyList, WM_SETREDRAW, TRUE, 0); InvalidateRect(partyList, nullptr, FALSE);
    choicesDirty = true;
    SYSTEMTIME time{}; GetLocalTime(&time); wchar_t line[160]{};
    if (teammates.empty()) swprintf_s(line, L"当前没有其他队员 · 获取于 %02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    else swprintf_s(line, L"已获取 %zu 位队友 · 获取于 %02u:%02u:%02u", teammates.size(), time.wHour, time.wMinute, time.wSecond);
    SetWindowTextW(partyStatus, line);
}
void SelectTeammate(int row) {
    if (MainRunner().Running() || teleportWork.busy || row < 0 || size_t(row) >= teammates.size()) return;
    const auto& member = teammates[size_t(row)];
    SetWindowTextW(edits[0], Wide(member.name).c_str());
    localMessage = L"队长名称已填入：" + Wide(member.name) + L"。设置会在保存或开始跟随时生效。";
    SetWindowTextW(partyActionStatus, (L"已选中：" + Wide(member.name) + L"。点击“传送至选中队友”即可请求传送。").c_str());
    RefreshStatusDisplay();
}
void StopPartyScan() {
    partyWork.stop = true;
    if (partyWork.thread.joinable()) partyWork.thread.join();
    partyWork.busy = false; partyWork.ready = false;
}
void StartPartyTeleport() {
    if (teleportWork.busy || partyWork.busy) return;
    const auto say = [](const std::wstring& message) { SetWindowTextW(partyActionStatus, message.c_str()); };
    std::string reason;
    if (MainRunner().Running() || MainRunner().GetPlayers().refreshing || (canStart && !canStart(reason))) {
        say(reason.empty() ? L"请先停止跟随，并等待当前操作结束。" : Wide(reason)); return;
    }
    const auto connection = pipe::GetConnectionSnapshot();
    if (!app::Config().TargetAlive() || !connection.ready || connection.gamePid != partySnapshot.pid) {
        say(L"请先连接新版 DLL，并获取当前角色的队友列表。"); return;
    }
    const int row = ListView_GetNextItem(partyList, -1, LVNI_SELECTED);
    if (row < 0 || size_t(row) >= teammates.size()) { say(L"请先选中一位队友。"); return; }
    const auto member = teammates[size_t(row)];
    if (teleportWork.thread.joinable()) teleportWork.thread.join();
    teleportWork.busy = true; teleportWork.ready = false; teleportWork.stop = false;
    teleportWork.name = Wide(member.name);
    EnableWindow(GetDlgItem(panel, PartyTeleport), FALSE);
    EnableWindow(GetDlgItem(panel, PartyRefresh), FALSE);
    say(L"正在核对队伍并请求传送至：" + teleportWork.name + L"…");
    try {
        teleportWork.thread = std::thread([member, connection] {
            packet::Reply reply{}; std::string error; bool received = false;
            try {
                received = usecase::TeleportToPartyMember(member, connection.gamePid, connection.sessionId,
                    connection.controllerId, reply, error, &teleportWork.stop);
            } catch (const std::exception& e) { error = e.what(); }
            catch (...) { error = "队友传送发生异常；未自动重发，请先检查游戏状态"; }
            LogDiagnostic("[PARTY_TELEPORT] pid=%u name=%s received=%u result=%u invoked=%u exception=%08X detail=%s error=%s",
                connection.gamePid, member.name.c_str(), unsigned(received), unsigned(reply.result),
                reply.invoked, reply.exceptionCode, reply.detail, error.c_str());
            {
                std::lock_guard<std::mutex> lock(teleportWork.mutex);
                teleportWork.reply = reply; teleportWork.error = std::move(error); teleportWork.received = received;
            }
            teleportWork.ready = true;
        });
    } catch (const std::exception& e) {
        teleportWork.busy = false; say(L"无法开始传送：" + Wide(e.what()));
    }
}
void UpdatePartyTeleport() {
    if (!teleportWork.ready.exchange(false)) return;
    if (teleportWork.thread.joinable()) teleportWork.thread.join();
    packet::Reply reply{}; std::string error; bool received = false;
    {
        std::lock_guard<std::mutex> lock(teleportWork.mutex);
        reply = teleportWork.reply; error = std::move(teleportWork.error); received = teleportWork.received;
    }
    teleportWork.busy = false;
    if (!received) {
        SetWindowTextW(partyActionStatus, Wide(error.empty() ? "传送结果未知，请先检查游戏状态" : error).c_str()); return;
    }
    if (reply.result == packet::Result::Ok) {
        ClearParty();
        SetWindowTextW(partyStatus, L"传送后请重新获取队友及其所在地图。");
        SetWindowTextW(partyActionStatus, (L"已向「" + teleportWork.name + L"」发送传送请求。是否成功以游戏切图为准。").c_str());
    } else SetWindowTextW(partyActionStatus, (L"传送未完成：" + Wide(reply.detail)).c_str());
}
void StopPartyTeleport() {
    teleportWork.stop = true;
    if (teleportWork.thread.joinable()) teleportWork.thread.join();
    teleportWork.busy = false; teleportWork.ready = false;
}
void Move(HWND control, int x, int y, int width, int height) {
    RECT current{}; GetWindowRect(control, &current); MapWindowPoints(nullptr, panel, reinterpret_cast<POINT*>(&current), 2);
    y -= scrollOffset; width = (std::max)(1, width); height = (std::max)(1, height);
    if (current.left != x || current.top != y || current.right - current.left != width || current.bottom - current.top != height)
        SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void Layout(HWND hwnd) {
    if (layingOut || !edits[0]) return;
    layingOut = true;
    RECT rect{}; GetClientRect(hwnd, &rect);
    const bool wide = rect.right >= 1100;
    contentHeight = (std::max)(wide ? 780 : 1200, int(rect.bottom));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = contentHeight - 1; scroll.nPage = UINT(rect.bottom); scroll.nPos = scrollOffset;
    SetScrollInfo(hwnd, SB_VERT, &scroll, TRUE); GetScrollInfo(hwnd, SB_VERT, &scroll); scrollOffset = scroll.nPos;
    GetClientRect(hwnd, &rect);
    const int mainRight = wide ? 16 + (rect.right - 48) * 55 / 100 : rect.right - 16;
    const int inner = mainRight - 40, width = (inner - 16) / 2;
    settingsCard = {16, 84, mainRight, 410};
    partyCard = {wide ? mainRight + 16 : 16, wide ? 84 : 474, rect.right - 16, wide ? contentHeight - 12 : 876};
    statusCard = {16, wide ? 474 : 890, mainRight, contentHeight - 12};
    Move(modeLabel, 28, 102, 100, 22);
    Move(modeChoice, 136, 96, inner - 108, 150);
    for (int i = 0; i < 6; ++i) {
        const int x = 28 + (i % 2) * (width + 16), y = 150 + (i / 2) * 60;
        Move(labels[i], x, y, width, 20);
        Move(edits[i], x, y + 24, width, i ? 30 : 270);
    }
    const int distanceWidth = 160, namesWidth = inner - distanceWidth - 16;
    Move(lootLabel, 28, 336, namesWidth, 22);
    Move(lootEdit, 28, 364, namesWidth, 32);
    Move(lootDistanceLabel, 28 + namesWidth + 16, 336, distanceWidth, 22);
    Move(lootDistanceEdit, 28 + namesWidth + 16, 364, distanceWidth, 32);
    const int buttonWidth = (inner - 40) / 5;
    for (int i = 0; i < 5; ++i)
        Move(GetDlgItem(hwnd, Start + i), 28 + i * (buttonWidth + 10), 424, buttonWidth, 36);
    const int px = partyCard.left + 12, pr = partyCard.right - 12, py = partyCard.top;
    Move(GetDlgItem(hwnd, PartyRefresh), pr - 140, py + 12, 140, 34);
    Move(partyStatus, px, py + 52, pr - px, 40);
    Move(partyList, px, py + 98, pr - px, partyCard.bottom - py - 218);
    Move(GetDlgItem(hwnd, PartyTeleport), px, partyCard.bottom - 108, pr - px, 34);
    Move(partyActionStatus, px, partyCard.bottom - 64, pr - px, 52);
    const int columns = (std::max)(360, pr - px - 22);
    const int columnWidths[] = {columns * 28 / 100, columns * 34 / 100, columns - columns * 28 / 100 - columns * 34 / 100};
    for (int i = 0; i < 3; ++i) if (ListView_GetColumnWidth(partyList, i) != columnWidths[i])
        ListView_SetColumnWidth(partyList, i, columnWidths[i]);
    layingOut = false;
    InvalidateRect(hwnd, nullptr, FALSE);
}
void ScrollPage(int position) {
    scrollOffset = position; Layout(panel);
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (ui::HandleMessage(hwnd, message, wp, lp, themed)) return themed;
    switch (message) {
    case WM_CREATE: {
        panel = hwnd;
        font = ui::Font(14); titleFont = ui::Font(24, FW_SEMIBOLD);
        std::string error;
        if (!LoadConfig(options, error)) localMessage = L"读取跟随配置失败：" + Wide(error);
        modeLabel = CreateWindowExW(0, L"STATIC", L"跟随模式", WS_CHILD | WS_VISIBLE,
            0, 0, 100, 22, hwnd, nullptr, nullptr, nullptr);
        ui::StyleLabel(modeLabel, font, true, true);
        modeChoice = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 0, 0, 200, 150, hwnd, HMENU(ModeChoice), nullptr, nullptr);
        ui::StyleControl(modeChoice, font);
        SendMessageW(modeChoice, CB_ADDSTRING, 0, LPARAM(L"剧情模式"));
        SendMessageW(modeChoice, CB_ADDSTRING, 0, LPARAM(L"异界模式"));
        SendMessageW(modeChoice, CB_SETCURSEL, WPARAM(options.mode), 0);
        const wchar_t* names[] = {L"队长角色名称（可输入或下拉选择）", L"与队长保持距离（默认 60）",
            L"队长位置采样间隔 ms", L"A* 重规划最小间隔 ms", L"触发重规划的目标位移", L"失联重新搜索间隔 ms"};
        const std::wstring values[] = {Wide(options.leaderName), std::to_wstring(options.distance),
            std::to_wstring(options.sampleMs), std::to_wstring(options.replanMs),
            std::to_wstring(options.moveThreshold), std::to_wstring(options.searchMs)};
        for (int i = 0; i < 6; ++i) {
            labels[i] = CreateWindowExW(0, L"STATIC", names[i], WS_CHILD | WS_VISIBLE,
                0, 0, 10, 22, hwnd, nullptr, nullptr, nullptr);
            ui::StyleLabel(labels[i], font, true, true);
            edits[i] = CreateWindowExW(0, i ? L"EDIT" : L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                (i ? ES_AUTOHSCROLL | ES_NUMBER : CBS_DROPDOWN | CBS_OWNERDRAWFIXED | CBS_AUTOHSCROLL | CBS_HASSTRINGS | WS_VSCROLL),
                0, 0, 10, i ? 32 : 270, hwnd, HMENU(INT_PTR(FirstEdit + i)), nullptr, nullptr);
            SendMessageW(edits[i], i ? EM_SETLIMITTEXT : CB_LIMITTEXT, i ? 6 : 96, 0);
            ui::StyleControl(edits[i], font);
            if (!i) {
                SendMessageW(edits[0], CB_SETMINVISIBLE, 8, 0);
            }
            SetWindowTextW(edits[i], values[i].c_str());
        }
        lootLabel = CreateWindowExW(0, L"STATIC", L"优先拾取名称（逗号分隔；留空仅跟随）", WS_CHILD | WS_VISIBLE,
            0, 0, 10, 22, hwnd, nullptr, nullptr, nullptr);
        ui::StyleLabel(lootLabel, font, true, true);
        lootEdit = CreateWindowExW(0, L"EDIT", Wide(options.lootNames).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 32, hwnd, HMENU(LootNames), nullptr, nullptr);
        SendMessageW(lootEdit, EM_SETLIMITTEXT, 2048, 0);
        SendMessageW(lootEdit, EM_SETCUEBANNER, FALSE, LPARAM(L"例如：崇高石,神聖石"));
        ui::StyleControl(lootEdit, font);
        lootDistanceLabel = CreateWindowExW(0, L"STATIC", L"拾取距离（世界像素）", WS_CHILD | WS_VISIBLE,
            0, 0, 10, 22, hwnd, nullptr, nullptr, nullptr);
        ui::StyleLabel(lootDistanceLabel, font, true, true);
        lootDistanceEdit = CreateWindowExW(0, L"EDIT", std::to_wstring(options.lootDistance).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 10, 32, hwnd, HMENU(LootDistance), nullptr, nullptr);
        SendMessageW(lootDistanceEdit, EM_SETLIMITTEXT, 5, 0);
        ui::StyleControl(lootDistanceEdit, font);
        const wchar_t* captions[] = {L"开始跟随", L"停止跟随", L"保存设置", L"刷新周围", L"打开日志"};
        for (int i = 0; i < 5; ++i) {
            HWND button = CreateWindowExW(0, L"BUTTON", captions[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 100, 36, hwnd, HMENU(INT_PTR(Start + i)), nullptr, nullptr);
            ui::StyleControl(button, font, i == 0 ? ui::ButtonStyle::Primary : i == 1 ? ui::ButtonStyle::Danger : ui::ButtonStyle::Secondary);
        }
        HWND getParty = CreateWindowExW(0, L"BUTTON", L"获取队友列表", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 140, 34, hwnd, HMENU(PartyRefresh), nullptr, nullptr);
        ui::StyleControl(getParty, font, ui::ButtonStyle::Primary);
        HWND teleport = CreateWindowExW(0, L"BUTTON", L"传送至选中队友", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 140, 34, hwnd, HMENU(PartyTeleport), nullptr, nullptr);
        ui::StyleControl(teleport, font, ui::ButtonStyle::Primary);
        EnableWindow(teleport, FALSE);
        partyActionStatus = CreateWindowExW(0, L"STATIC", L"单击队友可填入队长名称；选中队友后可传送。", WS_CHILD | WS_VISIBLE,
            0, 0, 10, 52, hwnd, HMENU(PartyActionStatus), nullptr, nullptr);
        ui::StyleLabel(partyActionStatus, font, true, true);
        partyStatus = CreateWindowExW(0, L"STATIC", L"点击获取队友列表，查看队友所在地图。", WS_CHILD | WS_VISIBLE,
            0, 0, 10, 40, hwnd, HMENU(PartyStatus), nullptr, nullptr);
        ui::StyleLabel(partyStatus, font, true, true);
        partyList = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 10, 120, hwnd, HMENU(PartyList), nullptr, nullptr);
        ui::StyleControl(partyList, font);
        ListView_SetExtendedListViewStyle(partyList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const wchar_t* columnNames[] = {L"队友名称", L"所在地图", L"地图 ID"};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(columnNames[i]); column.cx = 140;
            ListView_InsertColumn(partyList, i, &column);
        }
        EnableWindow(GetDlgItem(hwnd, Stop), FALSE);
        SetTimer(hwnd, 1, 200, nullptr); Layout(hwnd); RefreshStatusDisplay(); return 0;
    }
    case WM_SIZE: if (edits[0]) Layout(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const HDC target = BeginPaint(hwnd, &paint);
        RECT rect{}; GetClientRect(hwnd, &rect);
        PaintBuffer buffer(target, rect); const HDC dc = buffer.Dc();
        FillRect(dc, &rect, ui::Background());
        SetViewportOrgEx(dc, 0, -scrollOffset, nullptr);
        ui::Text(dc, L"自动跟随", {28, 10, rect.right - 28, 44}, titleFont);
        ui::Text(dc, L"优先拾取距离内的名单物品；每件最多尝试 2 次，失败跳过后继续跟随。",
            {28, 48, rect.right - 28, 78}, font, ui::color::kTextDim);
        ui::Card(dc, settingsCard);
        ui::Card(dc, partyCard);
        ui::Card(dc, statusCard);
        ui::Text(dc, L"队友与地图", {partyCard.left + 12, partyCard.top + 12, partyCard.right - 164, partyCard.top + 46}, font);
        const int sy = statusCard.top + 12, sr = statusCard.right - 12;
        ui::Text(dc, statusView.lines[0].c_str(), {28, sy, sr, sy + 24}, font, statusView.color);
        ui::Text(dc, statusView.lines[1].c_str(), {28, sy + 28, sr, sy + 52}, font, ui::color::kTextDim);
        ui::Text(dc, statusView.lines[2].c_str(), {28, sy + 56, sr, sy + 80}, font, ui::color::kTextDim);
        ui::Text(dc, statusView.lines[3].c_str(), {28, sy + 84, sr, sy + 108}, font, ui::color::kTextDim);
        ui::Text(dc, statusView.lines[4].c_str(), {28, sy + 112, sr, sy + 164}, font, ui::color::kTextDim,
            DT_LEFT | DT_TOP | DT_WORDBREAK);
        ui::Text(dc, statusView.lines[5].c_str(), {28, sy + 170, sr, statusCard.bottom - 12}, font,
            statusView.warning ? ui::color::kWarn : ui::color::kTextDim, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SetViewportOrgEx(dc, 0, 0, nullptr); buffer.Present();
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_TIMER: {
        if (wp != 1) break;
        const bool running = MainRunner().Running();
        const bool scanning = MainRunner().GetPlayers().refreshing;
        UpdatePartyList();
        UpdatePartyTeleport();
        const bool targetAlive = app::Config().TargetAlive();
        if (!targetAlive && partySnapshot.sampledAt) {
            ClearParty(); SetWindowTextW(partyStatus, L"目标游戏已退出，请重新打开对应角色面板。");
        }
        UpdatePlayerChoices();
        std::string reason;
        const auto connection = pipe::GetConnectionSnapshot();
        const bool available = !running && !scanning && !teleportWork.busy && connection.ready && (!canStart || canStart(reason));
        EnableIfChanged(GetDlgItem(hwnd, Start), available);
        EnableIfChanged(GetDlgItem(hwnd, Stop), running);
        EnableIfChanged(GetDlgItem(hwnd, Save), !running && !teleportWork.busy);
        EnableIfChanged(GetDlgItem(hwnd, Refresh), !running && !scanning && !teleportWork.busy);
        EnableIfChanged(GetDlgItem(hwnd, PartyRefresh), !partyWork.busy && !teleportWork.busy && targetAlive);
        const int selected = ListView_GetNextItem(partyList, -1, LVNI_SELECTED);
        EnableIfChanged(GetDlgItem(hwnd, PartyTeleport), available && !partyWork.busy && targetAlive &&
            partySnapshot.pid == connection.gamePid && selected >= 0 && size_t(selected) < teammates.size());
        CaptionIfChanged(GetDlgItem(hwnd, PartyTeleport), teleportWork.busy ? L"正在请求传送…" : L"传送至选中队友");
        CaptionIfChanged(GetDlgItem(hwnd, PartyRefresh), partyWork.busy ? L"正在获取…" : L"获取队友列表");
        for (auto edit : edits) EnableIfChanged(edit, !running && !teleportWork.busy);
        EnableIfChanged(lootEdit, !running && !teleportWork.busy);
        EnableIfChanged(lootDistanceEdit, !running && !teleportWork.busy);
        EnableIfChanged(modeChoice, !running && !teleportWork.busy);
        if (!running) MainRunner().Join();
        if (!scanning) MainRunner().JoinPlayerScan();
        RefreshStatusDisplay(); return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, SB_VERT, &info);
        int position = scrollOffset;
        switch (LOWORD(wp)) {
        case SB_LINEUP: position -= 40; break;
        case SB_LINEDOWN: position += 40; break;
        case SB_PAGEUP: position -= int(info.nPage); break;
        case SB_PAGEDOWN: position += int(info.nPage); break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: position = info.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = contentHeight; break;
        }
        ScrollPage(position); return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollPage(scrollOffset - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 60); return 0;
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<NMHDR*>(lp);
        if (notification->idFrom == PartyList && notification->code == NM_CLICK) {
            SelectTeammate(reinterpret_cast<NMITEMACTIVATE*>(lp)->iItem); return 0;
        }
        if (notification->idFrom == PartyList && notification->code == LVN_KEYDOWN &&
            reinterpret_cast<NMLVKEYDOWN*>(lp)->wVKey == VK_RETURN) {
            SelectTeammate(ListView_GetNextItem(partyList, -1, LVNI_SELECTED)); return 0;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == FirstEdit && HIWORD(wp) == CBN_SELCHANGE) {
            const auto index = SendMessageW(edits[0], CB_GETCURSEL, 0, 0);
            const auto size = SendMessageW(edits[0], CB_GETLBTEXTLEN, index, 0);
            if (index != CB_ERR && size >= 0 && size <= 384) {
                std::wstring selected(size_t(size) + 1, 0);
                SendMessageW(edits[0], CB_GETLBTEXT, index, LPARAM(selected.data())); selected.resize(size_t(size));
                LogDiagnostic("[UI_SELECT] name=%s list_revision=%llu", Utf8(selected).c_str(), playerListRevision);
            }
            return 0;
        }
        switch (LOWORD(wp)) {
        case Refresh: RefreshPlayerList(); break;
        case PartyRefresh: RefreshPartyList(); break;
        case PartyTeleport: StartPartyTeleport(); break;
        case OpenLog: {
            const auto path = DiagnosticLogPath();
            if (path.empty()) localMessage = L"无法创建跟随日志，请检查程序目录写入权限";
            else if (INT_PTR(ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                localMessage = L"日志位于程序目录 follow_logs 下，请手动打开";
            break;
        }
        case Start: case Save: {
            if (MainRunner().Running() || teleportWork.busy) break;
            Options edited; std::string error;
            if (!ReadEditor(edited)) break;
            LogDiagnostic("[UI_%s] requested=%s distance=%d sample_ms=%u replan_ms=%u loot_distance=%d",
                LOWORD(wp) == Start ? "START" : "SAVE", edited.leaderName.c_str(), edited.distance,
                edited.sampleMs, edited.replanMs, edited.lootDistance);
            if (LOWORD(wp) == Start && canStart && !canStart(error)) { localMessage = Wide(error); break; }
            if (!SaveConfig(edited, error)) { localMessage = Wide(error); break; }
            options = edited;
            SetWindowTextW(lootEdit, Wide(options.lootNames).c_str());
            if (LOWORD(wp) == Save) localMessage = L"已保存到程序目录的 follow.ini";
            else if (!MainRunner().Start(options, error)) localMessage = Wide(error);
            else localMessage.clear();
            break;
        }
        case Stop: MainRunner().Stop(); localMessage = L"正在停止跟随和拾取；已经下发的动作可能仍会完成。"; break;
        default: return 0;
        }
        RefreshStatusDisplay(); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1); StopPartyScan(); StopPartyTeleport(); MainRunner().Stop(); MainRunner().StopPlayerScan(); MainRunner().JoinPlayerScan();
        DeleteObject(font); DeleteObject(titleFont); font = titleFont = nullptr; panel = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace
void CreatePanel(HWND parent, std::function<bool(std::string&)> callback) {
    canStart = std::move(callback);
    WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"Poe2FollowPage";
    RegisterClassExW(&cls);
    panel = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"跟随",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 600, 570, parent, HMENU(760), cls.hInstance, nullptr);
}
void ShowPanel(bool visible, const RECT& bounds) {
    if (!panel) return;
    const bool entering = visible && !IsWindowVisible(panel);
    RECT current{}; GetWindowRect(panel, &current);
    MapWindowPoints(nullptr, GetParent(panel), reinterpret_cast<POINT*>(&current), 2);
    if (!EqualRect(&current, &bounds))
        SetWindowPos(panel, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    const bool shown = (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0;
    if (shown != visible) ShowWindow(panel, visible ? SW_SHOW : SW_HIDE);
    if (entering) RefreshStatusDisplay();
    if (entering && !MainRunner().Running() && !MainRunner().GetPlayers().refreshing) RefreshPlayerList();
}
void ShutdownPanel() { StopPartyScan(); StopPartyTeleport(); MainRunner().Stop(); MainRunner().StopPlayerScan(); MainRunner().Join(); MainRunner().JoinPlayerScan(); }
bool PartyTeleportPending() { return teleportWork.busy.load(); }
} // namespace follow
