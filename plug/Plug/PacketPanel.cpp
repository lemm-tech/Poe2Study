#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "PacketPanel.h"
#include "UiTheme.h"
#include "UseCases.h"
#include "NamedPipeClie.h"
#include "../Common/AppConfig.h"
#include "../Common/PacketProtocol.h"
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <deque>
#include <fstream>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace packetui {
namespace {
enum { Refresh = 7101, Items, Key, Pick, ActionId, X, Y, Flags, ActionSend,
       EndSend, Plain, PlainSend, Append, Preview, Check, CaptureSend, CaptureRecv,
       CaptureStart, CaptureStop, Records, Filter, Clear, Export, Replay,
       PortalRefresh, Portals, PortalEnter, PortalKey,
       PagePick = 7150, PageAction, PagePlain, PagePortal };
constexpr int kModeCount = 4, kPortalPage = PagePortal - PagePick;
HWND window = nullptr, modeTabs[kModeCount]{}, status = nullptr, details = nullptr, itemList = nullptr, portalList = nullptr;
HWND logList = nullptr, filter = nullptr;
HFONT font = nullptr, mono = nullptr, titleFont = nullptr;
int selectedPage = 0, scrollOffset = 0, contentHeight = 0;
bool layingOut = false;
std::vector<HWND> pages[kModeCount];
std::vector<poe::EntitySnapshot> items, portals;
std::function<bool(std::string&)> canSendAction;
uint64_t portalSession = 0, portalController = 0;
struct Record { uint64_t id = 0; uint32_t direction = 0; bool truncated = false; std::wstring time, label, text; std::string hex; };
std::deque<Record> records;
std::vector<size_t> visible;
std::unique_ptr<pipe::NamedPipeClie> observer;
std::thread worker;
std::atomic_bool busy{false}, ready{false}, stopping{false};
std::mutex resultsMutex, incomingMutex;
std::deque<packet::CaptureEvent> incoming;
std::atomic<uint64_t> uiDropped{0};
uint64_t recordSequence = 0, receivedCount = 0, serverDropped = 0;
uint32_t localCapture = 0;
uint64_t observedSession = 0, observedController = 0;
bool populating = false;
struct WorkResult {
    bool scan = false, portalScan = false, received = false;
    std::vector<poe::EntitySnapshot> items;
    packet::Reply reply{};
    std::string rawPreview;
    uint64_t sessionId = 0, controllerId = 0;
    std::wstring error;
} workResult;

std::wstring Wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), count);
    return result;
}
std::string Hex(const uint8_t* bytes, size_t length) {
    const char digits[] = "0123456789ABCDEF"; std::string text;
    for (size_t i = 0; i < length; ++i) {
        if (i) text += ' ';
        text += digits[bytes[i] >> 4]; text += digits[bytes[i] & 15];
    }
    return text;
}
std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(size_t(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1); value.resize(length); return value;
}
HWND Control(int id) { return GetDlgItem(window, id); }
bool SetTextIfChanged(HWND control, const std::wstring& value) {
    if (!control || Text(control) == value) return false;
    SetWindowTextW(control, value.c_str());
    return true;
}
bool EnableIfChanged(HWND control, bool enabled) {
    if (!control || (IsWindowEnabled(control) != FALSE) == enabled) return false;
    EnableWindow(control, enabled);
    return true;
}
void Say(const std::wstring& value) { SetTextIfChanged(status, value); }
HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int page = -1,
          ui::ButtonStyle buttonStyle = ui::ButtonStyle::Secondary) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
        parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    ui::StyleControl(control, font, buttonStyle);
    if (!wcscmp(cls, L"STATIC")) ui::StyleLabel(control, font, true);
    if (page >= 0) pages[page].push_back(control);
    return control;
}
void Move(HWND control, int x, int y, int width, int height) {
    RECT current{}; GetWindowRect(control, &current); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&current), 2);
    y -= scrollOffset; width = (std::max)(1, width); height = (std::max)(1, height);
    if (current.left == x && current.top == y && current.right - current.left == width && current.bottom - current.top == height) return;
    SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void ColumnWidth(HWND list, int index, int width) {
    if (ListView_GetColumnWidth(list, index) != width) ListView_SetColumnWidth(list, index, width);
}
bool Checked(int id) { return SendMessageW(Control(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetCell(HWND list, int row, int column, const std::wstring& value) {
    ListView_SetItemText(list, row, column, const_cast<wchar_t*>(value.c_str()));
}
void AddRow(HWND list, int row, const std::wstring& value) {
    LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row; item.pszText = const_cast<wchar_t*>(value.c_str());
    ListView_InsertItem(list, &item);
}
void Columns(HWND list, std::initializer_list<std::pair<const wchar_t*, int>> columns) {
    int index = 0;
    for (auto column : columns) {
        LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = const_cast<wchar_t*>(column.first); c.cx = column.second;
        ListView_InsertColumn(list, index++, &c);
    }
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
}
bool Number(int id, uint32_t& value, int base = 16, bool signedValue = false) {
    auto text = Text(Control(id));
    if (text.empty() || (!signedValue && text.front() == L'-')) { Say(L"请完整填写数值"); return false; }
    try {
        size_t end = 0;
        if (signedValue) {
            const auto parsed = std::stoll(text, &end, 10);
            if (end != text.size() || parsed < INT32_MIN || parsed > INT32_MAX) throw std::out_of_range("coordinate");
            value = uint32_t(int32_t(parsed));
        } else {
            const auto parsed = std::stoull(text, &end, base);
            if (end != text.size() || parsed > UINT32_MAX) throw std::out_of_range("number");
            value = uint32_t(parsed);
        }
    } catch (...) { Say(L"数值格式错误：key、动作 ID 和标志用十六进制，坐标用十进制"); return false; }
    return true;
}
bool PlainBytes(packet::Request& request) {
    const auto text = Text(Control(Plain)); int high = -1;
    for (wchar_t ch : text) {
        if (iswspace(ch)) continue;
        const int digit = ch >= L'0' && ch <= L'9' ? ch - L'0' :
                          ch >= L'a' && ch <= L'f' ? ch - L'a' + 10 :
                          ch >= L'A' && ch <= L'F' ? ch - L'A' + 10 : -1;
        if (digit < 0) { Say(L"明文只接受十六进制字节和空白，例如 01 A5 01"); return false; }
        if (high < 0) high = digit;
        else {
            if (request.length == packet::kMaxBytes) { Say(L"单次明文最多 1536 字节"); return false; }
            request.bytes[request.length++] = uint8_t(high * 16 + digit); high = -1;
        }
    }
    if (high >= 0 || !request.length) { Say(L"请填写完整的十六进制字节"); return false; }
    return true;
}
void OnCapture(MessageType type, const void* data, size_t size) {
    if (stopping || type != MT_PacketCapture || size != sizeof(packet::CaptureEvent)) return;
    packet::CaptureEvent event{}; std::memcpy(&event, data, size);
    if (event.version != packet::kVersion || event.length > packet::kMaxBytes ||
        (event.direction != packet::Send && event.direction != packet::Receive)) return;
    std::lock_guard<std::mutex> lock(incomingMutex);
    if (incoming.size() >= 512) { ++uiDropped; return; }
    incoming.push_back(event);
}
bool Observer() {
    if (observer && observer->IsConnected()) return true;
    observer.reset(new pipe::NamedPipeClie(app::Config().PipeFullName(), OnCapture, managed::Role::Observer));
    return observer->Initialize();
}
WorkResult Call(const packet::Request& request, uint64_t expectedSession = 0, uint64_t expectedController = 0) {
    WorkResult result;
    if (!app::Config().TargetAlive()) { result.error = L"目标游戏已退出"; return result; }
    if (request.operation == packet::Operation::Capture && request.captureMask && !Observer()) {
        result.error = L"无法连接封包记录通道，请确认使用新版 Monitor.dll"; return result;
    }
    pipe::NamedPipeClie client(app::Config().PipeFullName(), nullptr, managed::Role::Controller);
    if (!client.Initialize()) { result.error = L"管道尚未就绪，请先完成 DLL 连接"; return result; }
    if (expectedSession && (client.SessionId() != expectedSession || client.ClientId() != expectedController)) {
        result.error = L"传送门列表所属通信会话已变化，未发送，请重新刷新"; return result;
    }
    packet::Message message(request); std::vector<uint8_t> body;
    if (!client.Request(&message, MT_PacketReply, body, 8000) || body.size() != sizeof(result.reply)) {
        result.error = L"未收到封包回执；未自动重发，请先检查游戏状态"; return result;
    }
    std::memcpy(&result.reply, body.data(), body.size());
    if (result.reply.version != packet::kVersion || result.reply.objectLength > sizeof(result.reply.object)) {
        result.error = L"封包回执版本或长度不匹配"; return result;
    }
    result.reply.detail[sizeof(result.reply.detail) - 1] = 0;
    result.sessionId = client.SessionId(); result.controllerId = client.ClientId();
    if (request.operation == packet::Operation::SendPlain && (request.options & packet::PreviewOnly))
        result.rawPreview = Hex(request.bytes, request.length);
    result.received = true; return result;
}
void Start(std::function<WorkResult()> task) {
    if (busy.exchange(true)) return;
    if (worker.joinable()) worker.join();
    try {
        worker = std::thread([task] {
            WorkResult result;
            try { result = task(); } catch (const std::exception& error) { result.error = Wide(error.what()); }
            catch (...) { result.error = L"封包操作发生异常"; }
            { std::lock_guard<std::mutex> lock(resultsMutex); workResult = std::move(result); }
            ready = true;
        });
        Say(L"正在处理…");
    } catch (...) { busy = false; Say(L"无法创建操作线程"); }
}
bool RequestFromPage(packet::Request& request) {
    const int page = selectedPage;
    if (page == kPortalPage) {
        const auto connection = pipe::GetConnectionSnapshot();
        if (!connection.ready || connection.sessionId != portalSession || connection.controllerId != portalController) {
            Say(L"传送门列表所属会话已失效，请重新刷新传送门"); return false;
        }
        const int row = ListView_GetNextItem(portalList, -1, LVNI_SELECTED);
        if (row < 0 || size_t(row) >= portals.size()) { Say(L"请先刷新并选中一个传送门"); return false; }
        const auto& portal = portals[size_t(row)];
        if (portal.kind != poe::EntityKind::Portal || !portal.key || !portal.entity) { Say(L"传送门记录无效，请重新刷新"); return false; }
        request.operation = packet::Operation::EnterPortal;
        request.itemKey = portal.key; request.expectedEntity = portal.entity;
        request.actionFlags = packet::kInteractFlags;
    } else if (page == 0) {
        if (Checked(Append)) request.options |= packet::AppendEnd;
        request.operation = packet::Operation::Pick;
        if (!Number(Key, request.itemKey)) return false;
        if (!request.itemKey) { Say(L"请选择掉落物或填写非零的实体 key"); return false; }
        request.actionFlags = 0xFF000400;
        for (const auto& item : items) if (item.key == request.itemKey) request.expectedEntity = item.entity;
    } else if (page == 1) {
        if (Checked(Append)) request.options |= packet::AppendEnd;
        request.operation = packet::Operation::Action;
        uint32_t x = 0, y = 0;
        if (!Number(ActionId, request.actionId) || !Number(X, x, 10, true) || !Number(Y, y, 10, true) ||
            !Number(Flags, request.actionFlags)) return false;
        request.x = int32_t(x); request.y = int32_t(y);
    } else {
        request.operation = packet::Operation::SendPlain;
        request.options = 0;
        if (!PlainBytes(request)) return false;
    }
    return true;
}
void RefreshItems(bool portalScan = false) {
    Start([portalScan] {
        WorkResult result; result.scan = true; result.portalScan = portalScan;
        const auto connection = pipe::GetConnectionSnapshot();
        std::string error; std::vector<poe::EntitySnapshot> all;
        if (!usecase::ScanSurroundings(all, error)) result.error = Wide(error);
        if (portalScan && !connection.Same(pipe::GetConnectionSnapshot())) result.error = L"扫描期间通信会话变化，请重新刷新传送门";
        result.sessionId = connection.sessionId; result.controllerId = connection.controllerId;
        if (result.error.empty()) for (auto& item : all)
            if (item.kind == (portalScan ? poe::EntityKind::Portal : poe::EntityKind::WorldItem)) result.items.push_back(std::move(item));
        return result;
    });
}
void PopulateItems(HWND list, const std::vector<poe::EntitySnapshot>& entries) {
    populating = true;
    ListView_DeleteAllItems(list);
    int row = 0;
    for (const auto& item : entries) {
        wchar_t key[24], entity[32]; swprintf_s(key, L"%08X", item.key); swprintf_s(entity, L"%llX", item.entity);
        AddRow(list, row, item.name.empty() ? (list == portalList ? L"未命名传送门" : L"未命名掉落物") : Wide(item.name));
        SetCell(list, row, 1, key); SetCell(list, row, 2, entity);
        SetCell(list, row, 3, std::to_wstring(item.x) + L", " + std::to_wstring(item.y)); ++row;
    }
    populating = false;
}
void PopulateRecords() {
    populating = true;
    uint64_t selectedId = 0;
    const int selected = ListView_GetNextItem(logList, -1, LVNI_SELECTED);
    if (selected >= 0 && size_t(selected) < visible.size() && visible[size_t(selected)] < records.size())
        selectedId = records[visible[size_t(selected)]].id;
    const int top = ListView_GetTopIndex(logList);
    const bool atBottom = !selectedId && top + ListView_GetCountPerPage(logList) >= int(visible.size()) - 1;
    const int selectedFilter = int(SendMessageW(filter, CB_GETCURSEL, 0, 0));
    SendMessageW(logList, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(logList); visible.clear();
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        if (selectedFilter == 1 && record.direction != packet::Send) continue;
        if (selectedFilter == 2 && record.direction != packet::Receive) continue;
        const int row = int(visible.size()); visible.push_back(i); AddRow(logList, row, record.time);
        SetCell(logList, row, 1, record.label); SetCell(logList, row, 2, record.text);
        SetCell(logList, row, 3, Wide(record.hex.substr(0, 180)));
        if (record.id == selectedId) ListView_SetItemState(logList, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(logList, WM_SETREDRAW, TRUE, 0); InvalidateRect(logList, nullptr, FALSE);
    if (!visible.empty()) {
        const int row = ListView_GetNextItem(logList, -1, LVNI_SELECTED);
        ListView_EnsureVisible(logList, row >= 0 ? row : atBottom ? int(visible.size()) - 1 : (std::min)(top, int(visible.size()) - 1), FALSE);
    }
    populating = false;
}
void AddRecord(Record record) {
    SYSTEMTIME now{}; GetLocalTime(&now); wchar_t time[32];
    swprintf_s(time, L"%02u:%02u:%02u.%03u", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    record.id = ++recordSequence; record.time = time; records.push_back(std::move(record));
    if (records.size() > 1000) {
        records.pop_front();
        for (auto& index : visible) index = index ? index - 1 : SIZE_MAX;
    }
}
void Consume() {
    bool changed = false;
    if (ready.exchange(false)) {
        if (worker.joinable()) worker.join(); busy = false;
        WorkResult result;
        { std::lock_guard<std::mutex> lock(resultsMutex); result = std::move(workResult); }
        if (result.scan && result.portalScan) {
            portals = std::move(result.items); portalSession = result.sessionId; portalController = result.controllerId;
            PopulateItems(portalList, portals); SetWindowTextW(Control(PortalKey), L"");
        } else if (result.scan) { items = std::move(result.items); PopulateItems(itemList, items); }
        if (!result.error.empty()) Say(result.error);
        else if (result.received) {
            localCapture = result.reply.captureMask;
            observedSession = result.sessionId; observedController = result.controllerId;
            serverDropped = result.reply.dropped;
            const auto detail = Wide(result.reply.detail);
            Say(detail + L"；组包/写入完成 " + std::to_wstring(result.reply.invoked) + L" 次");
            Record record; record.label = L"调用回执"; record.text = detail;
            record.hex = result.rawPreview.empty() ? Hex(result.reply.object, result.reply.objectLength) : result.rawPreview;
            AddRecord(std::move(record)); changed = true;
            if (!records.back().hex.empty()) SetWindowTextW(details, Wide(records.back().hex).c_str());
        } else if (result.scan) Say(result.portalScan ? L"已读取 " + std::to_wstring(portals.size()) + L" 个传送门，选中后可发送进门请求" :
            L"已读取 " + std::to_wstring(items.size()) + L" 个掉落物，选中一行后可拾取");
    }
    std::deque<packet::CaptureEvent> received;
    { std::lock_guard<std::mutex> lock(incomingMutex); received.swap(incoming); }
    for (const auto& event : received) {
        Record record; record.direction = event.direction;
        record.truncated = (event.flags & packet::Truncated) != 0; ++receivedCount;
        record.label = event.direction == packet::Send ? L"发送明文" : L"接收片段";
        record.text = L"#" + std::to_wstring(event.sequence) + L"  " + std::to_wstring(event.originalLength) + L" 字节";
        if (event.flags & packet::Truncated) record.text += L"（已截断）";
        record.hex = Hex(event.bytes, event.length); AddRecord(std::move(record)); changed = true;
    }
    if (changed) PopulateRecords();
    const auto connection = pipe::GetConnectionSnapshot();
    if (observedSession && (observedSession != connection.sessionId || observedController != connection.controllerId)) {
        localCapture = 0; observedSession = observedController = 0; Say(L"通信会话已变化，收发记录已停止，可重新开启");
    }
    if (portalSession && (!connection.ready || portalSession != connection.sessionId || portalController != connection.controllerId)) {
        portals.clear(); portalSession = portalController = 0; PopulateItems(portalList, portals); SetWindowTextW(Control(PortalKey), L"");
        if (selectedPage == kPortalPage) Say(L"传送门列表所属会话已变化，请重新刷新");
    }
    for (int id : {Refresh, PortalRefresh, Pick, ActionSend, EndSend, PlainSend, Preview, Check, CaptureStart, CaptureStop})
        EnableIfChanged(Control(id), !busy);
    std::string portalReason;
    EnableIfChanged(Control(PortalEnter), !busy && !portals.empty() && ListView_GetNextItem(portalList, -1, LVNI_SELECTED) >= 0 &&
        (!canSendAction || canSendAction(portalReason)));
    SetTextIfChanged(Control(CaptureStart), localCapture ? L"更新收发记录" : L"开始收发记录");
    const auto stats = L"已接收 " + std::to_wstring(receivedCount) + L" 条；界面丢弃 " +
        std::to_wstring(uiDropped.load()) + L" 条；DLL 丢弃 " + std::to_wstring(serverDropped) + L" 条（最近回执）";
    SetTextIfChanged(Control(7208), stats);
}
void Page() {
    const int selected = selectedPage;
    for (int p = 0; p < kModeCount; ++p) ui::SetButtonSelected(modeTabs[p], p == selected);
    for (int p = 0; p < kModeCount; ++p) for (auto control : pages[p]) ShowWindow(control, p == selected ? SW_SHOW : SW_HIDE);
    ShowWindow(Control(Append), selected == 2 || selected == kPortalPage ? SW_HIDE : SW_SHOW);
}
void Layout() {
    if (layingOut || !window) return;
    layingOut = true;
    RECT rect{}; GetClientRect(window, &rect);
    contentHeight = (std::max)(710, int(rect.bottom));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = contentHeight - 1; scroll.nPage = UINT(rect.bottom); scroll.nPos = scrollOffset;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE); GetScrollInfo(window, SB_VERT, &scroll); scrollOffset = scroll.nPos;
    GetClientRect(window, &rect);
    const int width = rect.right, inner = width - 40;
    const bool wide = width >= 1020;
    Move(Control(7200), 20, 10, inner - 155, 32); Move(Control(Check), width - 160, 10, 140, 32);
    Move(Control(7208), 20, 48, inner, 24); Move(status, 20, 77, inner, 28);
    const int tabWidth = (std::min)(160, (inner - 8 * (kModeCount - 1)) / kModeCount);
    for (int p = 0; p < kModeCount; ++p) Move(modeTabs[p], 20 + p * (tabWidth + 8), 116, tabWidth, 34);
    Move(Control(Refresh), 20, 166, 124, 32); Move(Control(7201), 158, 172, 112, 24);
    Move(Control(Key), 276, 166, inner - 390, 32); Move(Control(Pick), width - 126, 166, 106, 32);
    Move(itemList, 20, 210, inner, 130);
    const int itemWidth = inner - 24;
    const int nameWidth = itemWidth * 32 / 100, keyWidth = itemWidth * 16 / 100, addressWidth = itemWidth * 27 / 100;
    ColumnWidth(itemList, 0, nameWidth); ColumnWidth(itemList, 1, keyWidth);
    ColumnWidth(itemList, 2, addressWidth); ColumnWidth(itemList, 3, itemWidth - nameWidth - keyWidth - addressWidth);
    Move(Control(PortalRefresh), 20, 166, 124, 32); Move(Control(7210), 158, 172, 112, 24);
    Move(Control(PortalKey), 276, 166, inner - 390, 32); Move(Control(PortalEnter), width - 126, 166, 106, 32);
    Move(portalList, 20, 210, inner, 130); Move(Control(7211), 20, 350, inner - 166, 40);
    ColumnWidth(portalList, 0, nameWidth); ColumnWidth(portalList, 1, keyWidth);
    ColumnWidth(portalList, 2, addressWidth); ColumnWidth(portalList, 3, itemWidth - nameWidth - keyWidth - addressWidth);
    const int fieldWidth = (inner - 24) / 4;
    const int labelIds[] = {7202, 7203, 7204, 7205};
    const int editIds[] = {ActionId, X, Y, Flags};
    for (int p = 0; p < 4; ++p) {
        Move(Control(labelIds[p]), 20 + p * (fieldWidth + 8), 166, fieldWidth, 24);
        Move(Control(editIds[p]), 20 + p * (fieldWidth + 8), 194, fieldWidth, 32);
    }
    Move(Control(ActionSend), 20, 240, 160, 34); Move(Control(EndSend), 192, 240, 150, 34);
    Move(Control(7206), 20, 286, inner, 54);
    Move(Control(Plain), 20, 166, inner - 166, 174); Move(Control(PlainSend), width - 170, 166, 150, 34);
    Move(Control(7207), width - 170, 212, 150, 128);
    Move(Control(Append), 20, 352, 280, 30); Move(Control(Preview), width - 170, 350, 150, 34);
    Move(Control(CaptureSend), 20, 404, 112, 32); Move(Control(CaptureRecv), 140, 404, 112, 32);
    Move(Control(CaptureStart), 264, 404, 144, 32); Move(Control(CaptureStop), 420, 404, 112, 32);
    const int toolsX = wide ? 552 : 20, toolsY = wide ? 404 : 448;
    Move(filter, toolsX, toolsY, 126, 180); Move(Control(Clear), toolsX + 138, toolsY, 74, 32);
    Move(Control(Export), toolsX + 224, toolsY, 96, 32); Move(Control(Replay), toolsX + 332, toolsY, 112, 32);
    const int logTop = wide ? 452 : 492;
    const int logHeight = (std::max)(110, contentHeight - logTop - 124);
    Move(logList, 20, logTop, inner, logHeight); Move(details, 20, logTop + logHeight + 12, inner, 92);
    Page(); layingOut = false;
}
void ScrollPage(int position) {
    const auto before = scrollOffset; scrollOffset = position; Layout();
    if (before != scrollOffset) InvalidateRect(window, nullptr, FALSE);
}
void ExportRecords() {
    const auto path = app::Config().RuntimeFileW(L"packet_capture.tsv");
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    output << "\xEF\xBB\xBFtime\tdirection\tdetail\thex\n";
    for (const auto& record : records) {
        auto utf8 = [](const std::wstring& text) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
            std::string value(n, '\0');
            if (n) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), value.data(), n, nullptr, nullptr);
            return value;
        };
        output << utf8(record.time) << '\t' << utf8(record.label) << '\t' << utf8(record.text) << '\t' << record.hex << '\n';
    }
    output.close(); Say(output.fail() ? L"导出失败" : L"记录已导出到 " + path);
}
LRESULT CALLBACK ModeTabProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_GETDLGCODE) return DLGC_BUTTON | DLGC_WANTARROWS;
    if (message == WM_KEYDOWN && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_HOME || wp == VK_END)) {
        const int index = GetDlgCtrlID(hwnd) - PagePick;
        const int next = wp == VK_HOME ? 0 : wp == VK_END ? kModeCount - 1 : (index + (wp == VK_LEFT ? kModeCount - 1 : 1)) % kModeCount;
        selectedPage = next; Page(); SetFocus(modeTabs[next]); return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ModeTabProc, id);
    return DefSubclassProc(hwnd, message, wp, lp);
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0; if (ui::HandleMessage(hwnd, message, wp, lp, themed)) return themed;
    switch (message) {
    case WM_CREATE: {
        window = hwnd; ui::ApplyWindow(hwnd); font = ui::Font(14); titleFont = ui::Font(23, FW_SEMIBOLD);
        mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        auto title = Make(hwnd, L"STATIC", L"封包工具", 0, 7200); ui::StyleLabel(title, titleFont);
        Make(hwnd, L"STATIC", L"", 0, 7208);
        status = Make(hwnd, L"STATIC", L"选择拾取、传送门、动作组包或明文发送；收发记录默认关闭", 0, 7299);
        Make(hwnd, L"BUTTON", L"检查通信与入口", BS_PUSHBUTTON | WS_TABSTOP, Check);
        int mode = 0;
        for (auto text : {L"拾取掉落物", L"动作组包", L"十六进制明文", L"进入传送门"}) {
            modeTabs[mode] = Make(hwnd, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, PagePick + mode, -1, ui::ButtonStyle::Tab);
            SetWindowSubclass(modeTabs[mode], ModeTabProc, 1, 0); ++mode;
        }
        Make(hwnd, L"BUTTON", L"刷新掉落物", BS_PUSHBUTTON | WS_TABSTOP, Refresh, 0);
        Make(hwnd, L"STATIC", L"实体 key（HEX）", 0, 7201, 0);
        Make(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, Key, 0);
        Make(hwnd, L"BUTTON", L"拾取一次", BS_PUSHBUTTON | WS_TABSTOP, Pick, 0);
        itemList = Make(hwnd, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, Items, 0);
        Columns(itemList, {{L"掉落物名称", 360}, {L"实体 key", 140}, {L"实体地址", 210}, {L"世界坐标", 170}});
        Make(hwnd, L"BUTTON", L"刷新传送门", BS_PUSHBUTTON | WS_TABSTOP, PortalRefresh, kPortalPage);
        Make(hwnd, L"STATIC", L"实体 key（HEX）", 0, 7210, kPortalPage);
        Make(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, PortalKey, kPortalPage);
        Make(hwnd, L"BUTTON", L"进入一次", BS_PUSHBUTTON | WS_TABSTOP, PortalEnter, kPortalPage, ui::ButtonStyle::Primary);
        portalList = Make(hwnd, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, Portals, kPortalPage);
        Columns(portalList, {{L"传送门名称", 360}, {L"实体 key", 140}, {L"实体地址", 210}, {L"世界坐标", 170}});
        Make(hwnd, L"STATIC", L"请先停止跟随、寻路和技能操作，再选择附近传送门；进门请求只发送一次。", 0, 7211, kPortalPage);
        const int labels[] = {7202,7203,7204,7205};
        const wchar_t* names[] = {L"动作 ID（HEX）",L"X（十进制）",L"Y（十进制）",L"标志（HEX）"};
        for (int i = 0; i < 4; ++i) Make(hwnd, L"STATIC", names[i], 0, labels[i], 1);
        Make(hwnd, L"EDIT", L"20002909", ES_AUTOHSCROLL | WS_TABSTOP, ActionId, 1);
        Make(hwnd, L"EDIT", L"0", ES_AUTOHSCROLL | WS_TABSTOP, X, 1);
        Make(hwnd, L"EDIT", L"0", ES_AUTOHSCROLL | WS_TABSTOP, Y, 1);
        Make(hwnd, L"EDIT", L"00000400", ES_AUTOHSCROLL | WS_TABSTOP, Flags, 1);
        Make(hwnd, L"BUTTON", L"组包并发送一次", BS_PUSHBUTTON | WS_TABSTOP, ActionSend, 1);
        Make(hwnd, L"BUTTON", L"发送结束动作", BS_PUSHBUTTON | WS_TABSTOP, EndSend, 1);
        Make(hwnd, L"STATIC", L"动作对象由游戏内 DLL 构造，序号取当前角色上下文。\nX / Y 直接填入动作字段；移动动作通常使用目标与当前位置的差值。", 0, 7206, 1);
        Make(hwnd, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, Plain, 2);
        SendMessageW(Control(Plain), WM_SETFONT, WPARAM(mono), FALSE);
        SendMessageW(Control(Plain), EM_SETLIMITTEXT, packet::kMaxBytes * 3 + 100, 0);
        Make(hwnd, L"BUTTON", L"发送明文一次", BS_PUSHBUTTON | WS_TABSTOP, PlainSend, 2);
        Make(hwnd, L"STATIC", L"填写完整十六进制字节。\n经游戏缓冲与发送函数处理。\n单次最多 1536 字节。", 0, 7207, 2);
        Make(hwnd, L"BUTTON", L"发送后追加结束动作", BS_AUTOCHECKBOX | WS_TABSTOP, Append);
        Make(hwnd, L"BUTTON", L"组包预览", BS_PUSHBUTTON | WS_TABSTOP, Preview);
        Make(hwnd, L"BUTTON", L"发送明文", BS_AUTOCHECKBOX | WS_TABSTOP, CaptureSend);
        Make(hwnd, L"BUTTON", L"接收片段", BS_AUTOCHECKBOX | WS_TABSTOP, CaptureRecv);
        SendMessageW(Control(CaptureSend), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(Control(CaptureRecv), BM_SETCHECK, BST_CHECKED, 0);
        Make(hwnd, L"BUTTON", L"开始收发记录", BS_PUSHBUTTON | WS_TABSTOP, CaptureStart);
        Make(hwnd, L"BUTTON", L"停止记录", BS_PUSHBUTTON | WS_TABSTOP, CaptureStop);
        filter = Make(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Filter);
        for (auto text : {L"全部记录", L"仅发送明文", L"仅接收片段"}) SendMessageW(filter, CB_ADDSTRING, 0, LPARAM(text));
        SendMessageW(filter, CB_SETCURSEL, 0, 0);
        Make(hwnd, L"BUTTON", L"清空", BS_PUSHBUTTON | WS_TABSTOP, Clear);
        Make(hwnd, L"BUTTON", L"导出记录", BS_PUSHBUTTON | WS_TABSTOP, Export);
        Make(hwnd, L"BUTTON", L"填入明文", BS_PUSHBUTTON | WS_TABSTOP, Replay);
        logList = Make(hwnd, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, Records);
        Columns(logList, {{L"接收时间",120},{L"方向 / 类型",115},{L"状态 / 长度",380},{L"十六进制预览",460}});
        details = Make(hwnd, L"EDIT", L"选中记录查看完整内容。切换页面后，已开启的收发记录继续生效。", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 7298);
        SendMessageW(details, WM_SETFONT, WPARAM(mono), FALSE);
        SetTimer(hwnd, 1, 150, nullptr); Layout(); return 0;
    }
    case WM_SIZE: if (modeTabs[0]) Layout(); return 0;
    case WM_VSCROLL: {
        SCROLLINFO scroll{sizeof(scroll), SIF_ALL}; GetScrollInfo(hwnd, SB_VERT, &scroll);
        int position = scroll.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: position -= 32; break;
        case SB_LINEDOWN: position += 32; break;
        case SB_PAGEUP: position -= int(scroll.nPage); break;
        case SB_PAGEDOWN: position += int(scroll.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = scroll.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = scroll.nMax; break;
        }
        ScrollPage(position); return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollPage(scrollOffset - GET_WHEEL_DELTA_WPARAM(wp) * 48 / WHEEL_DELTA); return 0;
    case WM_TIMER: Consume(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; auto dc = BeginPaint(hwnd, &paint); RECT rect{}; GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, ui::Background()); EndPaint(hwnd, &paint); return 0;
    }
    case WM_NOTIFY: {
        if (populating) return 0;
        auto header = reinterpret_cast<NMHDR*>(lp);
        if (header->idFrom == Items && header->code == LVN_ITEMCHANGED) {
            const int row = ListView_GetNextItem(itemList, -1, LVNI_SELECTED);
            if (row >= 0 && size_t(row) < items.size()) {
                wchar_t key[24]; swprintf_s(key, L"%08X", items[size_t(row)].key); SetWindowTextW(Control(Key), key);
            }
        }
        if (header->idFrom == Portals && header->code == LVN_ITEMCHANGED) {
            const int row = ListView_GetNextItem(portalList, -1, LVNI_SELECTED);
            if (row >= 0 && size_t(row) < portals.size()) {
                wchar_t key[24]; swprintf_s(key, L"%08X", portals[size_t(row)].key); SetWindowTextW(Control(PortalKey), key);
            } else SetWindowTextW(Control(PortalKey), L"");
        }
        if (header->idFrom == Records && header->code == LVN_ITEMCHANGED) {
            const int row = ListView_GetNextItem(logList, -1, LVNI_SELECTED);
            if (row >= 0 && size_t(row) < visible.size()) {
                const auto& record = records[visible[size_t(row)]];
                SetWindowTextW(details, (record.label + L"  " + record.text + L"\r\n" + Wide(record.hex)).c_str());
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= PagePick && id <= PagePortal && HIWORD(wp) == BN_CLICKED) {
            selectedPage = id - PagePick; Page(); return 0;
        }
        if (id == IDCANCEL) { SetFocus(GetParent(hwnd)); return 0; }
        if (id == Filter && HIWORD(wp) == CBN_SELCHANGE) { PopulateRecords(); return 0; }
        if (id == Clear) { records.clear(); PopulateRecords(); SetWindowTextW(details, L""); return 0; }
        if (id == Export) { ExportRecords(); return 0; }
        if (id == Replay) {
            const int row = ListView_GetNextItem(logList, -1, LVNI_SELECTED);
            if (row >= 0 && size_t(row) < visible.size()) {
                const auto& record = records[visible[size_t(row)]];
                if (record.direction != packet::Send) { Say(L"请选择发送明文记录；接收片段和对象预览不能直接重发"); return 0; }
                if (record.truncated) { Say(L"该发送记录已截断，不能填入明文重发"); return 0; }
                SetWindowTextW(Control(Plain), Wide(record.hex).c_str()); selectedPage = 2; Page(); ScrollPage(0); SetFocus(Control(Plain));
            }
            return 0;
        }
        if (busy) return 0;
        if (id == Refresh) { RefreshItems(); return 0; }
        if (id == PortalRefresh) { RefreshItems(true); return 0; }
        packet::Request request;
        if (id == Check) request.operation = packet::Operation::Status;
        else if (id == CaptureStart || id == CaptureStop) {
            request.operation = packet::Operation::Capture;
            if (id == CaptureStart) request.captureMask = (Checked(CaptureSend) ? packet::Send : 0) | (Checked(CaptureRecv) ? packet::Receive : 0);
            if (id == CaptureStart && !request.captureMask) { Say(L"请至少勾选一个记录方向"); return 0; }
        } else if (id == EndSend) request.operation = packet::Operation::EndAction;
        else if (id == Pick || id == PortalEnter || id == ActionSend || id == PlainSend || id == Preview) {
            if (!RequestFromPage(request)) return 0;
            if (id == Preview) request.options |= packet::PreviewOnly;
        } else return 0;
        if (packet::Sends(request.operation) && !(request.options & packet::PreviewOnly)) {
            std::string reason;
            if (canSendAction && !canSendAction(reason)) { Say(Wide(reason)); return 0; }
        }
        const auto requestSession = request.operation == packet::Operation::EnterPortal ? portalSession : 0;
        const auto requestController = request.operation == packet::Operation::EnterPortal ? portalController : 0;
        Start([request, requestSession, requestController] { return Call(request, requestSession, requestController); }); return 0;
    }
    case WM_CLOSE: return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1); window = nullptr; status = nullptr; details = nullptr;
        for (auto& tab : modeTabs) tab = nullptr;
        for (auto& page : pages) page.clear();
        if (font) DeleteObject(font); if (mono) DeleteObject(mono); if (titleFont) DeleteObject(titleFont);
        font = mono = titleFont = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace
bool Busy() { return busy.load(); }
void CreatePanel(HWND parent, std::function<bool(std::string&)> actionCheck) {
    if (window && IsWindow(window)) return;
    canSendAction = std::move(actionCheck);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = ui::Background(); cls.lpszClassName = L"Poe2PacketPage";
    cls.hIcon = ui::AppIcon(); cls.hIconSm = ui::AppIcon(true); RegisterClassExW(&cls);
    window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"封包工具",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL,
        0, 0, 600, 710, parent, HMENU(770), cls.hInstance, nullptr);
}
void ShowPanel(bool visible, const RECT& bounds) {
    if (!window) return;
    if (!visible && IsChild(window, GetFocus())) SetFocus(GetParent(window));
    SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
}
void Shutdown() {
    stopping = true;
    if (worker.joinable()) worker.join();
    if (observer && app::Config().TargetAlive()) { packet::Request stop; stop.operation = packet::Operation::Capture; Call(stop); }
    observer.reset();
    if (window && IsWindow(window)) DestroyWindow(window);
}
} // namespace packetui
