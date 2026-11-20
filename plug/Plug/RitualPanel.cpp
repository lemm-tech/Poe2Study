#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RitualPanel.h"
#include "RitualAutomation.h"
#include "UseCases.h"
#include "UiTheme.h"
#include "../Common/AppConfig.h"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace ritualui {
namespace {
enum { Scan = 7800, Go, Stop, Copy, X, Y, Candidates, Details,
       Plan, StartAutomation, PauseAutomation, CopyRoute, MarkCompleted, RetryInteraction,
       InteractDistance, LoadTimeout, BattleTimeout, FightElites, ArenaRadius, PreclearRadius, OpenLogs };
HWND panel = nullptr, list = nullptr, details = nullptr, editX = nullptr, editY = nullptr;
HWND scanButton = nullptr, goButton = nullptr, stopButton = nullptr, copyButton = nullptr;
HWND planButton = nullptr, startAutomationButton = nullptr, pauseAutomationButton = nullptr;
HWND copyRouteButton = nullptr, markCompletedButton = nullptr, retryInteractionButton = nullptr;
HWND logsButton = nullptr;
HWND editInteractDistance = nullptr, editLoadTimeout = nullptr, editBattleTimeout = nullptr;
HWND editArenaRadius = nullptr, editPreclearRadius = nullptr;
HWND fightElitesCheck = nullptr;
HFONT font = nullptr;
std::function<bool(const Target&, std::string&)> navigate;
std::function<void()> stopNavigation;
std::function<NavigationState()> navigationState;
std::function<bool(const ritual::Options&, std::string&)> automate;
usecase::RitualScanResult snapshot;
int selectedRow = -1;
bool populating = false, haveScan = false, scanAttempted = false, haveNavigation = false;
bool automationUsed = false, preserveActionText = false;
uint64_t automationSyncAfterRevision = 0;
std::wstring scanText = L"扫描未筛选的周围实体，单击一行填入目标坐标。";
std::wstring navigationText = L"未完成且交互关闭时先清理节点周围怪物，恢复可交互后再开启。";
RECT headerRect{}, actionsRect{}, statusRect{};
std::vector<std::array<std::wstring, 11>> automationCells;
struct ScanWork {
    std::thread thread;
    std::atomic_bool busy{false}, ready{false}, stop{false};
    std::mutex mutex;
    usecase::RitualScanResult result;
    std::string error;
    ~ScanWork() { stop = true; if (thread.joinable()) thread.join(); }
} work;

std::wstring Wide(const std::string& text)
{
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(count), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), count);
    return out;
}

void Enable(HWND window, bool enabled)
{
    if (window && bool(IsWindowEnabled(window)) != enabled) EnableWindow(window, enabled);
}

void SetTextIfChanged(HWND window, const wchar_t* value)
{
    if (!window) return;
    const int length = GetWindowTextLengthW(window);
    std::wstring current(size_t((std::max)(0, length)) + 1, L'\0');
    GetWindowTextW(window, current.data(), int(current.size()));
    current.resize(size_t((std::max)(0, length)));
    if (current != value) SetWindowTextW(window, value);
}

void Place(HWND window, int x, int y, int width, int height)
{
    RECT current{}; GetWindowRect(window, &current);
    MapWindowPoints(nullptr, GetParent(window), reinterpret_cast<POINT*>(&current), 2);
    if (current.left != x || current.top != y || current.right - current.left != width || current.bottom - current.top != height)
        SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Layout()
{
    RECT rc{}; GetClientRect(panel, &rc);
    const int width = rc.right, height = rc.bottom;
    headerRect = {0, 0, width, 70};
    actionsRect = {0, 78, width, 254};
    statusRect = {0, height - 48, width, height};
    Place(copyButton, width - 384, 12, 120, 32);
    Place(copyRouteButton, width - 256, 12, 118, 32);
    Place(scanButton, width - 130, 12, 116, 32);
    Place(editX, 34, 90, 70, 28); Place(editY, 132, 90, 70, 28);
    Place(goButton, 218, 88, 150, 32);
    Place(editInteractDistance, 445, 90, 42, 28);
    Place(editLoadTimeout, 548, 90, 42, 28);
    Place(editBattleTimeout, 80, 128, 48, 28);
    Place(editArenaRadius, 214, 128, 48, 28);
    Place(editPreclearRadius, 394, 128, 48, 28);
    Place(fightElitesCheck, 8, 164, (std::max)(170, width - 22), 28);
    Place(planButton, 6, 204, 78, 34);
    Place(startAutomationButton, 90, 204, 104, 34);
    Place(pauseAutomationButton, 200, 204, 92, 34);
    Place(stopButton, 298, 204, 68, 34);
    Place(retryInteractionButton, 372, 204, 86, 34);
    Place(markCompletedButton, 464, 204, 122, 34);
    Place(logsButton, 592, 204, 86, 34);
    Place(list, 0, 262, width, (std::max)(60, height - 400));
    Place(details, 0, height - 138, width, 82);
    const int columns[] = {42, 132, 48, 48, 76, 72, 54, 132, 72, 68, 54, 76, 68, 122, 100, 132, 150, 260};
    for (int i = 0; i < 18; ++i)
        if (ListView_GetColumnWidth(list, i) != columns[i]) ListView_SetColumnWidth(list, i, columns[i]);
}

void Cell(int row, int column, std::wstring text)
{
    ListView_SetItemText(list, row, column, text.data());
}

bool MetadataLeafIs(const std::string& path, const char* expected)
{
    const size_t slash = path.find_last_of("/\\");
    const std::string leaf = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t length = std::strlen(expected);
    if (leaf.size() != length) return false;
    for (size_t i = 0; i < length; ++i) {
        const auto a = static_cast<unsigned char>(leaf[i]);
        const auto b = static_cast<unsigned char>(expected[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool IsRitualObject(const poe::EntitySnapshot& item)
{
    return MetadataLeafIs(item.metadataPath, "RitualRuneObject");
}

bool HasCandidateObject(const poe::EntitySnapshot& item)
{
    for (const auto& candidate : snapshot.candidates) {
        if (item.key && candidate.key == item.key && candidate.entity == item.entity) return true;
        if (IsRitualObject(item) && IsRitualObject(candidate) && candidate.x == item.x && candidate.y == item.y) return true;
    }
    return false;
}

std::string CandidateType(const poe::EntitySnapshot& item) {
    if (monster::BossEntrance(item.metadataPath)) return "Boss 入口";
    if (monster::MapBoss(item.combat)) return "终极 Boss";
    if (monster::OptionalElite(item.combat)) return "精英怪";
    return poe::RecipeOf(item.kind).displayName;
}
void Populate();

void InsertCandidateRow(const poe::EntitySnapshot& item)
{
    const int rowIndex = int(snapshot.candidates.size());
    snapshot.candidates.push_back(item);
    automationCells.emplace_back();
    auto index = std::to_wstring(size_t(rowIndex) + 1);
    LVITEMW row{}; row.mask = LVIF_TEXT; row.iItem = rowIndex; row.pszText = index.data();
    ListView_InsertItem(list, &row);
    Cell(rowIndex, 1, Wide(item.name.empty() ? "（无显示名称）" : item.name));
    Cell(rowIndex, 2, std::to_wstring(item.x)); Cell(rowIndex, 3, std::to_wstring(item.y));
    wchar_t key[32]{}; swprintf_s(key, L"0x%X", item.key); Cell(rowIndex, 4, key);
    Cell(rowIndex, 5, Wide(CandidateType(item)));
    Cell(rowIndex, 17, Wide(item.metadataPath));
}

bool SyncAutomationObjects(const ritual::Status& status)
{
    if (!status.area.Available()) return false;
    if (snapshot.area.Available() && !snapshot.area.SameMap(status.area)) return false;
    if (!snapshot.area.Available()) snapshot.area = status.area;
    const auto oldSize = snapshot.candidates.size();
    snapshot.candidates.erase(std::remove_if(snapshot.candidates.begin(), snapshot.candidates.end(), [&](const auto& item) {
        if (!item.combat.monster && !monster::BossEntrance(item.metadataPath)) return false;
        return std::none_of(status.points.begin(), status.points.end(), [&](const auto& point) {
            return point.object.key == item.key && point.object.entity == item.entity;
        });
    }), snapshot.candidates.end());
    bool added = oldSize != snapshot.candidates.size();
    if (added) Populate();
    for (const auto& point : status.points) {
        if (HasCandidateObject(point.object)) continue;
        InsertCandidateRow(point.object);
        added = true;
    }
    if (added) {
        scanText = L"路线目标／祭祀候选 " + std::to_wstring(snapshot.candidates.size()) + L" 行 · 来自巡回状态";
        InvalidateRect(panel, &headerRect, FALSE);
    }
    return added;
}

std::wstring PointStateText(ritual::PointState state)
{
    return Wide(ritual::StateText(state));
}

void UpdateAutomationRows(const ritual::Status& status)
{
    if (automationCells.size() != snapshot.candidates.size())
        automationCells.resize(snapshot.candidates.size());
    const bool sameArea = status.area.SameMap(snapshot.area);
    for (size_t row = 0; row < snapshot.candidates.size(); ++row) {
        auto& candidate = snapshot.candidates[row];
        std::array<std::wstring, 11> values{};
        auto gameState = candidate.ritual;
        if (sameArea) {
            const ritual::Point* match = nullptr;
            for (const auto& point : status.points) {
                if (point.object.key == candidate.key && point.object.entity == candidate.entity) {
                    match = &point; break;
                }
            }
            if (!match && IsRitualObject(candidate)) {
                for (const auto& point : status.points) {
                    if (point.object.x == candidate.x && point.object.y == candidate.y &&
                        MetadataLeafIs(point.object.metadataPath, "RitualRuneObject")) {
                        match = &point; break;
                    }
                }
            }
            if (match) {
                if (candidate.name != match->object.name) Cell(int(row), 1, Wide(match->object.name));
                if (candidate.x != match->object.x) Cell(int(row), 2, std::to_wstring(match->object.x));
                if (candidate.y != match->object.y) Cell(int(row), 3, std::to_wstring(match->object.y));
                candidate = match->object;
                const auto typeText = Wide(encounterroute::Text(match->kind));
                wchar_t currentType[256]{};
                ListView_GetItemText(list, int(row), 5, currentType, _countof(currentType));
                if (typeText != currentType) Cell(int(row), 5, typeText);
                values[0] = match->order ? std::to_wstring(match->order) : L"";
                values[1] = PointStateText(match->state);
                if (match->pathDistance >= 0.0) {
                    wchar_t distance[32]{};
                    swprintf_s(distance, L"%.1f", match->pathDistance);
                    values[2] = distance;
                }
                values[3] = match->interactable ? L"存在" : L"否";
                values[4] = match->light ? L"存在" : L"否";
                values[5] = match->blocker ? L"存在" : match->activationConfirmed ? L"已确认" : L"未确认";
                values[6] = std::to_wstring(match->activationAttempts);
                if (automationUsed) gameState = match->ritual;
            }
        }
        if (candidate.combat.monster || monster::BossEntrance(candidate.metadataPath)) {
            for (size_t c = 3; c < values.size(); ++c) values[c].clear();
            values[7] = monster::BossEntrance(candidate.metadataPath) ? L"橙色 Boss 入口" :
                L"主体 HP=" + std::to_wstring(candidate.hp) + L"，稀有度=" + std::to_wstring(candidate.combat.rarity);
        } else {
            values[7] = Wide(ritualstate::Text(gameState));
            values[8] = gameState.valid ? std::to_wstring(gameState.current) : L"?";
            values[9] = gameState.valid && gameState.hasInteraction ? std::to_wstring(gameState.interaction) : L"?";
            values[10] = gameState.valid && gameState.hasNpc ? std::to_wstring(gameState.disabledByNpc) : L"?";
        }
        auto& previous = automationCells[row];
        for (size_t column = 0; column < values.size(); ++column) {
            if (previous[column] != values[column]) {
                Cell(int(row), int(column) + 6, values[column]);
                previous[column] = std::move(values[column]);
            }
        }
    }
}

void SelectRow(int row)
{
    if (populating || row < 0 || size_t(row) >= snapshot.candidates.size()) return;
    selectedRow = row;
    const auto& item = snapshot.candidates[size_t(row)];
    SetWindowTextW(editX, std::to_wstring(item.x).c_str());
    SetWindowTextW(editY, std::to_wstring(item.y).c_str());
    wchar_t identity[160];
    swprintf_s(identity, L"key: 0x%X    entity: 0x%llX    坐标: (%d, %d)", item.key,
                static_cast<unsigned long long>(item.entity), item.x, item.y);
    const auto text = Wide(item.name.empty() ? "（无显示名称）" : item.name) + L"\r\n" + identity +
        L"\r\n" + Wide(item.metadataPath);
    SetWindowTextW(details, text.c_str());
}

void Populate()
{
    populating = true; selectedRow = -1;
    automationCells.assign(snapshot.candidates.size(), {});
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < snapshot.candidates.size(); ++i) {
        const auto& item = snapshot.candidates[i];
        auto index = std::to_wstring(i + 1);
        LVITEMW row{}; row.mask = LVIF_TEXT; row.iItem = int(i); row.pszText = index.data();
        ListView_InsertItem(list, &row);
        Cell(int(i), 1, Wide(item.name.empty() ? "（无显示名称）" : item.name));
        Cell(int(i), 2, std::to_wstring(item.x)); Cell(int(i), 3, std::to_wstring(item.y));
        wchar_t key[32]; swprintf_s(key, L"0x%X", item.key); Cell(int(i), 4, key);
        Cell(int(i), 5, Wide(CandidateType(item)));
        Cell(int(i), 17, Wide(item.metadataPath));
    }
    UpdateAutomationRows(ritual::MainAutomation().GetStatus());
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, FALSE);
    SetWindowTextW(details, L"选中候选对象后，这里显示完整名称、坐标、地址和 metadata。");
    populating = false;
}

void BeginScan()
{
    if (ritual::MainAutomation().GetStatus().running) {
        scanText = L"祭祀巡回运行中，暂停或停止后才能重新扫描。";
        InvalidateRect(panel, &headerRect, FALSE);
        return;
    }
    if (work.busy.exchange(true)) return;
    if (work.thread.joinable()) work.thread.join();
    scanAttempted = true;
    automationUsed = false;
    preserveActionText = false;
    work.stop = false; work.ready = false;
    snapshot = {}; haveScan = false; Populate();
    SetWindowTextW(editX, L""); SetWindowTextW(editY, L"");
    scanText = L"正在扫描未筛选的周围树…";
    Enable(scanButton, false); Enable(copyButton, false);
    try {
        work.thread = std::thread([] {
            usecase::RitualScanResult result;
            std::string error;
            try { usecase::ScanRitualCandidates(result, error, &work.stop); }
            catch (const std::exception& e) { error = e.what(); }
            catch (...) { error = "祭祀扫描出现异常"; }
            {
                std::lock_guard<std::mutex> lock(work.mutex);
                work.result = std::move(result); work.error = std::move(error);
            }
            work.ready = true;
        });
    } catch (...) {
        work.busy = false; scanText = L"无法启动祭祀扫描线程"; Enable(scanButton, true);
    }
    InvalidateRect(panel, &headerRect, FALSE);
}

bool Coordinate(HWND edit, int32_t& out)
{
    wchar_t text[32]{}; GetWindowTextW(edit, text, 32);
    wchar_t* end = nullptr; errno = 0;
    const auto value = wcstoll(text, &end, 10);
    if (end == text || errno == ERANGE || value < 0 || value > INT32_MAX) return false;
    while (*end && iswspace(*end)) ++end;
    if (*end) return false;
    out = int32_t(value); return true;
}

bool PositiveNumber(HWND edit, uint32_t minimum, uint32_t maximum, uint32_t& out)
{
    wchar_t text[32]{}; GetWindowTextW(edit, text, 32);
    wchar_t* begin = text;
    while (*begin && iswspace(*begin)) ++begin;
    if (!*begin || *begin == L'-') return false;
    wchar_t* end = nullptr; errno = 0;
    const auto value = wcstoul(begin, &end, 10);
    if (end == begin || errno == ERANGE || value < minimum || value > maximum) return false;
    while (*end && iswspace(*end)) ++end;
    if (*end) return false;
    out = uint32_t(value); return true;
}

bool ReadAutomationOptions(bool planOnly, ritual::Options& options, std::string& error)
{
    uint32_t interactDistance = 0, loadTimeout = 0, battleTimeout = 0, arenaRadius = 0, preclearRadius = 0;
    if (!PositiveNumber(editInteractDistance, 1, 1000, interactDistance) ||
        !PositiveNumber(editLoadTimeout, 1, 600, loadTimeout) ||
        !PositiveNumber(editBattleTimeout, 1, 3600, battleTimeout) ||
        !PositiveNumber(editArenaRadius, 30, 500, arenaRadius) ||
        !PositiveNumber(editPreclearRadius, 30, 500, preclearRadius)) {
        error = "交互距离、重试/刷新秒数须为正整数，战斗和开启前清怪半径请填写 30–500";
        return false;
    }
    options = {};
    options.planOnly = planOnly;
    if (haveScan) options.expectedArea = snapshot.area;
    options.interactDistance = int32_t(interactDistance);
    options.loadTimeoutSec = loadTimeout;
    options.battleTimeoutSec = battleTimeout;
    options.arenaRadius = int32_t(arenaRadius);
    options.preclearRadius = int32_t(preclearRadius);
    options.fightElites = SendMessageW(fightElitesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return true;
}

void InvokeAutomation(bool planOnly)
{
    ritual::Options options;
    std::string error;
    if (!ReadAutomationOptions(planOnly, options, error)) {
        preserveActionText = true;
        navigationText = Wide(error);
        InvalidateRect(panel, &statusRect, FALSE);
        return;
    }
    const auto navigation = navigationState ? navigationState() : NavigationState{};
    if (!navigation.canStart || work.busy.load() || !automate) {
        preserveActionText = true;
        navigationText = L"当前有寻路、跟随或技能操作，结束后才能启动祭祀路线。";
        InvalidateRect(panel, &statusRect, FALSE);
        return;
    }
    const auto previousRevision = ritual::MainAutomation().GetStatus().revision;
    if (automate(options, error)) {
        if (!haveScan) {
            snapshot = {};
            selectedRow = -1;
            Populate();
            scanAttempted = false;
        }
        automationUsed = true;
        preserveActionText = false;
        automationSyncAfterRevision = previousRevision + 1;
        haveNavigation = false;
        navigationText = planOnly ? L"正在规划祭祀路线…" : L"正在启动祭祀巡回…";
    } else {
        preserveActionText = true;
        navigationText = Wide(error.empty() ? "暂时无法启动祭祀巡回" : error);
    }
    InvalidateRect(panel, &statusRect, FALSE);
}

void TogglePause()
{
    auto& automation = ritual::MainAutomation();
    const auto status = automation.GetStatus();
    if (!status.running) return;
    automation.SetPaused(!status.paused);
}

void MarkSelectedCompleted()
{
    if (selectedRow < 0 || size_t(selectedRow) >= snapshot.candidates.size()) return;
    preserveActionText = true;
    const auto& item = snapshot.candidates[size_t(selectedRow)];
    if (!IsRitualObject(item)) {
        navigationText = L"只能手动标记 RitualRuneObject 行。";
    } else {
        auto& automation = ritual::MainAutomation();
        if (automation.GetStatus().running) {
            navigationText = L"巡回运行期间不能手动修改完成状态。";
        } else {
            std::string error;
            if (automation.MarkCompleted(item.x, item.y, error)) {
                automationUsed = true;
                preserveActionText = true;
                navigationText = L"已手动标记所选祭祀点完成。";
            } else navigationText = Wide(error.empty() ? "无法标记所选祭祀点" : error);
        }
    }
    InvalidateRect(panel, &statusRect, FALSE);
}

void RetrySelectedInteraction()
{
    if (selectedRow < 0 || size_t(selectedRow) >= snapshot.candidates.size()) return;
    preserveActionText = true;
    const auto& item = snapshot.candidates[size_t(selectedRow)];
    if (!IsRitualObject(item)) return;
    auto& automation = ritual::MainAutomation();
    if (automation.GetStatus().running) {
        navigationText = L"请先停止祭祀巡回，再授权重试该点交互。";
    } else {
        std::string error;
        if (automation.ResetInteraction(item.x, item.y, error)) {
            automationUsed = true;
            preserveActionText = true;
            navigationText = L"已清除该点的交互发送记录；再次开始巡回后才会重试，不会自动重发。";
        } else navigationText = Wide(error.empty() ? "无法重置该点的交互状态" : error);
    }
    InvalidateRect(panel, &statusRect, FALSE);
}

void BeginNavigation()
{
    haveNavigation = false;
    if (ritual::MainAutomation().GetStatus().running) {
        navigationText = L"祭祀巡回运行中，请先暂停或停止巡回。";
        InvalidateRect(panel, &statusRect, FALSE);
        return;
    }
    Target target;
    if (!Coordinate(editX, target.x) || !Coordinate(editY, target.y)) {
        navigationText = L"X / Y 必须为非负十进制整数。";
    } else {
        // Manual edits detach a selected row from its old instance.
        if (selectedRow >= 0 && size_t(selectedRow) < snapshot.candidates.size()) {
            const auto& selected = snapshot.candidates[size_t(selectedRow)];
            if (selected.x == target.x && selected.y == target.y) target.area = snapshot.area;
        }
        std::string error;
        if (navigate && navigate(target, error)) {
            haveNavigation = true;
            automationUsed = false;
            preserveActionText = false;
            navigationText = L"正在获取地图并规划 A* 路径…";
        } else navigationText = Wide(error.empty() ? "暂时无法启动寻路" : error);
    }
    InvalidateRect(panel, &statusRect, FALSE);
}

bool CopyTable()
{
    std::wstring text = L"地图\t地图ID\t名称\tX\tY\tkey\tentity\t类型\t路线顺序\t状态\t路径距离\tInteractable\tLight\tBlocker\t交互次数\t游戏状态\tcurrent_state\tinteraction_enabled\tdisabled_by_ritual_npc\tmetadata\r\n";
    for (size_t i = 0; i < snapshot.candidates.size(); ++i) {
        const auto& item = snapshot.candidates[i];
        wchar_t values[160]; swprintf_s(values, L"\t%d\t%d\t0x%X\t0x%llX\t", item.x, item.y,
            item.key, static_cast<unsigned long long>(item.entity));
        text += Wide(snapshot.area.name) + L"\t" + Wide(snapshot.area.id) + L"\t" +
            Wide(item.name.empty() ? "（无显示名称）" : item.name) + values +
            Wide(CandidateType(item));
        for (const auto& cell : automationCells[i]) text += L"\t" + cell;
        text += L"\t" + Wide(item.metadataPath) + L"\r\n";
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) return false;
    void* data = GlobalLock(handle);
    if (!data) { GlobalFree(handle); return false; }
    memcpy(data, text.c_str(), bytes); GlobalUnlock(handle);
    if (!OpenClipboard(panel)) { GlobalFree(handle); return false; }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, handle);
    CloseClipboard(); if (!copied) GlobalFree(handle);
    return copied;
}

bool CopyRouteToClipboard()
{
    const auto status = ritual::MainAutomation().GetStatus();
    if (!status.area.SameMap(snapshot.area) || status.route.empty()) return false;
    std::wstring text = L"序号\tX\tY\r\n";
    for (size_t i = 0; i < status.route.size(); ++i) {
        text += std::to_wstring(i + 1) + L"\t" + std::to_wstring(status.route[i].x) + L"\t" +
            std::to_wstring(status.route[i].y) + L"\r\n";
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) return false;
    void* data = GlobalLock(handle);
    if (!data) { GlobalFree(handle); return false; }
    memcpy(data, text.c_str(), bytes); GlobalUnlock(handle);
    if (!OpenClipboard(panel)) { GlobalFree(handle); return false; }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, handle);
    CloseClipboard(); if (!copied) GlobalFree(handle);
    return copied;
}

void Tick()
{
    if (work.ready.exchange(false)) {
        if (work.thread.joinable()) work.thread.join();
        std::string error;
        {
            std::lock_guard<std::mutex> lock(work.mutex);
            snapshot = std::move(work.result); error = std::move(work.error);
        }
        work.busy = false; haveScan = error.empty(); Populate();
        if (!error.empty()) {
            scanText = L"扫描失败：" + Wide(error);
            SetWindowTextW(details, scanText.c_str());
        }
        else {
            std::set<std::pair<int32_t, int32_t>> positions;
            for (const auto& item : snapshot.candidates) positions.emplace(item.x, item.y);
            scanText = L"候选 " + std::to_wstring(snapshot.candidates.size()) + L" 个 · 坐标点 " +
                std::to_wstring(positions.size()) + L" 处 · 已遍历 " + std::to_wstring(snapshot.visited) + L" 个节点";
        }
        InvalidateRect(panel, &headerRect, FALSE);
    }
    const auto state = navigationState ? navigationState() : NavigationState{};
    const auto autoStatus = ritual::MainAutomation().GetStatus();
    const bool canSyncAutomation = automationUsed && autoStatus.revision > automationSyncAfterRevision &&
        !work.busy.load() && (!scanAttempted || haveScan);
    if (canSyncAutomation) SyncAutomationObjects(autoStatus);
    UpdateAutomationRows(autoStatus);
    const bool sameAutomationArea = autoStatus.area.SameMap(snapshot.area);
    const bool autoRunning = autoStatus.running;
    if (automationUsed && sameAutomationArea && !preserveActionText) {
        size_t completed = 0, required = 0;
        for (const auto& point : autoStatus.points) {
            if (point.kind == ritual::PointKind::BossEntrance) continue;
            ++required;
            if (point.state == ritual::PointState::Completed) ++completed;
        }
        std::wstring text = autoStatus.paused ? L"祭祀巡回已暂停" : L"祭祀巡回 · ";
        if (!autoStatus.paused)
            text += std::to_wstring(completed) + L" / " + std::to_wstring(required) + L" 已完成";
        if (autoStatus.active >= 0 && size_t(autoStatus.active) < autoStatus.points.size()) {
            const auto& point = autoStatus.points[size_t(autoStatus.active)];
            text += L" · 当前点 " + std::to_wstring(point.order ? point.order : uint32_t(autoStatus.active + 1)) +
                L" (" + std::to_wstring(point.object.x) + L", " + std::to_wstring(point.object.y) + L")";
            if (!point.detail.empty()) text += L" · " + Wide(point.detail);
        }
        if (!autoStatus.text.empty()) text += L" · " + Wide(autoStatus.text);
        if (text != navigationText) { navigationText = std::move(text); InvalidateRect(panel, &statusRect, FALSE); }
    } else if (haveNavigation && !preserveActionText) {
        const auto next = Wide(state.text) + (state.detail.empty() ? L"" : L" · " + Wide(state.detail));
        if (next != navigationText) { navigationText = next; InvalidateRect(panel, &statusRect, FALSE); }
        if (!state.running) haveNavigation = false;
    }

    const bool canStart = state.canStart && !work.busy;
    Enable(scanButton, !work.busy && !autoRunning);
    Enable(copyButton, !snapshot.candidates.empty());
    Enable(copyRouteButton, sameAutomationArea && !autoStatus.route.empty());
    Enable(goButton, canStart && !autoRunning);
    Enable(planButton, canStart && !autoRunning);
    Enable(fightElitesCheck, !autoRunning);
    Enable(startAutomationButton, autoStatus.paused || (canStart && !autoRunning));
    Enable(pauseAutomationButton, autoRunning);
    Enable(stopButton, state.running || autoRunning);
    const bool selectedObject = selectedRow >= 0 && size_t(selectedRow) < snapshot.candidates.size() &&
        IsRitualObject(snapshot.candidates[size_t(selectedRow)]);
    Enable(markCompletedButton, selectedObject && !autoRunning && !state.running && sameAutomationArea);
    Enable(retryInteractionButton, selectedObject && !autoRunning && !state.running && sameAutomationArea);
    SetTextIfChanged(startAutomationButton, autoStatus.paused ? L"继续巡回" : L"开始巡回");
    SetTextIfChanged(pauseAutomationButton, autoStatus.paused ? L"继续巡回" : L"暂停巡回");
}

void Paint(HWND hwnd)
{
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{}; GetClientRect(hwnd, &rc);
    HDC buffer = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ previous = SelectObject(buffer, bitmap);
    FillRect(buffer, &rc, ui::Background()); ui::Card(buffer, headerRect); ui::Card(buffer, actionsRect);
    const auto title = snapshot.area.Available() ? L"祭祀候选 · " + Wide(snapshot.area.name) + L"  /  " + Wide(snapshot.area.id) :
        std::wstring(L"祭祀候选对象");
    ui::Text(buffer, title.c_str(), {14, 8, rc.right - 398, 35}, font, ui::color::kText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    ui::Text(buffer, scanText.c_str(), {14, 42, rc.right - 14, 66}, font, ui::color::kTextDim,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    ui::Text(buffer, L"X", {12, 90, 32, 118}, font); ui::Text(buffer, L"Y", {110, 90, 130, 118}, font);
    ui::Text(buffer, L"交互距离", {374, 90, 442, 118}, font, ui::color::kTextDim);
    ui::Text(buffer, L"重试(s)", {488, 90, 548, 118}, font, ui::color::kTextDim,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    ui::Text(buffer, L"刷新(s)", {8, 128, 78, 156}, font, ui::color::kTextDim);
    ui::Text(buffer, L"战斗半径", {144, 128, 212, 156}, font, ui::color::kTextDim);
    ui::Text(buffer, L"开启前清怪半径", {280, 128, 392, 156}, font, ui::color::kTextDim);
    ui::Text(buffer, L"（待实测）", {452, 128, 550, 156}, font, ui::color::kTextDim);
    ui::Text(buffer, L"按路线处理祭祀和所选精英；终极 Boss 击杀确认后自动开门返回藏身處。", {10, 239, rc.right - 10, 258},
        font, ui::color::kTextDim);
    ui::Text(buffer, navigationText.c_str(), {12, statusRect.top, rc.right - 12, statusRect.bottom}, font,
        ui::color::kAccent, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, previous); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(hwnd, &ps);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    LRESULT themed = 0;
    if (ui::HandleMessage(hwnd, message, wp, lp, themed)) return themed;
    switch (message) {
    case WM_SIZE: if (list) Layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_TIMER: if (wp == 1) Tick(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case FightElites:
            WritePrivateProfileStringW(L"ritual", L"fightElites",
                SendMessageW(fightElitesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0",
                app::Config().RuntimeFileW(L"ritual.ini").c_str());
            return 0;
        case Scan: BeginScan(); return 0;
        case Go: BeginNavigation(); return 0;
        case Stop: if (stopNavigation) stopNavigation(); return 0;
        case Plan: InvokeAutomation(true); return 0;
        case StartAutomation: {
            const auto status = ritual::MainAutomation().GetStatus();
            if (status.running && status.paused) {
                ritual::MainAutomation().SetPaused(false);
                automationUsed = true;
                preserveActionText = false;
                automationSyncAfterRevision = 0;
            } else if (!status.running) InvokeAutomation(false);
            return 0;
        }
        case PauseAutomation: TogglePause(); return 0;
        case MarkCompleted: MarkSelectedCompleted(); return 0;
        case RetryInteraction: RetrySelectedInteraction(); return 0;
        case OpenLogs: {
            const auto path = app::Config().RuntimeFileW(L"ritual_logs");
            CreateDirectoryW(path.c_str(), nullptr);
            if (INT_PTR(ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
                navigationText = L"日志目录：" + path;
                InvalidateRect(hwnd, &statusRect, FALSE);
            }
            return 0;
        }
        case Copy:
            navigationText = CopyTable() ? L"已复制候选表格，可粘贴到 Excel 或文本。" : L"复制失败，剪贴板暂不可用。";
            InvalidateRect(hwnd, &statusRect, FALSE); return 0;
        case CopyRoute:
            navigationText = CopyRouteToClipboard() ? L"已复制 A* 路线坐标，可粘贴到 Excel 或文本。" : L"暂无可复制的路线坐标。";
            InvalidateRect(hwnd, &statusRect, FALSE); return 0;
        } break;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lp);
        if (header->hwndFrom == list && header->code == NM_CLICK && !populating) {
            SelectRow(ListView_GetNextItem(list, -1, LVNI_SELECTED)); return 0;
        }
        if (header->hwndFrom == list && header->code == LVN_ITEMCHANGED && !populating) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED))
                SelectRow(change->iItem);
            return 0;
        }
        break;
    }
    case WM_DESTROY: KillTimer(hwnd, 1); Shutdown(); panel = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

void CreatePanel(HWND parent, std::function<bool(const Target&, std::string&)> start,
                 std::function<void()> stop, std::function<NavigationState()> state,
                 std::function<bool(const ritual::Options&, std::string&)> automation)
{
    if (panel) return;
    navigate = std::move(start); stopNavigation = std::move(stop); navigationState = std::move(state);
    automate = std::move(automation);
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"Poe2RitualPanel"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    panel = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 600, 600, parent, nullptr, instance, nullptr);
    font = ui::Font();
    auto make = [&](const wchar_t* cls, const wchar_t* text, int id, DWORD style) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            0, 0, 100, 30, panel, HMENU(INT_PTR(id)), instance, nullptr);
    };
    scanButton = make(L"BUTTON", L"扫描祭祀候选", Scan, BS_PUSHBUTTON);
    copyButton = make(L"BUTTON", L"复制候选表格", Copy, BS_PUSHBUTTON);
    copyRouteButton = make(L"BUTTON", L"复制路线", CopyRoute, BS_PUSHBUTTON);
    goButton = make(L"BUTTON", L"A* 寻路到坐标", Go, BS_PUSHBUTTON);
    planButton = make(L"BUTTON", L"规划路线", Plan, BS_PUSHBUTTON);
    startAutomationButton = make(L"BUTTON", L"开始巡回", StartAutomation, BS_PUSHBUTTON);
    pauseAutomationButton = make(L"BUTTON", L"暂停巡回", PauseAutomation, BS_PUSHBUTTON);
    stopButton = make(L"BUTTON", L"停止", Stop, BS_PUSHBUTTON);
    retryInteractionButton = make(L"BUTTON", L"重试交互", RetryInteraction, BS_PUSHBUTTON);
    markCompletedButton = make(L"BUTTON", L"标记所选点完成", MarkCompleted, BS_PUSHBUTTON);
    logsButton = make(L"BUTTON", L"打开日志", OpenLogs, BS_PUSHBUTTON);
    editX = make(L"EDIT", L"", X, ES_AUTOHSCROLL); editY = make(L"EDIT", L"", Y, ES_AUTOHSCROLL);
    editInteractDistance = make(L"EDIT", L"18", InteractDistance, ES_AUTOHSCROLL | ES_NUMBER);
    editLoadTimeout = make(L"EDIT", L"20", LoadTimeout, ES_AUTOHSCROLL | ES_NUMBER);
    editBattleTimeout = make(L"EDIT", L"240", BattleTimeout, ES_AUTOHSCROLL | ES_NUMBER);
    editArenaRadius = make(L"EDIT", L"80", ArenaRadius, ES_AUTOHSCROLL | ES_NUMBER);
    editPreclearRadius = make(L"EDIT", L"120", PreclearRadius, ES_AUTOHSCROLL | ES_NUMBER);
    fightElitesCheck = make(L"BUTTON", L"打精英怪（与祭祀混合规划，终极 Boss 最后）", FightElites, BS_AUTOCHECKBOX);
    SendMessageW(fightElitesCheck, BM_SETCHECK,
        GetPrivateProfileIntW(L"ritual", L"fightElites", 0, app::Config().RuntimeFileW(L"ritual.ini").c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
    details = make(L"EDIT", L"选中候选对象后，这里显示完整名称、坐标、地址和 metadata。", Details,
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
    list = make(WC_LISTVIEWW, L"", Candidates, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t* titles[] = {L"序号", L"名称", L"X", L"Y", L"key", L"类型", L"路线序", L"状态", L"路径距离", L"Interactable", L"Light", L"Blocker", L"交互次数", L"游戏状态", L"current_state", L"interaction_enabled", L"disabled_by_ritual_npc", L"metadata"};
    for (int i = 0; i < 18; ++i) {
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = 100;
        column.pszText = const_cast<wchar_t*>(titles[i]); ListView_InsertColumn(list, i, &column);
    }
    for (auto control : {scanButton, copyButton, copyRouteButton, editX, editY, editInteractDistance,
                         editLoadTimeout, editBattleTimeout, editArenaRadius, editPreclearRadius, fightElitesCheck, details, list})
        ui::StyleControl(control, font);
    ui::StyleControl(goButton, font, ui::ButtonStyle::Primary);
    ui::StyleControl(planButton, font, ui::ButtonStyle::Primary);
    ui::StyleControl(startAutomationButton, font, ui::ButtonStyle::Primary);
    ui::StyleControl(pauseAutomationButton, font);
    ui::StyleControl(retryInteractionButton, font);
    ui::StyleControl(markCompletedButton, font);
    ui::StyleControl(logsButton, font);
    ui::StyleControl(stopButton, font, ui::ButtonStyle::Danger);
    SendMessageW(editX, EM_SETLIMITTEXT, 11, 0); SendMessageW(editY, EM_SETLIMITTEXT, 11, 0);
    SendMessageW(editInteractDistance, EM_SETLIMITTEXT, 4, 0);
    SendMessageW(editLoadTimeout, EM_SETLIMITTEXT, 4, 0);
    SendMessageW(editBattleTimeout, EM_SETLIMITTEXT, 4, 0);
    SendMessageW(editArenaRadius, EM_SETLIMITTEXT, 3, 0);
    SendMessageW(editPreclearRadius, EM_SETLIMITTEXT, 3, 0);
    Enable(copyButton, false); Enable(copyRouteButton, false); Enable(stopButton, false);
    Enable(pauseAutomationButton, false); Enable(markCompletedButton, false);
    Enable(retryInteractionButton, false);
    Layout(); SetTimer(panel, 1, 150, nullptr);
}

void ShowPanel(bool visible, const RECT& bounds)
{
    if (!panel) return;
    if (visible) Place(panel, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (bool(IsWindowVisible(panel)) != visible) ShowWindow(panel, visible ? SW_SHOW : SW_HIDE);
}

void Shutdown()
{
    work.stop = true;
    if (work.thread.joinable()) work.thread.join();
    work.busy = false; work.ready = false;
}
} // namespace ritualui
