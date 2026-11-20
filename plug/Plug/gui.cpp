// ============================================================
// gui.cpp —— 自动寻路图形界面（Win32 + GDI，无第三方依赖）
//
// 视觉风格：暗色主题（POE 那种深邃感）
//   · 整窗深灰蓝底 + 细描边卡片
//   · 自绘圆角按钮（悬停/按下/禁用三态）
//   · 地图区带发光边框、网格坐标刻度
//   · 角色用呼吸光环、目标用脉冲圆环、路径用渐变蓝
//   · 底部状态灯 + 进度条
//
// 线程模型（踩过坑，必须这样）：
//   · 窗口与绘制全在 UI 线程
//   · 收图 / 算路 / 行走全在后台线程，进度写进 g_state
//   · UI 用 50ms 定时器轮询 g_state 重绘，绝不跨线程操作窗口
//
// 编码（也踩过）：
//   工程用 /utf-8 编译，字面量是 UTF-8 字节。
//   Win32 的 ...W 版本 API 要宽字符，所以统一走 Utf8ToWide。
//
// 布局（也踩过）：
//   旧版写死地图控件 900x900 但窗口只有 660 高，底部被裁掉。
//   现在全部运行期算，WM_SIZE 重排，最小 620x680。
// ============================================================

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <tlhelp32.h>   // 注入：Toolhelp32 枚举模块，判断 Monitor.dll 是否已加载
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>   // atoi（周围列表 pos 反解析）
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <sstream>
#include <stdexcept>

#include "AppConfig.h"
#include "GameOffsets.h"
#include "Navigator.h"
#include "Logger.h"
#include "MapSink.h"
#include "UseCases.h"
#include "Inventory.h"
#include "Surroundings.h"
#include "NamedPipeClie.h"
#include "MonitorModule.h"
#include "ComboPanel.h"
#include "PacketPanel.h"
#include "RecoveryPanel.h"
#include "ViewPanel.h"
#include "RitualPanel.h"
#include "RitualAutomation.h"
#include "ComboConfig.h"
#include "ComboRunner.h"
#include "UiTheme.h"
#include "FollowPanel.h"
#include "FollowRunner.h"
#include "SessionLauncher.h"
#include "WorldArea.h"
#include "MyOLAFunction.h"
#include "PanelSession.h"
#include "../Common/InstanceMutex.h"
#include "../Common/LocateWindow.h"
#include "../Common/Protocal.h"

#include "../Common/MapFrame.h"
#include "../Common/PixelNavigation.h"
#include "../Common/Protocal.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

sessions::PanelSession g_panelSession;

// ============================================================
// 配色（暗色主题）
// ============================================================
namespace col {
using namespace ui::color;

// 地图地形
constexpr COLORREF kMapWall     = RGB(10, 11, 15);      // 墙：近黑
constexpr COLORREF kMapFloor    = RGB(196, 214, 226);   // 地板：冷白
constexpr COLORREF kMapSlow     = RGB(150, 160, 176);   // 减速带

// 玩家 / 目标 / 路径
constexpr COLORREF kPlayer      = RGB(88, 232, 152);
constexpr COLORREF kPlayerCore  = RGB(228, 255, 240);
constexpr COLORREF kGoal        = RGB(255, 108, 108);
constexpr COLORREF kPath        = RGB(96, 184, 255);
constexpr COLORREF kPathGlow    = RGB(40, 92, 148);
}

// ============================================================
// 布局：左上小地图（全局概览） + 左下放大视图（局部细节）
//
// 为什么这么分：
//   · 小地图看全局——目标在哪、路径怎么绕、角色在图上什么位置
//   · 放大视图看细节——墙缝、减速带、路径是否贴墙
//   两个视图共享同一个世界坐标系，只是缩放比不同；
//   点小地图 = 选全局目标，点放大图 = 在已选区域里精调。
//
// 联动规则：
//   放大视图的「视野中心」跟着角色走（角色一直在中间），
//   缩放比固定为 MINI 的 kZoomRatio 倍，滚轮可调。
// ============================================================
constexpr int kTabH = 42;   // 顶部页签条高度

// 地图控件的"期望边长"。ComputeLayout 每次都从这个常量重新规划，
// 保证窗口变大时地图能跟着长回来（不能用 g_miniSize 自己当起点，
// 那样只会缩不会涨，换图后会留下一个很小的地图）。
constexpr int kMapWanted = 340;
constexpr int kMapActionH = 36;
constexpr int kMapActionGap = 8;
constexpr int kMapHeaderH = 56;
constexpr int kMapHeaderGap = 8;

int g_pad      = 14;
// ★ 面板宽度是固定值：它参与右侧地图区可用宽度的计算，如果跟着地图
//   宽度变，两者就互相依赖成循环，布局会失控（地图盖住按钮）。
constexpr int kPanelW  = 612;
int g_panelW   = kPanelW;
int g_miniSize = kMapWanted;   // 右上地图控件边长（正方形）

// 地图的世界像素尺寸。
uint32_t g_mapW = 0;
uint32_t g_mapH = 0;

// ★ 地图里"可走区域"的包围盒（世界像素）。
//
//   世界尺寸是游戏给的整张图（可能 4002x2001），但可走部分常常只占其中
//   一小块。如果按整图缩放显示，道路会被缩成一团看不清。
//   所以显示、点击换算都只针对这块区域，让它铺满控件。
//   BuildMapGray 里算出，MapDrawRect / WorldToMini / MiniToWorld 共用。
int g_mapInX = 0;
int g_mapInY = 0;
int g_mapInW = 0;
int g_mapInH = 0;

// 记录控件位置
int g_miniX = 0, g_miniY = 0;

// ★ 需要重新布局（地图尺寸变化后置位，由 UI 定时器消费）。
//   地图是在后台线程拉回来的，不能在后台线程直接 SetWindowPos ——
//   Win32 窗口操作必须在创建它的线程里做，否则会出现无声的失败。
std::atomic_bool g_layoutDirty{ false };

void ComputeLayout(int clientW, int clientH)
{
    // 顶部让出页签条的高度
    clientH -= kTabH;
    // 面板在左、地图在右。
    //
    // ★ g_miniSize 是"期望尺寸"（340），g_mapView 是"实际尺寸"。
    //   窗口够大就用期望值，不够才缩。
    //
    // ★ g_miniX/g_miniY 是【主窗口坐标系】里的位置（用来摆控件），
    //   而 PaintMap 画在地图控件内部，坐标系永远从 (0,0) 开始。
    //   两者别搞混 —— 之前就是把这个坐标传进了 PaintMap 的 box，
    //   内容画到了控件外面，整个地图"消失"。
    // ★ 右侧地图区的可用宽度必须用"固定面板宽度"来算。
    //
    //   面板宽度和地图宽度是互相依赖的：面板宽 = 客户区 - 地图宽 - 边距，
    //   而 availW 又 = 客户区 - 面板宽 - 边距。如果面板宽随地图宽变，
    //   就成了循环依赖：地图变宽 -> 面板变窄 -> availW 变大 -> 地图更宽，
    //   最终地图控件把按钮都盖住（实测发生过）。
    //   所以面板宽度固定为 kPanelW，右侧可用区随之固定，长方形形态
    //   的上限自然受控。
    int availW = clientW - g_pad * 3 - kPanelW;
    int availH = clientH - g_pad * 2 - 34 - kMapActionGap - kMapActionH - kMapHeaderH - kMapHeaderGap - 166;

    // 正方形形态的基准边长、面板宽度，都以 kPanelW 为准。
    g_panelW = kPanelW;

    // ★ 必须从"期望尺寸常量"开始算，不能从 g_miniSize 自己开始。
    //
    //   原来写的是 `int side = g_miniSize;` 然后又 `g_miniSize = side;`
    //   —— 拿上次的结果当这次的下限起点，是个只缩不涨的正反馈：
    //   只要窗口曾经变小过（或切到某张图时 availH 变小），
    //   g_miniSize 就被压小并永久留在小值，之后窗口拉回来也长不回去。
    //   表现就是"换了张地图之后，刷新出来的地图很小、和背景对不上"。
    //   现在每次都从 kMapWanted 重新规划，窗口变小才缩、变大能恢复。
    int side = kMapWanted;
    if (side > availW) side = availW;
    if (side > availH) side = availH;
    if (side < 180) side = 180;

    g_miniSize = side;

    g_miniX = clientW - g_pad - g_miniSize;   // 主窗口坐标（控件摆放）
    g_miniY = g_pad + kTabH + kMapHeaderH + kMapHeaderGap;
}

// 地图控件内部坐标（PaintMap 用）：永远从 (0,0) 到 (g_miniSize,g_miniSize)
void GetMapLocalRect(RECT& rc)
{
    rc.left = 0;
    rc.top = 0;
    rc.right = g_miniSize;
    rc.bottom = g_miniSize;
}



// ============================================================
// 全局状态
// ============================================================
std::mutex g_mtx;

mapdata::CostBuffer  g_cost;
poe::WorldAreaSnapshot g_mapArea;
nav::PixelMap         g_grid;
std::vector<uint8_t> g_mapGray;
// g_mapW / g_mapH 声明在文件上方（ComputeLayout 之前），这里不再重复定义。

// 玩家当前世界坐标（鼠标点击选点、算路时都要读）。
// ★ 必须是原子：写它的是主循环，读它的是 DoPlanPath 那种 detach
//   出去的算路线程。普通 int32_t 跨线程读写是数据竞争（UB），
//   虽然 x86-64 上不会撕裂，但没有内存序保证，编译器可自由重排。
std::atomic<int32_t> g_playerX{ 0 }, g_playerY{ 0 };
bool     g_havePlayer = false;

std::vector<NavBatchChunk> g_path;
int32_t  g_goalX = 0, g_goalY = 0;
bool     g_hasGoal = false;

std::string g_status = "就绪";
std::string g_detail;
bool        g_busy = false;
std::atomic_bool g_walkCallBusy{false};
std::thread g_walkCallThread;
int         g_state = 0;      // 0=空闲 1=忙 2=成功 3=失败

// ------------------------------------------------------------
// 背包 / 周围 数据（GUI 新增）
// ------------------------------------------------------------
std::vector<poe::InventoryItem>  g_inv;      // 背包物品
std::vector<poe::EntitySnapshot> g_around;   // 周围实体
bool   g_haveInv = false;
bool   g_haveAround = false;
std::string g_scanErr;

// 列表滚动位置（按行数计）
int g_invScroll = 0;
int g_aroundScroll = 0;

poe::SkillSlotsSnapshot g_skills;
bool g_haveSkills = false;
std::string g_skillErr;
std::wstring g_skillUpdated;
int g_skillScroll = 0;
std::atomic_bool g_skillBusy{false};
std::atomic_bool g_skillDirty{false};
std::thread g_skillThread; // Owned by UI thread; joined before global state is destroyed.

// 当前页签：0=寻路 1=背包 2=周围 3=技能槽 4=跟随 5=封包工具 6=祭祀
int g_tab = 0;

// 页签区域（主窗口坐标）
constexpr int kPageCount = 7;
RECT g_tabRc[kPageCount] = {};
// UI-owned copies of exactly the cells last painted, so a background refresh
// cannot change the value associated with a row before it is redrawn.
struct CopyCell { RECT bounds; std::string text; int page; };
std::vector<CopyCell> g_copyCells;
std::wstring g_copyFeedback;
uint64_t g_copyFeedbackUntil = 0;

const CopyCell* HitCopyCell(POINT point)
{
    for (const auto& cell : g_copyCells)
        if (cell.page == g_tab && PtInRect(&cell.bounds, point)) return &cell;
    return nullptr;
}

std::vector<std::string> g_logs;
constexpr size_t kMaxLogs = 300;

enum class NavigationTest { None, NavigationOnly, NavigationAndCombo, Ritual };
std::atomic<NavigationTest> g_navigationTest{NavigationTest::None};
std::atomic_bool g_navigationFromRitual{false};
std::atomic_bool g_navigationComboStarted{false};
std::thread g_navigationThread; // UI-owned; joined before restart and shutdown.
std::atomic_bool g_stopNav{ false };
std::atomic<usecase::NavStopReason> g_navStopReason{usecase::NavStopReason::None};
std::atomic_bool g_running{ true };
std::thread g_pipeStartupThread;
uint64_t g_mapSessionId = 0;

// 面板区块 Y 坐标（WM_SIZE 里记账，PaintChrome 里读）
int Y_BUTTON_END = 0;
int Y_LOG_TOP    = 0;
int Y_RECOVERY_END = 0;

// 控件
// ★ 这两个窗口句柄会被后台 worker 线程读（InvalidateRect 重绘），
//   同时在 WM_DESTROY 里被主线程置空。普通 HWND 变量跨线程读写
//   属数据竞争；用原子保证 worker 一定能看到"窗口已经没了"。
//
//   访问一律走下面两个 helper，别直接读原子变量：
//   std::atomic<HWND> 不能隐式转换成 HWND 传给 Win32 API。
std::atomic<HWND> g_hMainStore{ nullptr };
std::atomic<HWND> g_hMapStore{ nullptr };

inline HWND MainHwnd() { return g_hMainStore.load(); }
inline HWND MapHwnd()  { return g_hMapStore.load(); }
WNDPROC g_oldMapProc = nullptr;

// 自绘按钮
struct Button {
    HWND  hwnd = nullptr;
    int   id = 0;
    int   kind = 0;        // 0=主按钮 1=次按钮 2=危险
    bool  hot = false;
    bool  down = false;
    wchar_t text[64] = { 0 };
};
Button g_btnRefresh;
Button g_btnGo;
Button g_btnStop;
Button g_btnScan;      // 背包/周围页的「扫描」按钮
Button g_btnSkills;    // 只读技能槽获取/刷新
Button g_btnNavComboStart;
Button g_btnNavComboStop;
Button g_btnNavSettings; // 寻路参数（自动保存）
Button g_btnCombo;

// ---- 裸走路 call 标定面板 ----
// 常驻显示在寻路页：两个输入框（目标 x / y）+ 一个「调一次」按钮。
// 语义：真实调一次走路 call，把调用前后角色坐标和到目标的距离打出来。
// 不做去重、不做冷却、不做路径状态机 —— 这是标定工具，行为要尽量"裸"。
HWND  g_editWalkX   = nullptr;   // 目标 x
HWND  g_editWalkY   = nullptr;   // 目标 y
Button g_btnWalkCall;            // 「走一次」按钮
Button g_btnWalkUsePlayer;       // 「取当前角色坐标」按钮
int   Y_WALKCALL_TOP = 0;        // 标定区顶部（PaintChrome 读）
int   Y_WALKCALL_END = 0;
// 最近一次结果（给界面显示 + 日志对照用）
int32_t g_wcPlayerX = 0, g_wcPlayerY = 0;
int32_t g_wcDistBefore = -1, g_wcDistAfter = -1;
int32_t g_wcTargetX = 0, g_wcTargetY = 0;
uint32_t g_wcElapsed = 0;
int32_t g_wcResult = -1;
bool     g_wcHaveResult = false;

HWND g_navSettingsWnd = nullptr;
HWND g_navSettingEdits[14] = {};
bool g_navSettingsReady = false;

// ---- 注入 Monitor.dll ----
// 「注入 DLL」按钮直接在本进程里做远程线程注入，不走外部 inject_monitor.exe：
//   1) 少一个可执行文件依赖（用户拷 NavGUI 就能用）
//   2) 能拿到更细的错误码，界面上直接显示出错在哪一步
// 注入是一次性动作，按钮做成可重复点（已注入时先 FreeLibrary 再重新 Load，
// 方便改完 Monitor.dll 直接热更新，不用重启游戏）。
Button g_btnInject;                  // 「注入 DLL」按钮
int    Y_INJECT_TOP = 0;             // 注入区顶部（MoveButtons 写，PaintChrome 读）
int    Y_INJECT_END = 0;

// ★ 注入相关的共享状态必须全部是原子的：
//   DoInjectDll 跑在 detach 出去的线程里，UI 线程的 WM_PAINT / WM_TIMER
//   同时在读它们。原来 g_injectState 是 std::wstring —— 一个线程在 finish()
//   里重新赋值（释放旧缓冲区）、另一个线程在 PaintChrome 里 .c_str()，
//   这是典型的 use-after-free，注入成功那一刻界面可能直接崩。
//   改成原子整数状态码，文字在绘制时按状态码翻译，谁都不用持有字符串。
std::atomic_bool g_injectBusy{ false };   // 注入进行中（防重复点）
std::atomic_bool g_monitorIncompatible{ false };

// 注入状态码（0=未注入 1=已注入 2=注入失败 3=未运行游戏 4=已加载但管道未就绪 5=未检测）
enum : int {
    kInjectUnknown = 5,
    kInjectNone    = 0,
    kInjectOk      = 1,
    kInjectFail    = 2,
    kInjectNoGame  = 3,
    kInjectNoPipe  = 4,
    kInjectBusy    = 6,
    kInjectWait    = 7,
    kInjectIncompatible = 8,
    kInjectVersionUnknown = 9,
};
std::atomic<int> g_injectCode{ kInjectUnknown };

// 状态码 → 界面文字。只返回常量指针，不分配内存，跨线程读取安全。
static const wchar_t* InjectStateText(int code)
{
    switch (code) {
    case kInjectOk:     return L"已注入";
    case kInjectFail:   return L"注入失败";
    case kInjectNoGame: return L"未运行游戏";
    case kInjectNoPipe: return L"已加载，管道未就绪";
    case kInjectBusy:   return L"已注入，但控制端被占用";
    case kInjectWait:   return L"已加载，等待控制连接";
    case kInjectIncompatible: return L"DLL 接口不兼容，需更新";
    case kInjectVersionUnknown: return L"无法验证 DLL 接口";
    case kInjectNone:   return L"未注入";
    default:            return L"未检测";
    }
}

// 位图缓存
HBITMAP g_bmp = nullptr;
HDC     g_memDC = nullptr;
uint32_t g_bmpW = 0, g_bmpH = 0;

// 动画时钟（毫秒，用于呼吸灯/脉冲）
ULONGLONG g_animT0 = 0;

// 字体
HFONT g_fontUI   = nullptr;
HFONT g_fontBold = nullptr;
HFONT g_fontMono = nullptr;
HFONT g_fontBig  = nullptr;

// ============================================================
// 工具
// ============================================================
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         s.c_str(), (int)s.size(), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        s.c_str(), (int)s.size(), &w[0], need);
    return w;
}

void AddLog(const std::string& s)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    sprintf_s(ts, sizeof(ts), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_logs.push_back(std::string(ts) + "  " + s);
        if (g_logs.size() > kMaxLogs) {
            g_logs.erase(g_logs.begin(), g_logs.begin() + (long)(g_logs.size() - kMaxLogs));
        }
    }
    if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
}

void SetStatus(const std::string& s, const std::string& detail, int state)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_status = s;
        g_detail = detail;
        g_state = state;
    }
    if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
}

// ============================================================
// 绘图小工具：圆角矩形 / 填充矩形 / 文本
// ============================================================
// 圆角矩形：直接用 GDI 的 RoundRect。
// 之前手拼 Arc 路径在部分 DPI 下会画出直角，RoundRect 更稳。
void FillRoundRect(HDC hdc, RECT rc, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void FillRoundRectSolid(HDC hdc, RECT rc, int radius, COLORREF fill)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

COLORREF LerpColor(COLORREF a, COLORREF b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    const int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    const int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, bl);
}

void DrawTextIn(HDC hdc, const wchar_t* s, RECT rc, COLORREF c, HFONT f,
                UINT fmt = DT_LEFT | DT_TOP | DT_SINGLELINE)
{
    HGDIOBJ old = SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c);
    DrawTextW(hdc, s, -1, &rc, fmt);
    SelectObject(hdc, old);
}

// 小圆点（用于状态灯）
void Dot(HDC hdc, int cx, int cy, int r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN   pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// 空心圆环
void Ring(HDC hdc, int cx, int cy, int r, COLORREF c, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

// ============================================================
// 灰度底图
// ============================================================
void BuildMapGray()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_cost.Empty()) return;

    const auto& geo = g_cost.Geo();
    g_mapW = geo.WorldW();
    g_mapH = geo.WorldH();
    g_mapGray.assign((size_t)g_mapW * g_mapH, 0);

    // ★ 同时算出"可走区域"的实际包围盒。
    //
    //   地图的世界尺寸是游戏给的整张图（可能 4002x2001），但真正能走的
    //   部分常常只占其中一小块。按整图缩放时可走区就被缩得很小、道路
    //   根本看不清（实测 4002x2001 只剩中间一小团）。
    //   记住包围盒，绘制时只显示这块，道路就能铺满控件。
    int minX = (int)g_mapW, minY = (int)g_mapH, maxX = -1, maxY = -1;

    for (uint32_t y = 0; y < g_mapH; ++y) {
        for (uint32_t x = 0; x < g_mapW; ++x) {
            const uint8_t c = g_cost.At(x, y);
            uint8_t v;
            if (c == 0)                 v = 6;       // 墙：极暗
            else if (c == kCostSpecial) v = 236;     // 地板：亮
            else                        v = (uint8_t)(120 + c * 24);   // 减速带
            g_mapGray[(size_t)y * g_mapW + x] = v;

            if (c != 0) {
                if ((int)x < minX) minX = (int)x;
                if ((int)y < minY) minY = (int)y;
                if ((int)x > maxX) maxX = (int)x;
                if ((int)y > maxY) maxY = (int)y;
            }
        }
    }

    if (maxX >= minX && maxY >= minY) {
        // 留一点边，免得可走区贴死在控件边缘
        const int padPx = 12;
        minX -= padPx; minY -= padPx;
        maxX += padPx; maxY += padPx;
        if (minX < 0) minX = 0;
        if (minY < 0) minY = 0;
        if (maxX >= (int)g_mapW) maxX = (int)g_mapW - 1;
        if (maxY >= (int)g_mapH) maxY = (int)g_mapH - 1;

        g_mapInX = minX;
        g_mapInY = minY;
        g_mapInW = maxX - minX + 1;
        g_mapInH = maxY - minY + 1;
    }
    else {
        // 全图没有可走点：退回整图
        g_mapInX = 0;
        g_mapInY = 0;
        g_mapInW = (int)g_mapW;
        g_mapInH = (int)g_mapH;
    }

    LOGI("[GUI] 地图 %ux%u，可走区域 %d,%d %dx%d（按此区域铺满显示）",
         g_mapW, g_mapH, g_mapInX, g_mapInY, g_mapInW, g_mapInH);

    // ★ 地图尺寸变了，控件形状必须跟着重算。
    //   ComputeLayout 依赖 g_mapW/g_mapH，而窗口创建时地图还没加载
    //   （g_mapW==0）算出来是正方形；换图后不重新布局，控件就一直是
    //   旧尺寸，地图和背景对不上。
    g_layoutDirty = true;
}

// ============================================================
// 坐标换算
// ============================================================
// ★ 地图在正方形框里的"实际绘制矩形"。
//
//   小地图控件是正方形，但地图本身不一定是正方形（实测遇到过
//   4002 x 2001 这种长条图）。原来 StretchBlt 把宽高都撑成 S×S，
//   长条图就被横向压扁、两侧留大片黑边 —— 看起来就是"地图很小、
//   和背景不匹配"。
//
//   现在按比例适配并居中：长边铺满，短边居中留边。
//   这个矩形是绘图层和坐标换算层的唯一事实来源 —— 两边必须用同一个，
//   否则底图和路径/角色标记会错位。
void MapDrawRect(RECT& out)
{
    // 控件是正方形（g_miniSize x g_miniSize）。显示的不是整张世界图，
    // 而是**可走区域包围盒**（g_mapInX/Y/W/H）按比例缩进控件并居中。
    //
    //   为什么不用整图：世界尺寸可能 4002x2001，但可走部分只占很小一块，
    //   按整图缩放后道路缩成一团、根本看不清。只显示可走区域，道路就
    //   能铺满控件（和 1656x1656 那种方图的效果一致）。
    out.left = 0;
    out.top = 0;
    out.right = g_miniSize;
    out.bottom = g_miniSize;

    const int inW = (g_mapInW > 0) ? g_mapInW : (int)g_mapW;
    const int inH = (g_mapInH > 0) ? g_mapInH : (int)g_mapH;
    if (inW <= 0 || inH <= 0) return;

    const double sx = (double)g_miniSize / (double)inW;
    const double sy = (double)g_miniSize / (double)inH;
    const double s = (sx < sy) ? sx : sy;

    const int w = (int)((double)inW * s + 0.5);
    const int h = (int)((double)inH * s + 0.5);
    if (w >= g_miniSize && h >= g_miniSize) return;   // 已经铺满，直接用整个控件

    const int ox = (g_miniSize - w) / 2;
    const int oy = (g_miniSize - h) / 2;
    out.left = ox;
    out.top = oy;
    out.right = ox + w;
    out.bottom = oy + h;
}

// 世界 -> 小地图像素
POINT WorldToMini(int32_t wx, int32_t wy)
{
    POINT p{ -1, -1 };
    if (g_mapW == 0 || g_mapH == 0) return p;
    RECT r;
    MapDrawRect(r);

    const int inW = (g_mapInW > 0) ? g_mapInW : (int)g_mapW;
    const int inH = (g_mapInH > 0) ? g_mapInH : (int)g_mapH;
    const double w = (double)(r.right - r.left);
    const double h = (double)(r.bottom - r.top);

    // 世界坐标先减去可走区原点，再按可走区尺寸缩放
    p.x = (LONG)(r.left + (double)(wx - g_mapInX) * w / (double)inW);
    p.y = (LONG)(r.top + (double)(wy - g_mapInY) * h / (double)inH);
    return p;
}

void MiniToWorld(int vx, int vy, int32_t& wx, int32_t& wy)
{
    if (g_mapW == 0 || g_mapH == 0) { wx = 0; wy = 0; return; }
    RECT r;
    MapDrawRect(r);
    const int inW = (g_mapInW > 0) ? g_mapInW : (int)g_mapW;
    const int inH = (g_mapInH > 0) ? g_mapInH : (int)g_mapH;
    const double w = (double)(r.right - r.left);
    const double h = (double)(r.bottom - r.top);
    if (w <= 0 || h <= 0) { wx = 0; wy = 0; return; }

    // 与 WorldToMini 严格互逆：先按可走区尺寸换算，再加回原点
    wx = g_mapInX + (int32_t)((double)(vx - r.left) * (double)inW / w);
    wy = g_mapInY + (int32_t)((double)(vy - r.top) * (double)inH / h);
}

// ============================================================
// 把 g_mapGray 转成 DIB 纹理（两个视图共用同一张）
//
// ★ 这段曾经在重写 PaintMap 时被误删，结果地图底图整个消失、
//   只剩灰底上的路径线。任何改动这块渲染代码都要确认它还在。
// ============================================================
bool EnsureMapBitmap(HDC hdc)
{
    if (g_mapGray.empty() || g_mapW == 0) return false;

    const bool needRebuild = !g_bmp || g_bmpW != g_mapW || g_bmpH != g_mapH;
    if (!needRebuild && g_bmp) return true;

    if (!g_memDC) g_memDC = CreateCompatibleDC(hdc);
    if (!g_memDC) return false;

    if (g_bmp) { DeleteObject(g_bmp); g_bmp = nullptr; }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)g_mapW;
    bi.bmiHeader.biHeight = -(LONG)g_mapH;    // 负数 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g_bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_bmp) return false;

    g_bmpW = g_mapW;
    g_bmpH = g_mapH;
    SelectObject(g_memDC, g_bmp);

    if (bits) {
        uint32_t* p = (uint32_t*)bits;
        for (size_t i = 0; i < g_mapGray.size(); ++i) {
            const uint8_t v = g_mapGray[i];
            const uint8_t r = (uint8_t)(v * 0.86f);
            const uint8_t g2 = (uint8_t)(v * 0.93f);
            const uint8_t b = (uint8_t)v;
            p[i] = ((uint32_t)r << 16) | ((uint32_t)g2 << 8) | (uint32_t)b;
        }
    }
    return true;
}

// ---- 在给定矩形里画一块地图（两个视图共用）----
//
// 参数：
//   hdc      目标
//   box      绘制区域（正方形）
//   toView   世界坐标 -> 该区域内像素 的转换函数
//   mini     true = 小地图（元素画小一点、栅格淡一点）
void DrawMapInto(HDC hdc, const RECT& box,
                 POINT (*toView)(int32_t, int32_t),
                 bool mini, float pulse)
{
    const int S = box.right - box.left;

    // 底图（首次绘制时构建 DIB 纹理），按地图自身宽高比缩进 box 并居中。
    // ★ 不能把宽高都写成 S：长条图（例如 4002x2001）会被横向压扁、
    //   两侧留大片黑边，看起来就是"地图很小、和背景不匹配"。
    EnsureMapBitmap(hdc);
    if (g_bmp) {
        RECT mr;
        MapDrawRect(mr);

        // ★ MapDrawRect 返回的就是【控件坐标系】（0..g_miniSize / 0..g_miniSize），
        //   而 box 也是同一个坐标系（0,0,g_miniSize,g_miniSize）——
        //   两者直接对应，不需要任何换算。
        //
        //   这里曾写过 k = S / g_miniSize 的缩放，那是控件还是正方形时代
        //   的遗留：控件改成矩形后，S 已经等于 g_miniSize，再乘 k 会把目标
        //   区域放大出控件范围（实测 614x307 的框里画出了 1109x554），
        //   表现就是"地图只显示一角 / 大小对不上"。
        const int dx = box.left + mr.left;
        const int dy = box.top + mr.top;
        const int dw = mr.right - mr.left;
        const int dh = mr.bottom - mr.top;

        // 布局诊断：只在尺寸/矩形变化时打一行（不是每次重绘都打）。
        // 排查"地图没铺满 / 大小不对"时看这一行就够：box 是控件框、
        // 地图是可走区包围盒、目标区域是实际 StretchBlt 的范围。
        static int s_lastW = -1, s_lastH = -1, s_lastDw = -1, s_lastDh = -1;
        if (box.right != s_lastW || box.bottom != s_lastH ||
            dw != s_lastDw || dh != s_lastDh) {
            s_lastW = box.right; s_lastH = box.bottom;
            s_lastDw = dw;       s_lastDh = dh;
            LOGI("[GUI] 底图绘制: box=%dx%d 地图=%ux%u 可走区=%d,%d %dx%d -> 目标区域 (%d,%d) %dx%d",
                 (int)(box.right - box.left), (int)(box.bottom - box.top),
                 g_mapW, g_mapH, g_mapInX, g_mapInY, g_mapInW, g_mapInH,
                 dx, dy, dw, dh);
        }

        // ★ 拉伸模式必须是 COLORONCOLOR，不能用 HALFTONE。
        //
        //   HALFTONE 是抖动算法：把源像素按半色调网格重新排布，同一张图
        //   每次画出来的像素排布都可能有细微差别（受 BrushOrg 相位影响）。
        //   叠加 WM_TIMER 的 50ms 高频重绘，地图看起来就是"一直在闪"。
        //   （SetBrushOrgEx 那行也一并删了——它是专门配合 HALFTONE 调
        //   相位的，COLORONCOLOR 下无意义。）
        //
        //   COLORONCOLOR 是简单取样：直接取最近的源像素，结果确定、
        //   每次画出来完全一样，不闪。代价是缩小到很小时丢细节，但
        //   地图视图是"看路"的，不是看像素的，这个代价完全可接受。
        //
        //   实测对比（1725x2139 -> ~700x850 视图）：HALFTONE 下肉眼明显
        //   闪烁；COLORONCOLOR 完全稳定，路和障碍一样清晰。
        SetStretchBltMode(hdc, COLORONCOLOR);
        // 源区域只取"可走区包围盒"，让它缩放到铺满目标矩形。
        // 原来源区域是整张 g_mapW x g_mapH，可走区小的时候道路就被缩没了。
        const int srcW = (g_mapInW > 0) ? g_mapInW : (int)g_mapW;
        const int srcH = (g_mapInH > 0) ? g_mapInH : (int)g_mapH;
        StretchBlt(hdc, dx, dy, dw, dh,
                   g_memDC, g_mapInX, g_mapInY, srcW, srcH, SRCCOPY);
    }
    else {
        HBRUSH eb = CreateSolidBrush(RGB(8, 9, 12));
        FillRect(hdc, &box, eb);
        DeleteObject(eb);
    }

    // 栅格参考线
    {
        HPEN pen = CreatePen(PS_SOLID, 1, mini ? RGB(40, 46, 60) : RGB(52, 60, 78));
        HGDIOBJ op = SelectObject(hdc, pen);
        const int step = S / 9;
        for (int i = 1; i <= 8; ++i) {
            const int x = box.left + i * step;
            MoveToEx(hdc, x, box.top + 2, nullptr); LineTo(hdc, x, box.bottom - 2);
            const int y = box.top + i * step;
            MoveToEx(hdc, box.left + 2, y, nullptr); LineTo(hdc, box.right - 2, y);
        }
        SelectObject(hdc, op);
        DeleteObject(pen);
    }

    // Current ritual scope uses the same world-coordinate circle as selection and A*.
    ritualcombat::Circle ritualCircle;
    if (g_mapSessionId == pipe::GetConnectionSnapshot().sessionId &&
        ritual::MainAutomation().GetCombatCircle(g_mapArea, ritualCircle)) {
        const auto center = toView(ritualCircle.center.x, ritualCircle.center.y);
        const auto lo = toView(ritualCircle.center.x - ritualCircle.radius, ritualCircle.center.y - ritualCircle.radius);
        const auto hi = toView(ritualCircle.center.x + ritualCircle.radius, ritualCircle.center.y + ritualCircle.radius);
        const auto saved = SaveDC(hdc);
        IntersectClipRect(hdc, box.left, box.top, box.right, box.bottom);
        const auto pen = CreatePen(PS_SOLID, 1, RGB(255, 181, 71));
        const auto previousPen = SelectObject(hdc, pen);
        const auto previousBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Ellipse(hdc, lo.x, lo.y, hi.x, hi.y);
        SelectObject(hdc, previousBrush); SelectObject(hdc, previousPen); DeleteObject(pen);
        Dot(hdc, center.x, center.y, 2, RGB(255, 181, 71));
        const auto label = L"祭祀范围 R=" + std::to_wstring(ritualCircle.radius);
        RECT text{center.x + 6, center.y + 5, center.x + 170, center.y + 25};
        SelectObject(hdc, g_fontUI); SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255, 181, 71));
        DrawTextW(hdc, label.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        RestoreDC(hdc, saved);
    }

    // 路径
    if (g_path.size() > 1) {
        const int glowW = mini ? 3 : 7;
        const int lineW = mini ? 1 : 3;

        HPEN glow = CreatePen(PS_SOLID, glowW, col::kPathGlow);
        HGDIOBJ op = SelectObject(hdc, glow);
        for (size_t i = 0; i < g_path.size(); ++i) {
            const POINT p = toView(g_path[i].x, g_path[i].y);
            if (i == 0) MoveToEx(hdc, p.x, p.y, nullptr);
            else        LineTo(hdc, p.x, p.y);
        }
        SelectObject(hdc, op);
        DeleteObject(glow);

        HPEN pen = CreatePen(PS_SOLID, lineW, col::kPath);
        op = SelectObject(hdc, pen);
        for (size_t i = 0; i < g_path.size(); ++i) {
            const POINT p = toView(g_path[i].x, g_path[i].y);
            if (i == 0) MoveToEx(hdc, p.x, p.y, nullptr);
            else        LineTo(hdc, p.x, p.y);
        }
        SelectObject(hdc, op);
        DeleteObject(pen);

        // 航点小点只在放大图里画（小图上会糊成一团）
        if (!mini) {
            for (size_t i = 0; i < g_path.size(); i += 4) {
                const POINT p = toView(g_path[i].x, g_path[i].y);
                if (p.x < box.left || p.x > box.right) continue;
                if (p.y < box.top  || p.y > box.bottom) continue;
                Dot(hdc, p.x, p.y, 2, RGB(150, 210, 255));
            }
        }
    }

    // 目标
    if (g_hasGoal) {
        const POINT p = toView(g_goalX, g_goalY);
        const int r1 = mini ? 5 : 10;
        const int r2 = r1 + (mini ? 2 : (int)(pulse * 5));

        Ring(hdc, p.x, p.y, r2, LerpColor(col::kGoal, col::kPanel, 0.55f), mini ? 1 : 2);
        Ring(hdc, p.x, p.y, r1, col::kGoal, mini ? 1 : 2);

        const int arm = mini ? 8 : 16;
        const int gapR = mini ? 3 : 6;
        HPEN pen = CreatePen(PS_SOLID, mini ? 1 : 2, col::kGoal);
        HGDIOBJ op = SelectObject(hdc, pen);
        MoveToEx(hdc, p.x - arm, p.y, nullptr); LineTo(hdc, p.x - gapR, p.y);
        MoveToEx(hdc, p.x + gapR, p.y, nullptr); LineTo(hdc, p.x + arm, p.y);
        MoveToEx(hdc, p.x, p.y - arm, nullptr); LineTo(hdc, p.x, p.y - gapR);
        MoveToEx(hdc, p.x, p.y + gapR, nullptr); LineTo(hdc, p.x, p.y + arm);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }

    // 角色
    if (g_havePlayer) {
        const POINT p = toView(g_playerX.load(), g_playerY.load());
        const int coreR = mini ? 3 : 5;
        const int glowR = coreR + (mini ? 4 : (int)(pulse * 8));

        Ring(hdc, p.x, p.y, glowR, LerpColor(col::kPlayer, col::kPanel, 0.6f), mini ? 1 : 2);
        Dot(hdc, p.x, p.y, coreR, col::kPlayer);
        Dot(hdc, p.x, p.y, mini ? 1 : 2, col::kPlayerCore);

        // 十字刻线只在大图里画
        if (!mini) {
            const int a = 14, b = 10;
            HPEN pen = CreatePen(PS_SOLID, 1, col::kPlayer);
            HGDIOBJ op = SelectObject(hdc, pen);
            MoveToEx(hdc, p.x - a, p.y, nullptr); LineTo(hdc, p.x - b, p.y);
            MoveToEx(hdc, p.x + b, p.y, nullptr); LineTo(hdc, p.x + a, p.y);
            MoveToEx(hdc, p.x, p.y - a, nullptr); LineTo(hdc, p.x, p.y - b);
            MoveToEx(hdc, p.x, p.y + b, nullptr); LineTo(hdc, p.x, p.y + a);
            SelectObject(hdc, op);
            DeleteObject(pen);
        }
    }
}


// ============================================================
// 绘制两个地图视图 + 标题
// ============================================================
void PaintMap(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdcScreen = BeginPaint(hwnd, &ps);
    if (!hdcScreen) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right;
    const int ch = rc.bottom;
    if (cw <= 0 || ch <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    // ★ 双缓冲：全部画到内存位图，最后一次性 BitBlt 上去。
    //   原来直接画在窗口 DC 上，每次重绘都能看到"先清背景再画内容"，
    //   叠加定时器高频刷新就表现为地图闪。
    HDC hdc = CreateCompatibleDC(hdcScreen);
    HBITMAP bmpMem = CreateCompatibleBitmap(hdcScreen, cw, ch);
    HGDIOBJ oldBmp = SelectObject(hdc, bmpMem);

    {
        HBRUSH bg = CreateSolidBrush(col::kBg);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
    }

    // 动画相位保留（路径/角色的呼吸效果），但不再靠它触发重绘
    const float t = (float)((GetTickCount64() - g_animT0) % 2000) / 2000.0f;
    const float pulse = 0.5f + 0.5f * (float)sin(t * 6.2831853);

    std::lock_guard<std::mutex> lk(g_mtx);

    if (g_mapGray.empty() || g_mapW == 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, col::kTextDim);
        HGDIOBJ old = SelectObject(hdc, g_fontBold);
        DrawTextW(hdc, L"尚未载入地图", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT rc2 = rc;
        rc2.top = rc.top + (rc.bottom - rc.top) / 2 + 26;
        SelectObject(hdc, g_fontUI);
        DrawTextW(hdc, L"点左侧「刷新地图」从游戏读取", -1, &rc2,
                  DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
        return;
    }

    // ---- 地图（控件内部坐标永远从 0,0 开始）----
    RECT box{ 0, 0, g_miniSize, g_miniSize };
    DrawMapInto(hdc, box, WorldToMini, true, pulse);

    // 边框
    {
        HPEN pen = CreatePen(PS_SOLID, 1, col::kBorderGlow);
        HGDIOBJ op = SelectObject(hdc, pen);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, box.left, box.top, box.right, box.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }

    // 左上角标签
    {
        HBRUSH tag = CreateSolidBrush(RGB(14, 16, 22));
        RECT tagRc{ box.left + 1, box.top + 1, box.left + 86, box.top + 25 };
        FillRect(hdc, &tagRc, tag);
        DeleteObject(tag);

        RECT lr{ box.left + 8, box.top + 5, box.right - 6, box.top + 23 };
        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ of = SelectObject(hdc, g_fontUI);
        SetTextColor(hdc, col::kTextDim);
        wchar_t lab[64];
        swprintf_s(lab, L"地图 %d×%d", (int)g_mapW, (int)g_mapH);
        DrawTextW(hdc, lab, -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, of);
    }

    // ---- 一次性贴到屏幕（双缓冲的最终输出）----
    BitBlt(hdcScreen, 0, 0, cw, ch, hdc, 0, 0, SRCCOPY);

    SelectObject(hdc, oldBmp);
    DeleteObject(bmpMem);
    DeleteDC(hdc);

    EndPaint(hwnd, &ps);
}

// ============================================================
// 绘制自绘按钮
// ============================================================
void PaintButton(const Button& b, HDC hdc, RECT rc, bool enabled)
{
    COLORREF base = col::kPanelHi;
    COLORREF text = col::kText;
    COLORREF border = col::kBorder;

    if (b.kind == 0) {            // 主按钮：蓝色
        base   = enabled ? (b.down ? RGB(52, 100, 160) : (b.hot ? RGB(74, 140, 220) : RGB(58, 116, 186)))
                         : RGB(40, 46, 60);
        text   = enabled ? RGB(240, 248, 255) : col::kTextDim;
        border = enabled ? col::kAccent : col::kBorder;
    }
    else if (b.kind == 2) {       // 危险按钮：暗红
        base   = enabled ? (b.down ? RGB(120, 44, 44) : (b.hot ? RGB(104, 52, 52) : RGB(76, 40, 44)))
                         : RGB(40, 46, 60);
        text   = enabled ? RGB(255, 226, 226) : col::kTextDim;
        border = enabled ? RGB(150, 70, 70) : col::kBorder;
    }
    else {                        // 次按钮
        base   = enabled ? (b.down ? RGB(44, 50, 64) : (b.hot ? RGB(52, 58, 74) : col::kPanel))
                         : RGB(32, 36, 46);
        text   = enabled ? col::kText : col::kTextDim;
        border = b.hot && enabled ? col::kBorderGlow : col::kBorder;
    }

    FillRoundRect(hdc, rc, 6, base, border);

    DrawTextIn(hdc, b.text, rc, text, g_fontUI,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ============================================================
// 页签条
//
// 顶部页签与各功能页面一一对应，窄窗口时等比例收窄。
// ============================================================


void ComputeTabs(int clientW)
{
    const int x0 = g_pad;
    const int w = (std::min)(104, (clientW - g_pad * 2 - (kPageCount - 1) * 6) / kPageCount);
    for (int i = 0; i < kPageCount; ++i) {
        g_tabRc[i].left   = x0 + i * (w + 6);
        g_tabRc[i].top    = 8;
        g_tabRc[i].right  = g_tabRc[i].left + w;
        g_tabRc[i].bottom = 8 + 28;
    }
}

void PaintTabs(HDC hdc, int clientW)
{
    // 底条
    RECT bar{ 0, 0, clientW, kTabH };
    HBRUSH b = CreateSolidBrush(RGB(14, 16, 21));
    FillRect(hdc, &bar, b);
    DeleteObject(b);

    HPEN pen = CreatePen(PS_SOLID, 1, col::kBorder);
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, 0, kTabH - 1, nullptr);
    LineTo(hdc, clientW, kTabH - 1);
    SelectObject(hdc, op);
    DeleteObject(pen);

    const wchar_t* names[kPageCount] = { L"寻路", L"背包", L"周围", L"技能槽", L"跟随", L"封包工具", L"祭祀" };

    int invN = 0, aroundN = 0, skillN = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        invN = (int)g_inv.size();
        aroundN = (int)g_around.size();
        skillN = (int)g_skills.slots.size();
    }

    for (int i = 0; i < kPageCount; ++i) {
        const bool active = (g_tab == i);
        RECT r = g_tabRc[i];

        wchar_t label[64];
        if (i == 0 || i >= 4) swprintf_s(label, L"%s", names[i]);
        else if (i == 1) swprintf_s(label, L"%s  %d", names[i], invN);
        else if (i == 2) swprintf_s(label, L"%s  %d", names[i], aroundN);
        else             swprintf_s(label, L"%s  %d", names[i], skillN);

        if (active) {
            FillRoundRect(hdc, r, 6, col::kPanelHi, col::kAccent);
        }
        else {
            FillRoundRect(hdc, r, 6, col::kPanel, col::kBorder);
        }

        DrawTextIn(hdc, label, r,
                   active ? col::kText : col::kTextDim,
                   active ? g_fontBold : g_fontUI,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 右侧提示：快捷键 + 行数
    wchar_t hint[128];
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        count = (g_tab == 1) ? (int)g_inv.size()
              : (g_tab == 2) ? (int)g_around.size()
              : (g_tab == 3) ? (int)g_skills.slots.size() : 0;
    }
    if (g_tab == 0) {
        wcscpy_s(hint, L"Ctrl+C 复制日志   Ctrl+S 导出");
    }
    else if (g_tab == 3) {
        swprintf_s(hint, L"共 %d 条   单击 ID 复制   Ctrl+C", count);
    }
    else if (g_tab == 2) swprintf_s(hint, L"共 %d 条  单击名称复制  Ctrl+C / S", count);
    else if (g_tab == 4) wcscpy_s(hint, L"当前地图 · 动态寻路跟随");
    else if (g_tab == 5) wcscpy_s(hint, L"拾取 · 动作组包 · 明文收发记录");
    else if (g_tab == 6) wcscpy_s(hint, L"候选坐标 · A* 单寻路");
    else {
        swprintf_s(hint, L"共 %d 条   Ctrl+C 复制   Ctrl+S 导出", count);
    }

    RECT hr{ (std::max)(clientW - 340 - g_pad, int(g_tabRc[kPageCount - 1].right) + 12), 8, clientW - g_pad, 36 };
    if (hr.right - hr.left > 250) DrawTextIn(hdc, hint, hr, col::kTextDim, g_fontUI,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

// ============================================================
// 列表页（背包 / 周围共用）
//
// 纯 GDI 表格：不搞 EDIT/LISTVIEW 那些子窗口，
// 全部自绘 + 自己处理滚轮，风格可控且不会有白底闪烁。
// ============================================================
struct ListColumn {
    const wchar_t* title;
    int width;
    int align;      // 0=左 1=右 2=居中
};

RECT TableCellRect(const RECT& row, const ListColumn* cols, int column)
{
    int x = row.left + 8;
    for (int i = 0; i < column; ++i) x += cols[i].width;
    return {x, row.top, x + cols[column].width - 8, row.bottom};
}

void AddCopyCell(const RECT& row, const RECT& body, const ListColumn* cols,
                 int column, const std::string& text, int page)
{
    RECT bounds{}, cell = TableCellRect(row, cols, column);
    if (!text.empty() && IntersectRect(&bounds, &cell, &body))
        g_copyCells.push_back({bounds, text, page});
}

void PaintTableHeader(HDC hdc, const RECT& rc, const ListColumn* cols, int nCols)
{
    HBRUSH b = CreateSolidBrush(RGB(22, 25, 33));
    FillRect(hdc, &rc, b);
    DeleteObject(b);

    HPEN pen = CreatePen(PS_SOLID, 1, col::kBorder);
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
    LineTo(hdc, rc.right, rc.bottom - 1);
    SelectObject(hdc, op);
    DeleteObject(pen);

    for (int i = 0; i < nCols; ++i) {
        RECT cr = TableCellRect(rc, cols, i);
        UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (cols[i].align == 1) fmt = DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (cols[i].align == 2) fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        DrawTextIn(hdc, cols[i].title, cr, col::kTextDim, g_fontBold, fmt);
    }
}

// 画一行表格内容
void PaintTableRow(HDC hdc, const RECT& rowRc, const ListColumn* cols, int nCols,
                   const wchar_t** cells, COLORREF rowColor, bool zebra)
{
    if (zebra) {
        HBRUSH b = CreateSolidBrush(RGB(23, 26, 34));
        FillRect(hdc, &rowRc, b);
        DeleteObject(b);
    }

    for (int i = 0; i < nCols; ++i) {
        RECT cr = TableCellRect(rowRc, cols, i);
        UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (cols[i].align == 1) fmt = DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (cols[i].align == 2) fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        DrawTextIn(hdc, cells[i] ? cells[i] : L"", cr,
                   i == 0 ? rowColor : col::kText, g_fontUI, fmt);
    }
}

// 空状态提示
void PaintEmpty(HDC hdc, const RECT& rc, const wchar_t* line1, const wchar_t* line2)
{
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ old = SelectObject(hdc, g_fontBold);
    SetTextColor(hdc, col::kTextDim);
    RECT r1 = rc;
    r1.top = rc.top + (rc.bottom - rc.top) / 2 - 24;
    DrawTextW(hdc, line1, -1, &r1, DT_CENTER | DT_TOP | DT_SINGLELINE);

    SelectObject(hdc, g_fontUI);
    RECT r2 = rc;
    r2.top = r1.top + 26;
    DrawTextW(hdc, line2, -1, &r2, DT_CENTER | DT_TOP | DT_SINGLELINE);
    SelectObject(hdc, old);
}

// 背包页
void PaintInventoryPage(HDC hdc, const RECT& area)
{
    // 统计栏
    RECT head{ area.left, area.top, area.right, area.top + 40 };
    FillRoundRect(hdc, head, 8, col::kPanel, col::kBorder);

    std::vector<poe::InventoryItem> items;
    bool have;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        items = g_inv;
        have = g_haveInv;
        err = g_scanErr;
    }

    // 按品级统计
    int cnt[5] = { 0, 0, 0, 0, 0 };
    uint64_t totalStack = 0;
    for (const auto& it : items) {
        const int g = (it.grade >= 0 && it.grade <= 3) ? it.grade : 4;
        cnt[g]++;
        totalStack += it.num;
    }

    wchar_t summary[256];
    swprintf_s(summary, L"共 %zu 件    白 %d    蓝 %d    金 %d    暗金 %d    合计 %llu 个",
               items.size(), cnt[0], cnt[1], cnt[2], cnt[3],
               (unsigned long long)totalStack);
    RECT sr{ head.left + 14, head.top, head.right - 14, head.bottom };
    DrawTextIn(hdc, summary, sr, col::kText, g_fontUI,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 表头
    // ★ 全部固定宽度、全部左对齐。
    //   名称列不再"吃剩余宽度"——那样会把后面的列顶到窗口右边，
    //   看起来就像数量/entity 靠右了。整表从左侧依次排开，
    //   右边留白即可。
    ListColumn cols[] = {
        { L"格",      72,  0 },
        { L"品级",    80,  0 },
        { L"名称",    200, 0 },
        { L"数量",    72,  0 },
        { L"entity",  170, 0 },
    };
    const int nCols = 5;

    RECT hdr{ area.left, head.bottom + 6, area.right, head.bottom + 32 };
    PaintTableHeader(hdc, hdr, cols, nCols);

    // 内容区
    RECT body{ area.left, hdr.bottom, area.right, area.bottom };
    if (items.empty()) {
        if (!have)          PaintEmpty(hdc, body, L"还没扫描", L"点下面的「扫描背包」按钮");
        else if (!err.empty()) PaintEmpty(hdc, body, L"扫描失败", Utf8ToWide(err).c_str());
        else                PaintEmpty(hdc, body, L"背包是空的", L"");
        return;
    }

    const int rowH = 24;
    const int visible = (body.bottom - body.top) / rowH;
    int maxScroll = (int)items.size() - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_invScroll > maxScroll) g_invScroll = maxScroll;
    if (g_invScroll < 0) g_invScroll = 0;

    // 品级配色
    const COLORREF gradeColor[5] = {
        RGB(200, 200, 200),   // 白
        RGB(120, 170, 255),   // 蓝
        RGB(255, 215, 120),   // 金
        RGB(220, 140, 60),    // 暗金
        RGB(160, 160, 160),
    };

    const int start = g_invScroll;
    const int end = (start + visible < (int)items.size()) ? (start + visible) : (int)items.size();

    int y = body.top;
    for (int i = start; i < end; ++i) {
        const auto& it = items[(size_t)i];
        RECT row{ body.left, y, body.right, y + rowH };

        wchar_t c0[32], c1[32], c2[512], c3[32], c4[40];
        swprintf_s(c0, L"0x%X", (unsigned)it.key);
        swprintf_s(c1, L"%s", Utf8ToWide(poe::GradeName(it.grade)).c_str());

        // 名称 = 前缀 + 名字
        std::string full = it.prefix;
        full += it.name;
        swprintf_s(c2, L"%s", Utf8ToWide(full).c_str());
        swprintf_s(c3, L"%u", it.num);
        swprintf_s(c4, L"0x%llX", (unsigned long long)it.entity);

        const wchar_t* cells[5] = { c0, c1, c2, c3, c4 };
        const int gi = (it.grade >= 0 && it.grade <= 3) ? it.grade : 4;

        PaintTableRow(hdc, row, cols, nCols, cells, gradeColor[gi], ((i - start) % 2) == 1);
        y += rowH;
    }

    // 滚动条
    if (maxScroll > 0) {
        const int trackH = body.bottom - body.top;
        const int thumbH = trackH * visible / (int)items.size();
        const int thumbY = body.top + (trackH - thumbH) * g_invScroll / maxScroll;
        RECT thumb{ body.right - 6, thumbY, body.right - 2, thumbY + thumbH };
        HBRUSH b = CreateSolidBrush(col::kBorderGlow);
        FillRect(hdc, &thumb, b);
        DeleteObject(b);
    }
}

// 周围实体页
void PaintAroundPage(HDC hdc, const RECT& area)
{
    RECT head{ area.left, area.top, area.right, area.top + 40 };
    FillRoundRect(hdc, head, 8, col::kPanel, col::kBorder);

    std::vector<poe::EntitySnapshot> list;
    bool have;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        list = g_around;
        have = g_haveAround;
        err = g_scanErr;
    }

    // 按类型统计
    int nMonster = 0, nItem = 0, nChest = 0, nOther = 0;
    for (const auto& s : list) {
        switch (s.kind) {
        case poe::EntityKind::Monster:   nMonster++; break;
        case poe::EntityKind::WorldItem: nItem++;    break;
        case poe::EntityKind::Chest:     nChest++;   break;
        default:                         nOther++;   break;
        }
    }

    wchar_t summary[256];
    swprintf_s(summary, L"共 %zu 个    怪物 %d    掉落 %d    箱子 %d    其它 %d",
               list.size(), nMonster, nItem, nChest, nOther);
    RECT sr{ head.left + 14, head.top, head.right - 14, head.bottom };
    DrawTextIn(hdc, summary, sr, col::kText, g_fontUI,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 全部固定宽度、全部左对齐（同上：不抢剩余宽度）
    ListColumn cols[] = {
        { L"类型",   80,  0 },
        { L"名称",   150, 0 },
        { L"X",      90,  0 },
        { L"Y",      90,  0 },
        { L"HP",     80,  0 },
        { L"entity", 170, 0 },
    };
    const int nCols = 6;

    RECT hdr{ area.left, head.bottom + 6, area.right, head.bottom + 32 };
    PaintTableHeader(hdc, hdr, cols, nCols);

    RECT body{ area.left, hdr.bottom, area.right, area.bottom };
    if (list.empty()) {
        if (!have)             PaintEmpty(hdc, body, L"还没扫描", L"点下面的「扫描周围」按钮");
        else if (!err.empty())  PaintEmpty(hdc, body, L"扫描失败", Utf8ToWide(err).c_str());
        else                   PaintEmpty(hdc, body, L"周围没有实体", L"");
        return;
    }

    const int rowH = 24;
    const int visible = (body.bottom - body.top) / rowH;
    int maxScroll = (int)list.size() - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_aroundScroll > maxScroll) g_aroundScroll = maxScroll;
    if (g_aroundScroll < 0) g_aroundScroll = 0;

    const int start = g_aroundScroll;
    const int end = (start + visible < (int)list.size()) ? (start + visible) : (int)list.size();

    int y = body.top;
    for (int i = start; i < end; ++i) {
        const auto& s = list[(size_t)i];
        RECT row{ body.left, y, body.right, y + rowH };

        COLORREF kc = col::kText;
        const wchar_t* kindName = L"其它";
        switch (s.kind) {
        case poe::EntityKind::Monster:   kindName = L"怪物"; kc = RGB(255, 120, 120); break;
        case poe::EntityKind::WorldItem: kindName = L"掉落"; kc = RGB(120, 230, 160); break;
        case poe::EntityKind::Chest:     kindName = L"箱子"; kc = RGB(255, 200, 100); break;
        case poe::EntityKind::Player:    kindName = L"玩家"; kc = RGB(120, 180, 255); break;
        case poe::EntityKind::NPC:       kindName = L"NPC";  kc = RGB(180, 160, 255); break;
        case poe::EntityKind::Portal:    kindName = L"传送门"; kc = RGB(180, 220, 255); break;
        case poe::EntityKind::Stash:     kindName = L"仓库"; kc = RGB(200, 180, 120); break;
        case poe::EntityKind::GuildStash:kindName = L"公会仓"; kc = RGB(200, 180, 120); break;
        case poe::EntityKind::Hideout:   kindName = L"藏身处"; kc = RGB(160, 200, 180); break;
        case poe::EntityKind::CheckPoint:kindName = L"存档点"; kc = RGB(160, 200, 180); break;
        case poe::EntityKind::WaterWell: kindName = L"水井"; kc = RGB(160, 200, 220); break;
        default: break;
        }

        wchar_t c1[512], c2[32], c3[32], c4[32], c5[40];
        swprintf_s(c1, L"%s", Utf8ToWide(s.name).c_str());
        swprintf_s(c2, L"%d", (int)s.x);
        swprintf_s(c3, L"%d", (int)s.y);
        if (s.hp > 0) swprintf_s(c4, L"%u", s.hp);
        else          wcscpy_s(c4, L"-");
        swprintf_s(c5, L"0x%llX", (unsigned long long)s.entity);

        const wchar_t* cells[6] = { kindName, c1, c2, c3, c4, c5 };
        PaintTableRow(hdc, row, cols, nCols, cells, kc, ((i - start) % 2) == 1);
        AddCopyCell(row, body, cols, 1, s.name, 2);
        y += rowH;
    }

    if (maxScroll > 0) {
        const int trackH = body.bottom - body.top;
        const int thumbH = trackH * visible / (int)list.size();
        const int thumbY = body.top + (trackH - thumbH) * g_aroundScroll / maxScroll;
        RECT thumb{ body.right - 6, thumbY, body.right - 2, thumbY + thumbH };
        HBRUSH b = CreateSolidBrush(col::kBorderGlow);
        FillRect(hdc, &thumb, b);
        DeleteObject(b);
    }
}

// 技能槽页：空槽保留索引；技能 ID 只显示完整 32 位十六进制，单击可复制。
void PaintSkillSlotsPage(HDC hdc, const RECT& area)
{
    poe::SkillSlotsSnapshot snapshot;
    bool have;
    std::string error;
    std::wstring updated;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        snapshot = g_skills;
        have = g_haveSkills;
        error = g_skillErr;
        updated = g_skillUpdated;
    }
    const bool busy = g_skillBusy.load();
    const size_t bound = std::count_if(snapshot.slots.begin(), snapshot.slots.end(),
        [](const poe::SkillSlot& slot) { return slot.occupied; });
    RECT head{area.left, area.top, area.right, area.top + 80};
    FillRoundRect(hdc, head, 8, col::kPanel, col::kBorder);
    wchar_t summary[256];
    if (!snapshot.characterName.empty()) {
        swprintf_s(summary, L"角色 %s   槽位 %zu / 已绑定 %zu",
            Utf8ToWide(snapshot.characterName).c_str(), snapshot.slots.size(), bound);
    } else {
        wcscpy_s(summary, L"当前角色技能槽（从 0 开始）");
    }
    RECT sr{head.left + 14, head.top, head.right - 190, head.top + 40};
    DrawTextIn(hdc, summary, sr, col::kText, g_fontUI,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    std::wstring status;
    if (busy) status = L"正在读取当前角色技能槽…";
    else if (!error.empty()) status = Utf8ToWide(error);
    else if (have) {
        status = L"已刷新 " + updated + L"   PID=" + std::to_wstring(snapshot.pid) +
            L"   单击技能 ID 复制（0x 开头的十六进制）";
    } else status = L"点击右侧「获取 / 刷新技能槽」；Ctrl+C 可复制当前列表。";
    RECT statusRc{head.left + 14, head.top + 42, head.right - 14, head.bottom - 4};
    DrawTextIn(hdc, status.c_str(), statusRc, error.empty() ? col::kTextDim : RGB(255,120,120),
               g_fontUI, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);

    const int width = area.right - area.left - 12;
    const int nameWidth = std::clamp(width / 5, 102, 180);
    const ListColumn cols[] = {
        {L"技能槽ID", 76, 0},
        {L"技能内部标识", width - 76 - nameWidth - 150, 0},
        {L"繁体中文名", nameWidth, 0},
        {L"技能ID", 150, 0},
    };
    RECT hdr{area.left, head.bottom + 6, area.right, head.bottom + 32};
    PaintTableHeader(hdc, hdr, cols, 4);
    RECT body{area.left, hdr.bottom, area.right, area.bottom};
    if (snapshot.slots.empty()) {
        PaintEmpty(hdc, body, busy ? L"正在读取…" : !error.empty() ? L"获取失败" :
                   have ? L"没有技能槽" : L"尚未获取技能槽",
                   !error.empty() ? L"请检查上方错误信息后重新刷新" : L"读取游戏内存，不需要注入");
        return;
    }
    constexpr int rowH = 44;
    const int visible = std::max(1L, (body.bottom - body.top) / rowH);
    const int maxScroll = std::max(0, static_cast<int>(snapshot.slots.size()) - visible);
    g_skillScroll = std::clamp(g_skillScroll, 0, maxScroll);
    const int end = std::min(g_skillScroll + visible, static_cast<int>(snapshot.slots.size()));
    for (int i = g_skillScroll, y = body.top; i < end; ++i, y += rowH) {
        const auto& slot = snapshot.slots[static_cast<size_t>(i)];
        const auto slotText = std::to_wstring(slot.slotId);
        const auto internal = slot.occupied ? Utf8ToWide(slot.internalId) : L"（空槽）";
        const auto name = Utf8ToWide(slot.localizedName);
        const wchar_t* cells[] = {slotText.c_str(), internal.c_str(), name.c_str(), L""};
        RECT row{body.left, y, body.right, y + rowH};
        PaintTableRow(hdc, row, cols, 4, cells, slot.occupied ? col::kAccent : col::kTextDim,
                      ((i - g_skillScroll) % 2) == 1);
        if (slot.occupied) {
            const auto id = poe::FormatSkillId(slot.skillId);
            RECT cell = TableCellRect(row, cols, 3);
            DrawTextIn(hdc, Utf8ToWide(id).c_str(), cell, col::kText, g_fontMono,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            AddCopyCell(row, body, cols, 3, id, 3);
        }
    }
    if (maxScroll > 0) {
        const int trackH = body.bottom - body.top;
        const int thumbH = std::max(12, trackH * visible / static_cast<int>(snapshot.slots.size()));
        const int thumbY = body.top + (trackH - thumbH) * g_skillScroll / maxScroll;
        RECT thumb{body.right - 6, thumbY, body.right - 2, thumbY + thumbH};
        HBRUSH brush = CreateSolidBrush(col::kBorderGlow);
        FillRect(hdc, &thumb, brush);
        DeleteObject(brush);
    }
}

// 底部状态条（所有页共用）
void PaintBottomBar(HDC hdc, const RECT& client)
{
    bool busy;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        busy = g_busy || g_skillBusy.load();
    }

    RECT bar{ 0, client.bottom - 30, client.right, client.bottom };
    HBRUSH br = CreateSolidBrush(RGB(14, 16, 21));
    FillRect(hdc, &bar, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, col::kBorder);
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, 0, bar.top, nullptr);
    LineTo(hdc, client.right, bar.top);
    SelectObject(hdc, op);
    DeleteObject(pen);

    RECT lr{ g_pad, bar.top, client.right / 2, bar.bottom - 8 };
    wchar_t info[192];
    wcscpy_s(info, L"Monitor  \\\\.\\pipe\\Lmy        Plug.exe · NavGUI");
    DrawTextIn(hdc, info, lr, col::kTextDim, g_fontMono,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT rr{ client.right / 2, bar.top, client.right - g_pad, bar.bottom - 8 };
    const bool copied = !g_copyFeedback.empty() && GetTickCount64() < g_copyFeedbackUntil;
    DrawTextIn(hdc, copied ? g_copyFeedback.c_str() : busy ? L"运行中…" : L"就绪", rr,
               copied || busy ? col::kAccent : col::kTextDim, copied ? g_fontUI : g_fontMono,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}
// ============================================================
// 绘制右侧面板与底部状态条
// ============================================================
void PaintChrome(HWND hwnd, HDC hdc)
{
    RECT client;
    GetClientRect(hwnd, &client);

    // 整窗底色
    HBRUSH bg = CreateSolidBrush(col::kBg);
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    // 顶部页签条
    ComputeTabs(client.right);
    PaintTabs(hdc, client.right);
    g_copyCells.clear();

    // 非寻路页：画列表页，右侧面板与地图都不画
    if (g_tab != 0) {
        RECT page{ g_pad, kTabH + g_pad, client.right - g_pad, client.bottom - 38 };
        if (g_tab == 1) PaintInventoryPage(hdc, page);
        else if (g_tab == 2) PaintAroundPage(hdc, page);
        else if (g_tab == 3) PaintSkillSlotsPage(hdc, page);
        PaintBottomBar(hdc, client);
        return;
    }

    const float t = (float)((GetTickCount64() - g_animT0) % 1600) / 1600.0f;
    const float pulse = 0.5f + 0.5f * (float)sin(t * 6.2831853);

    std::string status, detail;
    poe::WorldAreaSnapshot mapArea;
    std::vector<std::string> logs;
    bool busy;
    bool hasGoal, havePlayer;
    int  state;
    int  pathN;
    uint32_t pw = 0, ph = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        status = g_status;
        detail = g_detail;
        logs = g_logs;
        busy = g_busy;
        state = g_state;
        hasGoal = g_hasGoal;
        havePlayer = g_havePlayer;
        pathN = (int)g_path.size();
        pw = g_mapW; ph = g_mapH;
        mapArea = g_mapArea;
    }

    // ---- 面板在左，地图在右 ----
    const int px = g_pad;
    const int panelW = client.right - g_pad * 2 - g_miniSize - g_pad;
    if (panelW < 100) return;

    // Keep identity outside the map canvas so it never covers terrain or markers.
    {
        const int top = g_miniY - kMapHeaderH - kMapHeaderGap;
        RECT card{g_miniX, top, g_miniX + g_miniSize, top + kMapHeaderH};
        FillRoundRect(hdc, card, 8, col::kPanel, col::kBorder);
        const auto title = mapArea.Available() ? L"地图  " + Utf8ToWide(mapArea.name) :
            std::wstring(busy ? L"正在读取地图信息…" : L"地图名称：未读取");
        const auto id = mapArea.Available() ? L"ID  " + Utf8ToWide(mapArea.id) : std::wstring(L"地图 ID：—");
        RECT nameRect{card.left + 10, top + 6, card.right - 10, top + 29};
        RECT idRect{card.left + 10, top + 30, card.right - 10, top + 50};
        DrawTextIn(hdc, title.c_str(), nameRect, col::kText, g_fontBold,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawTextIn(hdc, id.c_str(), idRect, col::kTextDim, g_fontUI,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    int y = g_pad + kTabH;   // 让出顶部页签条的高度

    // ---------- 整块面板底板 ----------
    // 所有内容画在一张卡片里，用分隔线切分区块，
    // 比多个浮空小块干净得多（之前卡片之间有背景空隙，看着像断开了）。
    {
        RECT panel{ px - 4, g_pad + kTabH - 4, px + panelW + 4, client.bottom - 38 };
        FillRoundRect(hdc, panel, 10, col::kPanel, col::kBorder);
    }

    // ---------- 标题区 ----------
    // ★ y 已经包含 kTabH 偏移（见上面 int y = g_pad + kTabH）
    //   之前这里是 y + 10 起画，和顶部页签条贴太近导致视觉重叠
    {
        RECT card{ px, y, px + panelW, y + 56 };

        // 左侧强调竖条
        RECT bar{ px + 1, y + 12, px + 4, y + 44 };
        FillRoundRectSolid(hdc, bar, 2, col::kAccent);

        RECT tr{ px + 16, y + 10, px + panelW - 12, y + 34 };
        DrawTextIn(hdc, L"自动寻路", tr, col::kText, g_fontBig,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT sr{ px + 16, y + 34, px + panelW - 12, y + 50 };
        wchar_t sub[128];
        if (pw > 0 && ph > 0) {
            swprintf_s(sub, L"地图 %u × %u   路径 %d 点", pw, ph, pathN);
        }
        else {
            wcscpy_s(sub, L"等待地图数据");
        }
        DrawTextIn(hdc, sub, sr, col::kTextDim, g_fontUI,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 64;
    }

    // ---------- 状态卡 ----------
    {
        // 分隔线
        {
            HPEN pen = CreatePen(PS_SOLID, 1, col::kBorder);
            HGDIOBJ op = SelectObject(hdc, pen);
            MoveToEx(hdc, px + 8, y - 4, nullptr);
            LineTo(hdc, px + panelW - 8, y - 4);
            SelectObject(hdc, op);
            DeleteObject(pen);
        }

        // 状态灯
        COLORREF dotC = col::kTextDim;
        if (state == 1)      dotC = LerpColor(col::kAccent, RGB(200, 230, 255), pulse);
        else if (state == 2) dotC = col::kGood;
        else if (state == 3) dotC = col::kBad;
        else                 dotC = col::kTextDim;

        // 忙碌时画呼吸光晕
        if (state == 1) {
            Ring(hdc, px + 22, y + 24, 10 + (int)(pulse * 4),
                 LerpColor(col::kAccent, col::kPanel, 0.55f), 2);
        }
        Dot(hdc, px + 22, y + 24, 5, dotC);

        RECT tr{ px + 38, y + 10, px + panelW - 12, y + 32 };
        DrawTextIn(hdc, Utf8ToWide(status).c_str(), tr, col::kText, g_fontBold,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT dr{ px + 38, y + 34, px + panelW - 12, y + 56 };
        DrawTextIn(hdc, Utf8ToWide(detail).c_str(), dr, col::kTextDim, g_fontUI,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 角色/目标坐标小字
        RECT cr{ px + 16, y + 56, px + panelW - 12, y + 70 };
        wchar_t coords[160] = { 0 };
        if (havePlayer && hasGoal) {
            int32_t gx, gy;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                gx = g_goalX; gy = g_goalY;
            }
            swprintf_s(coords, L"角色 (%d,%d)   →   目标 (%d,%d)",
                       g_playerX.load(), g_playerY.load(), gx, gy);
        }
        else if (havePlayer) {
            swprintf_s(coords, L"角色 (%d,%d)   目标未选", g_playerX.load(), g_playerY.load());
        }
        else {
            wcscpy_s(coords, L"未收到角色坐标");
        }
        DrawTextIn(hdc, coords, cr, col::kTextDim, g_fontMono,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 82;
    }

    // ---------- 按钮 ----------
    //
    // ★ 这里原来有一段 paintBtn，用 GetWindowRect + MapWindowPoints 把每个
    //   自绘按钮再往**父窗口的 DC** 上画一遍。那段是纯粹的重复绘制：
    //   按钮是真正的子窗口（类 L"Poe2Button"），它们的 BtnProc 里已经有
    //   WM_PAINT → PaintButton，自己会画。
    //
    //   两套绘制叠在同一个矩形上，加上父窗口整幅 BitBlt 会把子窗口区域
    //   一起涂掉（缺 WS_CLIPCHILDREN 时），像素就在"父窗口画的"和
    //   "子窗口画的"之间来回翻，视觉上就是按钮和周围卡片一直在抖。
    //   现在父窗口已加 WS_CLIPCHILDREN，这里再画也只会被裁掉，
    //   所以整段删掉 —— 让每个按钮只有一个绘制者。

    // ---------- 注入状态文字 ----------
    // 一行 18px 小字，紧贴「注入 DLL」按钮下方：
    //   "已注入" 用正常色，其它状态用灰/警示色，一眼判断当前能不能点。
    //
    // ★ 起点要跳过按钮本身（36 高 + 4 间距）。
    //   原来直接从 Y_INJECT_TOP 画到 Y_INJECT_END，把整个按钮矩形也
    //   算进了文字区，等于每帧往按钮上再刷一遍文字（缺 WS_CLIPCHILDREN
    //   时这块就是闪烁最厉害的地方之一）。
    if (Y_INJECT_TOP > 0 && Y_INJECT_END > Y_INJECT_TOP) {
        const int stTop = Y_INJECT_END - 18;
        if (Y_INJECT_END > stTop) {
            RECT ir{ px + 2, stTop, px + panelW - 2, Y_INJECT_END };
            const int code = g_injectCode.load();
            const COLORREF c =
                (code == kInjectOk)                 ? col::kGood :
                (code == kInjectFail)               ? col::kBad  :
                (code == kInjectBusy || code == kInjectWait ||
                 code == kInjectIncompatible || code == kInjectVersionUnknown) ? col::kWarn :
                col::kTextDim;
            DrawTextIn(hdc, InjectStateText(code), ir, c, g_fontUI,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---------- 裸走路 call 标定区 ----------
    //
    // 画在右侧地图刷新按钮下方：标题 + 结果两行。
    // 输入框和按钮是真正的子窗口（EDIT / 自绘按钮），这里只画文字，
    // 不画它们的边框 —— 子窗口自己会刷。
    //
    // ★ 这里的纵向坐标必须和 MoveButtons 里的摆位算法**完全一致**，
    //   否则文字会压到子窗口上（之前就是标题压在 EDIT 行上）。
    //   MoveButtons 里的顺序是：
    //     卡片顶边 = Y_WALKCALL_TOP
    //     标题行   = TOP + 0   .. TOP + 22   （kTitleH）
    //     EDIT 行  = TOP + 22  .. +26        （editH）
    //     「走一次」= TOP + 54  .. +36
    //     结果文字 = TOP + 96  ..
    const int walkX = g_miniX, walkW = g_miniSize;
    if (Y_WALKCALL_TOP > 0 && Y_WALKCALL_END > Y_WALKCALL_TOP) {
        RECT bg{ walkX, Y_WALKCALL_TOP, walkX + walkW, Y_WALKCALL_END };
        HBRUSH panelBrush = CreateSolidBrush(col::kPanel);
        FillRect(hdc, &bg, panelBrush);
        DeleteObject(panelBrush);
        {
            HBRUSH borderBrush = CreateSolidBrush(col::kBorder);
            RECT fr{ bg.left, bg.top, bg.right - 1, bg.bottom - 1 };
            FrameRect(hdc, &fr, borderBrush);
            DeleteObject(borderBrush);
        }

        const int kTitleH = 22;      // 与 MoveButtons 保持一致
        const int kEditH  = 26;
        const int kWcGap  = 6;

        // 标题
        RECT hr{ walkX + 12, Y_WALKCALL_TOP + 4, walkX + walkW - 10,
                 Y_WALKCALL_TOP + 4 + 18 };
        DrawTextIn(hdc, L"走路 call 标定", hr, col::kTextDim, g_fontBold,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 结果文字起点 = 标题 + EDIT 行 + 「走一次」按钮 + 间距
        const int resTop = Y_WALKCALL_TOP + kTitleH + kEditH + kWcGap + 36 + kWcGap;

        // 结果行：一行放"角色 / 距离 / 耗时"，一行放"目标"
        wchar_t line1[192];
        if (g_wcHaveResult) {
            swprintf_s(line1, L"角色(%d,%d)  距离 %d → %d  %ums  r=%d",
                       g_wcPlayerX, g_wcPlayerY,
                       g_wcDistBefore, g_wcDistAfter,
                       (unsigned)g_wcElapsed, g_wcResult);
        }
        else {
            wcscpy_s(line1, L"填目标坐标后点「走一次」");
        }
        RECT r1{ walkX + 12, resTop, walkX + walkW - 10, resTop + 16 };
        DrawTextIn(hdc, line1, r1, col::kText, g_fontMono,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (g_wcHaveResult) {
            wchar_t line2[128];
            swprintf_s(line2, L"目标(%d,%d)", g_wcTargetX, g_wcTargetY);
            RECT r2{ walkX + 12, resTop + 16, walkX + walkW - 10, resTop + 32 };
            DrawTextIn(hdc, line2, r2, col::kTextDim, g_fontMono,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---------- 日志卡 ----------
    // ★ 原来这里还有一张"操作说明"卡（4 行灰字）画在 Y_BUTTON_END 起，
    //   而日志卡 Y_LOG_TOP 也是从同一个 Y 起 —— 两块内容直接叠在一起，
    //   谁都看不清。现在把操作说明删掉（那几行的价值早就被日志覆盖了，
    //   "游戏必须在前台"挪到底部状态条），整个下方区域全给日志。
    //
    // ★ 日志卡必须先铺一块**不透明**的面板底色再画文字：
    //   主窗口 WM_PAINT 是双缓冲整幅重画，但这块区域下方还叠着
    //   WM_ERASEBKGND 画的内容；没有实心底色时，日志文字会和
    //   穿透过来的其它绘制叠在一起（实测出现"日志和操作说明重叠"）。
    if (client.bottom - g_pad - 34 > Y_LOG_TOP + 50) {
        // 日志紧跟恢复区；矮窗口优先完整显示恢复设置。
        const int logTop = Y_LOG_TOP;
        const int logBot = client.bottom - g_pad - 34;

        // 先铺底色，把这一块完全盖住
        RECT bg{ px, logTop, px + panelW, logBot };
        HBRUSH panelBrush = CreateSolidBrush(col::kPanel);
        FillRect(hdc, &bg, panelBrush);
        DeleteObject(panelBrush);
        {
            // FrameRect 收的是画刷不是画笔
            HBRUSH borderBrush = CreateSolidBrush(col::kBorder);
            RECT fr{ bg.left, bg.top, bg.right - 1, bg.bottom - 1 };
            FrameRect(hdc, &fr, borderBrush);
            DeleteObject(borderBrush);
        }

        // 阈值从 40 降到 20：窗口一矮（或者上面按钮行数变多）就会掉到
        // 40 以下，那时整块日志卡直接不画 —— 用户只能看到一片空白，
        // 根本不知道发生了什么。宁可挤一点也至少画 1 行。
        if (logBot - logTop > 20) {
            // 标题行：左边"日志"，右边提示"游戏必须在前台"
            RECT hr{ px + 16, logTop + 8, px + panelW - 12, logTop + 26 };
            DrawTextIn(hdc, L"日志", hr, col::kTextDim, g_fontBold,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT tip{ px + 16, logTop + 8, px + panelW - 12, logTop + 26 };
            DrawTextIn(hdc, L"游戏必须在前台！   Ctrl+C 复制 / Ctrl+S 导出",
                       tip, col::kTextDim, g_fontUI,
                       DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            // 只画最近能放下的行
            const int lineH = 17;
            int maxLines = (logBot - logTop - 34) / lineH;
            if (maxLines < 1) maxLines = 1;

            int start = (int)logs.size() - maxLines;
            if (start < 0) start = 0;

            int ly = logTop + 30;
            for (int i = start; i < (int)logs.size(); ++i) {
                RECT lr{ px + 14, ly, px + panelW - 10, ly + lineH };
                const std::wstring w = Utf8ToWide(logs[(size_t)i]);
                // 日志是这一块的核心信息，用 kText（比 kTextDim 亮）——
                // 原来用 kTextDim，在深色底上偏灰，验收时很难一眼看清
                DrawTextIn(hdc, w.c_str(), lr, col::kText, g_fontMono,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                ly += lineH;
            }
        }
    }

    // ---------- 底部状态条 ----------
    PaintBottomBar(hdc, client);
}

// ============================================================
// 按钮摆位（WM_SIZE 里调用，同时记录面板各区块的 Y 坐标）
// ============================================================
void MoveButtons(HWND hwnd, int px, int panelW, int startY)
{
    int y = startY;
    RECT client{}; GetClientRect(hwnd, &client);
    const bool compact = client.bottom < 760;
    const int injectHeight = compact ? 30 : 36;
    const int actionHeight = compact ? 30 : 44;
    const int settingsHeight = compact ? 30 : 40;
    const int rowGap = compact ? 6 : 8;

    // Map actions stay with the map, including after a resize/re-layout.
    SetWindowPos(g_btnRefresh.hwnd, nullptr, g_miniX,
                 g_miniY + g_miniSize + kMapActionGap, g_miniSize, kMapActionH,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // 注入是第一步：按钮与状态一起放在左侧操作区最前面。
    Y_INJECT_TOP = y;
    if (g_btnInject.hwnd) {
        SetWindowPos(g_btnInject.hwnd, nullptr, px, y, panelW, injectHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Y_INJECT_END = y + injectHeight + 4 + 18;
    y = Y_INJECT_END + rowGap;

    const int halfW = (panelW - 8) / 2;
    const int rightW = panelW - halfW - 8;
    SetWindowPos(g_btnGo.hwnd, nullptr, px, y, halfW, actionHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_btnStop.hwnd, nullptr, px + halfW + 8, y, rightW, actionHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += actionHeight + rowGap;

    // 第二排统一控制寻路与技能连招的并行启动、停止。
    SetWindowPos(g_btnNavComboStart.hwnd, nullptr, px, y, halfW, actionHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_btnNavComboStop.hwnd, nullptr, px + halfW + 8, y, rightW, actionHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += actionHeight + rowGap;

    SetWindowPos(g_btnNavSettings.hwnd, nullptr, px, y, halfW, settingsHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_btnCombo.hwnd, nullptr, px + halfW + 8, y, rightW, settingsHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += settingsHeight + rowGap;
    Y_BUTTON_END = y;
    const int viewWidth = (std::max)(148, (std::min)(190, panelW / 4));
    const int recoveryWidth = panelW - viewWidth - 8;
    recoveryui::MovePanel(px, y + 6, recoveryWidth);
    viewui::MovePanel(px + recoveryWidth + 8, y + 6, viewWidth, recoveryui::kHeight);
    Y_RECOVERY_END = y + 6 + recoveryui::kHeight;

    // ---------- 走路 call 标定区 ----------
    //
    // 布局（宽为地图边长）：
    //   行0 标题"走路 call 标定"        ← PaintChrome 画的
    //   行1 [x输入][y输入][取角色坐标]   ← 三个子窗口横排
    //   行2 [走一次]                     ← 整宽按钮
    //   行3 结果文字（PaintChrome 画的，不是子窗口）
    //
    // 标定区跟随右侧地图。
    // 单独给标题留一行，避免与输入框重叠。
    const int wcGap = 6;
    const int kTitleH = 22;
    // The recovery card owns the left column; keep calibration under the map.
    const int walkX = g_miniX, walkW = g_miniSize;
    const int cardTop = g_miniY + g_miniSize + kMapActionGap + kMapActionH + 8;
    Y_WALKCALL_TOP = cardTop;

    // 子窗口从标题行下面开始
    int wy = cardTop + kTitleH;

    {
        const int editH = 26;
        // 「取角色坐标」占 96px，两个输入框平分剩下的宽度
        const int useW = 96;
        const int editW = (walkW - useW - wcGap * 2) / 2;

        if (g_editWalkX) {
            SetWindowPos(g_editWalkX, nullptr, walkX, wy, editW, editH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (g_editWalkY) {
            SetWindowPos(g_editWalkY, nullptr, walkX + editW + wcGap, wy, editW, editH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (g_btnWalkUsePlayer.hwnd) {
            SetWindowPos(g_btnWalkUsePlayer.hwnd, nullptr,
                         walkX + walkW - useW, wy, useW, editH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        wy += editH + wcGap;

        if (g_btnWalkCall.hwnd) {
            SetWindowPos(g_btnWalkCall.hwnd, nullptr, walkX, wy, walkW, 36,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        wy += 36 + wcGap;

        // 结果文字占位（PaintChrome 在里面画两行，坐标从 Y_WALKCALL_TOP 算）
        wy += 56;
    }

    Y_WALKCALL_END = wy;

    // 日志使用恢复卡下面的剩余空间。
    Y_LOG_TOP = Y_RECOVERY_END + 6;
}

// ============================================================
// 后台任务：收图
// ============================================================
void DoRefreshMap()
{
    // 窗口已经销毁就别干活了：本函数会拉整张地图（最长等 30 秒），
    // 跑完还要 InvalidateRect 重绘 —— 那时 HWND 已经悬空。
    if (!g_running.load()) {
        return;
    }

    if (g_monitorIncompatible.load()) {
        SetStatus("Monitor.dll 接口不兼容", "请退出此游戏后重新启动，再从当前目录注入", 3);
        AddLog("刷新地图取消：DLL 接口不兼容，详见当前会话 navgui_log.txt");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_busy) return;
        g_busy = true;
        g_mapArea = {};
    }
    SetStatus("正在获取地图…", "确认 Monitor.dll 已注入、角色已在场景里", 1);

    // Reuse the fixed Data role; Tracker and Controller keep their own transports.
    // 先确认三条共享连接真的已经注册成功；否则 FetchCostBuffer 只会再等
    // 一轮超时，最后把 ControllerOwned 误报成“地图获取失败”。
    if (!pipe::StartManagedConnections(app::Config().PipeFullName(), 500)) {
        const auto connection = pipe::GetConnectionSnapshot();
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_busy = false;
        }
        if (connection.controllerError == managed::Error::ControllerOwned) {
            SetStatus("控制端被占用", "请关闭其他 NavGUI 后，再刷新地图", 3);
            AddLog("刷新地图取消：Controller 已被其他 NavGUI 占用");
        } else {
            SetStatus("通信未就绪", "Monitor 已加载，但当前 GUI 尚未拿到三条角色连接", 3);
            AddLog("刷新地图取消：Controller/Tracker/Data 未全部连接");
        }
        return;
    }
    const auto mapConnection = pipe::GetConnectionSnapshot();
    GameSession areaSession;
    poe::WorldAreaSnapshot areaBefore, areaAfter;
    std::string areaError;
    const bool haveAreaSession = InitializeSession(areaSession, &areaError);
    if (haveAreaSession)
        poe::ReadWorldArea(areaSession.memory, areaSession.moduleBase, areaBefore, areaError);
    mapdata::CostBuffer fresh;
    const int rc = usecase::FetchCostBuffer(fresh, "GUI");
    if (haveAreaSession)
        poe::ReadWorldArea(areaSession.memory, areaSession.moduleBase, areaAfter, areaError);
    const bool areaChanged = areaBefore.Available() && areaAfter.Available() && !areaBefore.SameMap(areaAfter);
    if (areaChanged) {
        { std::lock_guard<std::mutex> lk(g_mtx); g_busy = false; }
        SetStatus("读取期间地图已切换", "请在进入场景后重新刷新地图", 3);
        AddLog("地图读取取消：世界对象或 WorldAreas 记录已变化，丢弃本次数据");
        return;
    }

    if (rc == 0 && !fresh.Empty() && mapConnection.Same(pipe::GetConnectionSnapshot())) {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_cost = fresh;
            g_mapSessionId = mapConnection.sessionId;
            g_mapArea = areaBefore.SameMap(areaAfter) ? areaAfter : poe::WorldAreaSnapshot{};
        }
        BuildMapGray();

        nav::PixelMap g;
        size_t passable = 0;
        g.BuildFrom(fresh);
        passable = g.PassableCount();
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_grid = g; // A failed rebuild clears the previous map too.
        }


        char buf[224];
        sprintf_s(buf, "地图就绪：%u × %u，白色节点 %zu（纯像素，无腐蚀）",
                  fresh.Geo().WorldW(), fresh.Geo().WorldH(), passable);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_busy = false;
        }
        SetStatus("地图已就绪", "在地图上点一下选目标", 0);
        AddLog(buf);
        if (areaBefore.SameMap(areaAfter)) {
            AddLog("地图名称：" + areaAfter.name + "；地图 ID：" + areaAfter.id);
        } else {
            AddLog("地图数据已载入，名称 / ID 暂不可读：" + areaError);
        }
        if (MapHwnd()) InvalidateRect(MapHwnd(), nullptr, FALSE);
    }
    else {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_busy = false;
        }

        // 先看权威的三角色连接状态，避免把 Controller 被占用、会话变化
        // 和真正的地图采集失败都压成一个“获取地图失败”。
        const auto connection = pipe::GetConnectionSnapshot();
        if (connection.controllerError == managed::Error::ControllerOwned) {
            SetStatus("控制端被占用", "请关闭其他 NavGUI 后，再刷新地图", 3);
            AddLog("获取地图失败：Controller 已被其他 NavGUI 占用");
        } else if (!connection.ready) {
            SetStatus("通信未就绪", "Monitor 已加载，但当前 GUI 尚未拿到 Controller/Tracker/Data", 3);
            AddLog("获取地图失败：Controller/Tracker/Data 未全部连接");
        } else {
            SetStatus("获取地图失败", "游戏需在场景内且 Monitor.dll 能读取当前地图", 3);
            AddLog("获取地图失败：地图请求未返回完整数据");
        }
    }
}

// ============================================================
// 按当前页签摆放按钮
//
// 寻路页：单寻路测试、寻路与连招组合测试、两组设置入口。
// 背包页 / 周围页：底部中央放一个「扫描」按钮
// ============================================================
void MovePageButtons(HWND hwnd, int clientW, int clientH)
{
    recoveryui::ShowPanel(g_tab == 0);
    viewui::ShowPanel(g_tab == 0);
    follow::ShowPanel(g_tab == 4, {g_pad, kTabH + g_pad, clientW - g_pad, clientH - 38});
    packetui::ShowPanel(g_tab == 5, {g_pad, kTabH + g_pad, clientW - g_pad, clientH - 38});
    ritualui::ShowPanel(g_tab == 6, {g_pad, kTabH + g_pad, clientW - g_pad, clientH - 38});
    auto show = [](HWND h, bool on) {
        if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    };

    show(g_btnSkills.hwnd, g_tab == 3);
    show(g_btnCombo.hwnd, g_tab == 0);
    if (g_tab == 0) {
        RECT rc{}; GetClientRect(hwnd, &rc);
        MoveButtons(hwnd, g_pad, rc.right - g_pad * 2 - g_miniSize - g_pad,
                    g_pad + kTabH + 64 + 82);
    }
    if (g_tab == 3 && g_btnSkills.hwnd) {
        SetWindowPos(g_btnSkills.hwnd, nullptr, clientW - g_pad - 174,
                     kTabH + g_pad + 4, 164, 32, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // 地图只在寻路页显示
    if (MapHwnd()) ShowWindow(MapHwnd(), (g_tab == 0) ? SW_SHOW : SW_HIDE);

    if (g_tab == 0) {
        show(g_btnRefresh.hwnd, true);
        show(g_btnGo.hwnd, true);
        show(g_btnStop.hwnd, true);
        show(g_btnNavComboStart.hwnd, true);
        show(g_btnNavComboStop.hwnd, true);
        show(g_btnNavSettings.hwnd, true);
        // 走路 call 标定（寻路页常驻）
        show(g_editWalkX, true);
        show(g_editWalkY, true);
        show(g_btnWalkCall.hwnd, true);
        show(g_btnWalkUsePlayer.hwnd, true);
        show(g_btnInject.hwnd, true);   // 注入 DLL
        show(g_btnScan.hwnd, false);
        return;
    }

    show(g_btnRefresh.hwnd, false);
    show(g_btnGo.hwnd, false);
    show(g_btnStop.hwnd, false);
    show(g_btnNavComboStart.hwnd, false);
    show(g_btnNavComboStop.hwnd, false);
    show(g_btnNavSettings.hwnd, false);
    show(g_editWalkX, false);
    show(g_editWalkY, false);
    show(g_btnWalkCall.hwnd, false);
    show(g_btnWalkUsePlayer.hwnd, false);
    show(g_btnInject.hwnd, false);
    show(g_btnScan.hwnd, g_tab == 1 || g_tab == 2);

    // 扫描按钮放在统计栏右侧，不压地图（地图在寻路页才显示）
    const int bw = 140, bh = 32;
    const int bx = clientW - g_pad - bw - 420;   // 统计栏左侧，避开地图
    const int by = kTabH + g_pad + 4;
    if (g_btnScan.hwnd) {
        SetWindowPos(g_btnScan.hwnd, nullptr, bx, by, bw, bh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
// ============================================================
// 扫描：调 Plug.exe snapshot 子进程，解析它的控制台输出
//
// 为什么这样做（而不是在 NavGUI 里直连 OLA）：
//   · 遍历代码本来就能跑，Plug.exe snapshot 的输出已经验证过
//   · OLA 驱动同时在游戏里被 Monitor.dll 占着，NavGUI 再开一个
//     驱动实例会一直等设备，不返回
//   · 让 Plug.exe 自己跑一遍最省事，NavGUI 只负责解析 + 显示
//
// Plug.exe snapshot 的输出格式（见 entry.cpp 的 PrintInventory /
// PrintSurroundings）：
//
//   ===== Inventory =====
//   key:0x1F  entity:0x1234ABCD  品级:金装  name:混沌石 *3
//   ...
//   Total: 42 items
//
//   ===== Surroundings =====
//   ...（SnapshotToText 的两行格式）
//   Total: 210 entities
//
// 解析策略：按 "===== Inventory =====" / "===== Surroundings ====="
// 分段，背包行用 key:...品级:...name:... *N 提取，
// 周围实体行交给 SnapshotToText 的反向规则（kind + 名字 + 坐标）。
// 解析失败的行走 g_logs 里提示，不丢数据也不崩。
// ============================================================

// 跑一个命令并收它的 stdout（新版 Plug.exe 用 UTF-8 BOM；兼容旧版 ACP/GBK）
//   wantTotals : 收够几条 "Total:" 就算完事（inventory/around 各 1 条，
//                一次扫两个才需要 2）。超时/挂死一律 TerminateProcess。
bool RunCapture(const std::string& cmdLine, std::string& outUtf8, int wantTotals)
{
    outUtf8.clear();

    // 用匿名管道接 stdout
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmdLine;

    const BOOL ok = CreateProcessA(
        nullptr, mutableCmd.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);

    CloseHandle(wr);
    wr = nullptr;

    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    // 读输出。★ Plug.exe 内部的 OLA 注册可能永久卡死（不返回也不报错），
    //   所以超时放在这里：时间一到直接 TerminateProcess，不指望它自己退。
    std::string raw;
    char buf[4096];
    DWORD got = 0;
    bool sawData = false;
    const ULONGLONG deadline = GetTickCount64() + 25000ULL;

    while (GetTickCount64() < deadline) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) {
            if (avail == 0) {
                if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
                continue;
            }
        }

        got = 0;
        if (!ReadFile(rd, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) {
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
            continue;
        }
        buf[got] = '\0';
        raw += buf;
        sawData = true;

        size_t n = 0, pos = 0;
        while ((pos = raw.find("Total:", pos)) != std::string::npos) { ++n; pos += 6; }
        if ((int)n >= wantTotals) break;
    }

    CloseHandle(rd);
    // 不管正常结束还是超时，一律强制终止（挂死的子进程不会自己退）
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!sawData) {
        outUtf8.clear();
        return true;   // 没拿到数据，交给上层报"没有输出"
    }

    // 新版输出优先用 BOM 明确标识 UTF-8；旧版无 BOM 时兼容严格 UTF-8、
    // 当前 Windows ACP，最后回退到 GBK(936)。
    auto validUtf8 = [](const std::string& bytes) {
        return !bytes.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.data(), (int)bytes.size(), nullptr, 0) > 0;
    };
    auto toUtf8 = [](UINT cp, const std::string& bytes) {
        std::string result;
        int wn = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (wn <= 0) return result;
        std::wstring w((size_t)wn, L'\0');
        MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), &w[0], wn);
        int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
        if (un <= 0) return result;
        result.resize((size_t)un);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &result[0], un, nullptr, nullptr);
        return result;
    };
    if (!raw.empty()) {
        if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            outUtf8.assign(raw.data() + 3, raw.size() - 3);
        }
        else if (validUtf8(raw)) outUtf8 = raw;
        else {
            outUtf8 = toUtf8(GetACP(), raw);
            if (outUtf8.empty()) outUtf8 = toUtf8(936, raw);
            if (outUtf8.empty()) outUtf8 = raw;
        }
    }
    return true;
}

// 把插件目录找出来（Plug.exe 和 NavGUI.exe 同目录）
std::string ModuleDir()
{
    char mod[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, mod, MAX_PATH);
    std::string full(mod);
    const size_t p = full.find_last_of("\\/");
    return (p == std::string::npos) ? std::string() : full.substr(0, p + 1);
}

std::wstring ModuleDirW()
{
    wchar_t mod[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, mod, (DWORD)_countof(mod));
    if (!length || length >= _countof(mod)) return std::wstring();
    std::wstring full(mod, length);
    const size_t p = full.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring() : full.substr(0, p + 1);
}

struct NavUiField { const char* key; const wchar_t* label; int minValue; int maxValue; };
const NavUiField kNavUiFields[14] = {
    {"stepDistance", L"下发距离", 1, 1000},
    {"arriveTolerance", L"到达容差", 1, 1000},
    {"waypointTolerance", L"路点容差", 1, 30},
    {"pollMs", L"位置采样(ms)", 20, 1000},
    {"leadMs", L"提前衔接(ms)", 0, 1000},
    {"minCallMs", L"调用间隔(ms)", 100, 3000},
    {"startupMs", L"起步宽限(ms)", 100, 5000},
    {"stepWaitMs", L"单步等待(ms)", 300, 30000},
    {"stillSamples", L"静止采样次数", 2, 20},
    {"stallLimit", L"卡住恢复次数", 2, 20},
    {"deviateDistance", L"偏离阈值", 1, 1000},
    {"positionRetries", L"坐标重试次数", 1, 5},
    {"recoveryLimit", L"恢复总次数", 1, 100},
    {"replanMs", L"规划失败重试(ms)", 150, 30000},
};

std::string NavConfigPath()
{
    return app::Config().RuntimeFile("config.ini");
}

void SaveNavUiField(int index)
{
    if (!g_navSettingsReady || index < 0 || index >= 14 || !g_navSettingEdits[index]) return;
    wchar_t text[64] = {};
    GetWindowTextW(g_navSettingEdits[index], text, 64);
    wchar_t* end = nullptr;
    long value = wcstol(text, &end, 10);
    if (end == text || *end != L'\0') return;
    const auto& f = kNavUiFields[index];
    if (value < f.minValue || value > f.maxValue) return;
    char buf[32] = {};
    sprintf_s(buf, "%ld", value);
    WritePrivateProfileStringA("nav", f.key, buf, NavConfigPath().c_str());
}

void SaveAllNavUiFields()
{
    for (int i = 0; i < 14; ++i) SaveNavUiField(i);
}

HFONT g_navTitleFont = nullptr, g_navUiFont = nullptr, g_navHintFont = nullptr;

void LayoutNavSettings(HWND hwnd)
{
    RECT rect{}; GetClientRect(hwnd, &rect);
    const int cardWidth = (rect.right - 48) / 2;
    const int rowHeight = (rect.bottom - 252) / 7;
    for (int i = 0; i < 14; ++i) {
        const int left = 16 + (i / 7) * (cardWidth + 16);
        const int top = 160 + (i % 7) * rowHeight;
        MoveWindow(g_navSettingEdits[i], left + cardWidth - 110, top + 4, 94, 32, TRUE);
    }
    MoveWindow(GetDlgItem(hwnd, 2499), rect.right - 168, rect.bottom - 70, 140, 34, TRUE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK NavSettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT themed = 0;
    if (ui::HandleMessage(hwnd, msg, wp, lp, themed)) return themed;
    switch (msg) {
    case WM_CREATE: {
        ui::ApplyWindow(hwnd);
        g_navTitleFont = ui::Font(22, FW_SEMIBOLD);
        g_navUiFont = ui::Font(14);
        g_navHintFont = ui::Font(12);
        usecase::NavigateOptions opt;
        usecase::LoadNavOptionsFromIni(opt);
        for (int i = 0; i < 14; ++i) {
            HWND edit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                0, 0, 94, 32, hwnd, (HMENU)(INT_PTR)(2400 + i), nullptr, nullptr);
            g_navSettingEdits[i] = edit;
            ui::StyleControl(edit, g_navUiFont);
            SendMessageW(edit, EM_SETLIMITTEXT, 5, 0);
            wchar_t value[32] = {};
            int v = 0;
            switch (i) {
            case 0: v=opt.stepDistance; break; case 1: v=opt.arriveTolerance; break;
            case 2: v=opt.waypointTolerance; break; case 3: v=opt.pollMs; break;
            case 4: v=opt.leadMs; break; case 5: v=opt.minCallMs; break;
            case 6: v=opt.startupMs; break; case 7: v=opt.stepWaitMs; break;
            case 8: v=opt.stillSamples; break; case 9: v=opt.stallLimit; break;
            case 10: v=opt.deviateDistance; break; case 11: v=opt.positionRetries; break;
            case 12: v=opt.recoveryLimit; break; case 13: v=opt.replanMs; break;
            }
            swprintf_s(value, L"%d", v);
            SetWindowTextW(edit, value);
        }
        HWND close = CreateWindowExW(0, L"BUTTON", L"保存并关闭",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 140, 34,
            hwnd, (HMENU)2499, nullptr, nullptr);
        ui::StyleControl(close, g_navUiFont, ui::ButtonStyle::Primary);
        g_navSettingsReady = true;
        LayoutNavSettings(hwnd);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {780, 680}; return 0;
    case WM_SIZE:
        if (g_navSettingsReady) LayoutNavSettings(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{}; GetClientRect(hwnd, &rect); FillRect(dc, &rect, ui::Background());
        ui::Text(dc, L"寻路设置", {28, 18, rect.right - 28, 52}, g_navTitleFont);
        ui::Text(dc, L"调整移动衔接、位置检测与卡住恢复；有效修改自动保存。",
                 {28, 60, rect.right - 28, 84}, g_navUiFont, ui::color::kTextDim);
        const int cardWidth = (rect.right - 48) / 2;
        const int rowHeight = (rect.bottom - 252) / 7;
        for (int col = 0; col < 2; ++col) {
            const int left = 16 + col * (cardWidth + 16);
            ui::Card(dc, {left, 104, left + cardWidth, rect.bottom - 94});
            ui::Text(dc, col == 0 ? L"移动与衔接" : L"检测与恢复",
                     {left + 16, 120, left + cardWidth - 16, 144}, g_navUiFont, ui::color::kAccent);
        }
        for (int i = 0; i < 14; ++i) {
            const int left = 32 + (i / 7) * (cardWidth + 16);
            const int top = 160 + (i % 7) * rowHeight;
            ui::Text(dc, kNavUiFields[i].label, {left, top, left + cardWidth - 140, top + 23}, g_navUiFont);
            wchar_t range[64]{};
            swprintf_s(range, L"范围 %d – %d", kNavUiFields[i].minValue, kNavUiFields[i].maxValue);
            ui::Text(dc, range, {left, top + 24, left + cardWidth - 140, top + 42}, g_navHintFont, ui::color::kTextDim);
        }
        ui::Text(dc, L"修改立即写入 config.ini\n范围外的输入不会保存。",
                 {28, rect.bottom - 76, rect.right - 196, rect.bottom - 24}, g_navUiFont,
                 ui::color::kTextDim, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) >= 2400 && LOWORD(wp) < 2414 && HIWORD(wp) == EN_CHANGE) {
            SaveNavUiField(int(LOWORD(wp) - 2400));
            return 0;
        }
        if (LOWORD(wp) == 2499 || LOWORD(wp) == IDCANCEL) {
            SaveAllNavUiFields();
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        SaveAllNavUiFields();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_navSettingsReady = false;
        for (auto& h : g_navSettingEdits) h = nullptr;
        g_navSettingsWnd = nullptr;
        return 0;
    case WM_NCDESTROY:
        DeleteObject(g_navTitleFont); g_navTitleFont = nullptr;
        DeleteObject(g_navUiFont); g_navUiFont = nullptr;
        DeleteObject(g_navHintFont); g_navHintFont = nullptr;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowNavSettings(HWND owner)
{
    if (g_navSettingsWnd) { ShowWindow(g_navSettingsWnd, SW_SHOWNORMAL); SetForegroundWindow(g_navSettingsWnd); return; }
    HINSTANCE inst = (HINSTANCE)GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.hInstance=inst; wc.lpfnWndProc=NavSettingsProc;
    wc.hCursor=LoadCursorW(nullptr,(LPCWSTR)IDC_ARROW); wc.hbrBackground=ui::Background();
    wc.hIcon=ui::AppIcon(); wc.hIconSm=ui::AppIcon(true);
    wc.lpszClassName=L"Poe2NavSettings";
    RegisterClassExW(&wc);
    g_navSettingsWnd=CreateWindowExW(WS_EX_CONTROLPARENT, L"Poe2NavSettings", L"寻路设置",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 800, 700,
        owner, nullptr, inst, nullptr);
    if (!g_navSettingsWnd) return;
    ui::CenterWindow(g_navSettingsWnd,owner);
    ShowWindow(g_navSettingsWnd,SW_SHOW); UpdateWindow(g_navSettingsWnd);
}

// ============================================================
// 扫描：调 Plug.exe 子进程读内存，解析它的控制台输出
//
// 为什么走子进程（而不是 NavGUI 里直接读）：
//   · 遍历代码本来就能跑，Plug.exe 的输出已验证过
//   · 不用在本进程里再开一份内存访问
//
// 两个模式分开：inventory 只读背包，around 只读周围，
// 各自独立，互不拖累（周围实体多的时候扫描更慢）。
// ============================================================

// 解析背包段
void ParseInventory(const std::string& text, std::vector<poe::InventoryItem>& inv)
{
    bool inInv = false;
    poe::InventoryItem pending;
    bool hasPending = false;

    std::istringstream ss(text);
    std::string ln;
    while (std::getline(ss, ln)) {
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        if (ln.empty()) continue;

        if (ln.rfind("===== Inventory", 0) == 0) { inInv = true; continue; }
        if (ln.rfind("=====", 0) == 0)            { inInv = false; continue; }
        if (!inInv) continue;
        if (ln.rfind("Total:", 0) == 0) continue;

        if (ln.find("key:0x") != std::string::npos) {
            if (hasPending) { inv.push_back(pending); }
            pending = poe::InventoryItem();
            hasPending = true;

            const size_t kp = ln.find("key:0x");
            pending.key = (uint8_t)strtoul(ln.c_str() + kp + 6, nullptr, 16);

            const size_t ep = ln.find("entity:0x");
            if (ep != std::string::npos) {
                pending.entity = strtoull(ln.c_str() + ep + 9, nullptr, 16);
            }

            const size_t gp = ln.find("品级:");
            if (gp != std::string::npos) {
                const size_t vs = gp + 7;   // "品级:" UTF-8 = 7 字节
                size_t ve = ln.find_first_of(" \t", vs);
                if (ve == std::string::npos) ve = ln.size();
                const std::string g = ln.substr(vs, ve - vs);
                if (g == "白装")      pending.grade = 0;
                else if (g == "蓝装") pending.grade = 1;
                else if (g == "金装") pending.grade = 2;
                else if (g == "暗金") pending.grade = 3;
                else                  pending.grade = -1;
            }

            const size_t np = ln.find("name:");
            if (np != std::string::npos) {
                pending.name = ln.substr(np + 5);
            }
        }
        else if (hasPending) {
            std::string body = ln;
            if (body.rfind("name:", 0) == 0) body = body.substr(5);

            const size_t star = body.rfind(" *");
            if (star != std::string::npos) {
                bool digits = true;
                for (size_t k = star + 2; k < body.size(); ++k) {
                    if (body[k] < '0' || body[k] > '9') { digits = false; break; }
                }
                if (digits) {
                    pending.num = (uint32_t)strtoul(body.c_str() + star + 2, nullptr, 10);
                    body = body.substr(0, star);
                }
            }
            if (!body.empty()) {
                if (!pending.name.empty()) pending.name += " ";
                pending.name += body;
            }
        }
    }
    if (hasPending) inv.push_back(pending);
}

// 解析周围段
void ParseAround(const std::string& text, std::vector<poe::EntitySnapshot>& around)
{
    bool inAround = false;
    std::string pendKind, pendMeta;

    auto kindOf = [](const std::string& k) {
        if (k == "怪物")        return poe::EntityKind::Monster;
        if (k == "玩家")        return poe::EntityKind::Player;
        if (k == "传送门")      return poe::EntityKind::Portal;
        if (k == "掉落物")      return poe::EntityKind::WorldItem;
        if (k == "NPC")         return poe::EntityKind::NPC;
        if (k == "藏身处物件")  return poe::EntityKind::Hideout;
        if (k == "记录点")      return poe::EntityKind::CheckPoint;
        if (k == "水井")        return poe::EntityKind::WaterWell;
        if (k == "地图特殊箱子") return poe::EntityKind::Chest;
        if (k == "公会仓库")    return poe::EntityKind::GuildStash;
        if (k == "仓库")        return poe::EntityKind::Stash;
        return poe::EntityKind::None;
    };

    std::istringstream ss(text);
    std::string ln;
    while (std::getline(ss, ln)) {
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        if (ln.empty()) continue;

        if (ln.rfind("===== Surroundings", 0) == 0) { inAround = true; continue; }
        if (ln.rfind("=====", 0) == 0)              { inAround = false; continue; }
        if (!inAround) continue;
        if (ln.rfind("Total:", 0) == 0) continue;

        if (!ln.empty() && ln[0] == '[') {
            const size_t close = ln.find(']');
            if (close != std::string::npos) {
                pendKind = ln.substr(1, close - 1);
                pendMeta = ln;
            }
            continue;
        }

        if (ln.rfind("name:", 0) == 0 && !pendKind.empty()) {
            poe::EntitySnapshot s;
            s.kind = kindOf(pendKind);

            const size_t kp = pendMeta.find("key:0x");
            if (kp != std::string::npos) {
                s.key = (uint32_t)strtoul(pendMeta.c_str() + kp + 6, nullptr, 16);
            }
            const size_t ep = pendMeta.find("entity:0x");
            if (ep != std::string::npos) {
                s.entity = strtoull(pendMeta.c_str() + ep + 9, nullptr, 16);
            }

            std::string body = ln.substr(5);
            const size_t hpP = body.find(" hp:");
            const size_t posP = body.find(" pos:[");

            if (posP != std::string::npos) {
                std::string coord = body.substr(posP + 6);
                const size_t comma = coord.find(',');
                if (comma != std::string::npos) {
                    s.x = atoi(coord.c_str());
                    s.y = atoi(coord.c_str() + comma + 1);
                }
                body = body.substr(0, posP);
            }
            if (hpP != std::string::npos && (posP == std::string::npos || hpP < posP)) {
                s.hp = (uint32_t)strtoul(body.c_str() + hpP + 4, nullptr, 10);
                body = body.substr(0, hpP);
            }
            s.name = body;

            around.push_back(s);
            pendKind.clear();
            pendMeta.clear();
        }
    }
}

// ---- 只扫背包 ----
void DoScanInventory()
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_busy) return;
        g_busy = true;
    }
    SetStatus("正在扫描背包…", "调用 Plug.exe inventory", 1);
    AddLog("扫描背包…");

    const auto& target = app::Config();
    const std::string cmd = "\"" + ModuleDir() + "Plug.exe\" inventory -pid=" +
        std::to_string(target.targetPid) + " -start=" + std::to_string(target.targetStart);
    std::string text;
    std::string err;

    if (!RunCapture(cmd, text, 1 /* 只要一条 Total: */) || text.empty()) {
        err = "Plug.exe 没有输出";
    }
    else {
        std::vector<poe::InventoryItem> inv;
        ParseInventory(text, inv);

        // ★ 锁只包数据赋值：SetStatus/AddLog 内部会再拿 g_mtx，
        //   放在锁里会自死锁（同一线程重复 lock）
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_inv = inv;
            g_haveInv = true;
            g_scanErr = err;
            g_invScroll = 0;
            g_busy = false;
        }

        char buf[128];
        sprintf_s(buf, "背包：%zu 件", inv.size());
        SetStatus("背包已更新", buf, 2);
        AddLog(buf);
        if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_haveInv = true;
        g_scanErr = err;
        g_busy = false;
    }
    SetStatus("背包扫描失败", err, 3);
    AddLog("背包扫描失败：" + err);
    if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
}

// ---- 只扫周围 ----
void DoScanAround()
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_busy) return;
        g_busy = true;
    }
    SetStatus("正在扫描周围…", "调用 Plug.exe around", 1);
    AddLog("扫描周围…");

    const auto& target = app::Config();
    const std::string cmd = "\"" + ModuleDir() + "Plug.exe\" around -pid=" +
        std::to_string(target.targetPid) + " -start=" + std::to_string(target.targetStart);
    std::string text;
    std::string err;

    if (!RunCapture(cmd, text, 1 /* 只要一条 Total: */) || text.empty()) {
        err = "Plug.exe 没有输出";
    }
    else {
        std::vector<poe::EntitySnapshot> around;
        ParseAround(text, around);

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_around = around;
            g_haveAround = true;
            g_scanErr = err;
            g_aroundScroll = 0;
            g_busy = false;
        }

        char buf[128];
        sprintf_s(buf, "周围：%zu 个实体", around.size());
        SetStatus("周围已更新", buf, 2);
        AddLog(buf);
        if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_haveAround = true;
        g_scanErr = err;
        g_busy = false;
    }
    SetStatus("周围扫描失败", err, 3);
    AddLog("周围扫描失败：" + err);
    if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
}

// ============================================================
// 把当前页数据生成文本
//
// 格式按 TSV（制表符分隔）：
//   · 粘到 Excel / WPS 会自动分列
//   · 粘到记事本也能看
// ============================================================
// Dedicated, joined worker: only snapshot state crosses the thread boundary.
void StartSkillSlotsScan()
{
    if (!g_running || g_skillBusy.exchange(true)) return;
    if (g_skillThread.joinable()) g_skillThread.join();
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_skills = {};
        g_haveSkills = false;
        g_skillErr.clear();
        g_skillUpdated.clear();
        g_skillScroll = 0;
    }
    try {
        g_skillThread = std::thread([] {
            poe::SkillSlotsSnapshot snapshot;
            std::string error;
            bool ok = false;
            try {
                ok = usecase::ScanSkillSlots(snapshot, error);
            } catch (const std::exception& e) {
                error = std::string("获取技能槽失败：") + e.what();
            } catch (...) {
                error = "获取技能槽失败：未知异常";
            }
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t stamp[64];
            swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u",
                       now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                g_skills = ok ? std::move(snapshot) : poe::SkillSlotsSnapshot{};
                g_haveSkills = true;
                g_skillErr = ok ? std::string{} : error;
                g_skillUpdated = ok ? stamp : L"";
            }
            g_skillBusy = false;
            g_skillDirty = true;
        });
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_skillErr = std::string("无法启动读取线程：") + e.what();
        g_haveSkills = true;
        g_skillBusy = false;
        g_skillDirty = true;
    }
}

std::string BuildPageText()
{
    std::string out;
    char line[1024];

    std::lock_guard<std::mutex> lk(g_mtx);

    if (g_tab == 1) {
        // ---- 背包 ----
        out = "格\t品级\t名称\t数量\tentity\n";
        for (const auto& it : g_inv) {
            std::string name = it.prefix;
            name += it.name;
            sprintf_s(line, sizeof(line), "0x%X\t%s\t%s\t%u\t0x%llX\n",
                      (unsigned)it.key,
                      poe::GradeName(it.grade),
                      name.c_str(),
                      it.num,
                      (unsigned long long)it.entity);
            out += line;
        }
    }
    else if (g_tab == 2) {
        // ---- 周围 ----
        out = "类型\t名称\tX\tY\tHP\tentity\n";
        for (const auto& s : g_around) {
            const char* kind = "其它";
            switch (s.kind) {
            case poe::EntityKind::Monster:    kind = "怪物";   break;
            case poe::EntityKind::Player:     kind = "玩家";   break;
            case poe::EntityKind::Portal:     kind = "传送门"; break;
            case poe::EntityKind::WorldItem:  kind = "掉落";   break;
            case poe::EntityKind::NPC:        kind = "NPC";    break;
            case poe::EntityKind::Chest:      kind = "箱子";   break;
            case poe::EntityKind::Stash:      kind = "仓库";   break;
            case poe::EntityKind::GuildStash: kind = "公会仓"; break;
            case poe::EntityKind::Hideout:    kind = "藏身处"; break;
            case poe::EntityKind::CheckPoint: kind = "存档点"; break;
            case poe::EntityKind::WaterWell:  kind = "水井";   break;
            default: break;
            }
            sprintf_s(line, sizeof(line), "%s\t%s\t%d\t%d\t%u\t0x%llX\n",
                      kind, s.name.c_str(), (int)s.x, (int)s.y, s.hp,
                      (unsigned long long)s.entity);
            out += line;
        }
    }
    else if (g_tab == 3) {
        if (g_skillBusy) return "技能槽正在读取，请稍后复制。\n";
        if (!g_skillErr.empty()) return "技能槽读取失败：" + g_skillErr + "\n";
        if (!g_haveSkills) return "尚未获取技能槽。\n";
        return poe::SkillSlotsToTsv(g_skills);
    }
    else {
        // ---- 寻路页：状态 + 日志 ----
        out = "== 状态 ==\n";
        out += g_status + "\n";
        out += (g_detail.empty() ? std::string() : (g_detail + "\n"));
        sprintf_s(line, sizeof(line), "角色 (%d,%d)  目标 (%d,%d)\n",
                  g_playerX.load(), g_playerY.load(), g_goalX, g_goalY);
        out += line;
        sprintf_s(line, sizeof(line), "路径 %zu 点  地图 %u x %u\n",
                  g_path.size(), g_mapW, g_mapH);
        out += line;
        out += "\n== 日志 ==\n";
        for (const auto& s : g_logs) {
            out += s;
            out += "\n";
        }
    }
    return out;
}

// 复制到剪贴板（UTF-8 -> 宽字符）
bool CopyToClipboard(HWND owner, const std::string& utf8)
{
    if (utf8.empty()) return false;

    // UTF-8 -> UTF-16
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], wn);

    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return false;
    }
    memcpy(p, w.c_str(), bytes);
    GlobalUnlock(h);
    if (!OpenClipboard(owner)) { GlobalFree(h); return false; }
    const bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(h); // Only a successful SetClipboardData transfers ownership.
    return ok;
}

// 导出到文件（UTF-8 带 BOM，Excel 打开中文不乱码）
bool ExportToFile(const std::string& utf8, const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    DWORD wrote = 0;
    WriteFile(f, "\xEF\xBB\xBF", 3, &wrote, nullptr);
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
    CloseHandle(f);
    return true;
}

// ★ 算路代次号：每次点地图 +1。
//   DoPlanPath 跑在 detach 出去的线程里，一次 A* + 插值可能要几百毫秒。
//   快速连点两下地图时，先发起的慢任务可能后完成，把**旧目标**的路径
//   和 g_goalX/g_goalY 覆盖掉新点击 —— 用户看到点了 B 点，图上却画回
//   A 点的路，启动导航测试后角色跑向 A。落盘前比对代次，过期的直接丢弃。
std::atomic<uint32_t> g_planEpoch{ 0 };

// ============================================================
// 后台任务：算路预览
// ============================================================
void DoPlanPath(int32_t goalWX, int32_t goalWY, uint32_t myEpoch)
{
    const int32_t curX = g_playerX.load();
    const int32_t curY = g_playerY.load();

    usecase::NavigateOptions opt;
    // 路点闭环参数从 config.ini 的 [nav] 段读取；保留当前 WalkCall。
    usecase::LoadNavOptionsFromIni(opt);

    std::vector<NavBatchChunk> pts;
    int32_t gwx = goalWX, gwy = goalWY;
    std::string err;

    // ★ 这个函数跑在 detach 出去的线程里，必须自己持有一份数据快照。
    //
    //   原实现：gridCopy 拷贝了，但 PlanPath 的第一个参数直接传了全局
    //   g_cost（引用，锁在这里已经释放）。而 UI 线程的 DoRefreshMap 会
    //   整体赋值 g_cost（内部 vector 换块），算路线程还在遍历旧指针 ——
    //   典型的数据竞争 + 悬垂引用，表现为偶发算路失败或读到半新半旧的
    //   地图，极端情况越界读。
    //
    //   DoWalk（下面 DoWalk 开头）早就是这么修的，这里保持一致。
    nav::PixelMap gridCopy;
    mapdata::CostBuffer costCopy;
    bool gridEmpty = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        gridCopy = g_grid;
        costCopy = g_cost;
        gridEmpty = g_grid.Empty();
    }

    LOGI("[GUI] DoPlanPath 目标(%d,%d) 角色(%d,%d) 白色节点图空=%d 代价空=%d",
         goalWX, goalWY, curX, curY, (int)gridEmpty, (int)costCopy.Empty());

    // 地图还没拉到就直接收工：没有代价表算不出任何路，
    // 继续往下走只会拿到一条空路径 + 一个没意义的错误串。
    if (costCopy.Empty() || gridEmpty) {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_path.clear();
            g_hasGoal = false;
        }
        SetStatus("算路失败", "地图数据还没准备好，先刷新地图", 3);
        AddLog("算路取消：地图数据为空");
        if (MapHwnd()) InvalidateRect(MapHwnd(), nullptr, FALSE);
        if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
        return;
    }

    const bool ok = usecase::PlanPath(costCopy, &gridCopy,
                                      curX, curY,
                                      goalWX, goalWY, opt,
                                      pts, gwx, gwy, err);

    // ★ 落盘前校验代次：算路期间用户又点了一次地图，本次结果已过期。
    //   直接丢弃，否则会把新目标的路径/终点覆盖回旧的。
    if (myEpoch != g_planEpoch.load()) {
        LOGI("[GUI] DoPlanPath 目标(%d,%d) 结果已过期（epoch %u != %u），丢弃",
             goalWX, goalWY, myEpoch, g_planEpoch.load());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (ok) {
            g_path = pts;
            g_goalX = gwx;
            g_goalY = gwy;
            g_hasGoal = true;

        }
        else {
            g_path.clear();
            g_hasGoal = false;
        }
    }

    if (MapHwnd()) InvalidateRect(MapHwnd(), nullptr, FALSE);
    if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);

    if (ok) {
        char buf[192];
        sprintf_s(buf, "路径就绪：%zu 个航点", pts.size());
        SetStatus("路径已算出", "选择「单寻路测试」或「寻路+技能循环测试」", 0);
        AddLog(buf);
    }
    else {
        SetStatus("算路失败", err, 3);
        AddLog("算路失败：" + err);
    }
}

// ============================================================
// 后台任务：行走
// ============================================================
// 自绘按钮改文字的前向声明；状态按钮共用。
void SetButtonText(Button& b, const wchar_t* text);

// ============================================================
// 注入 Monitor.dll（GUI 内置，不依赖外部 inject_monitor.exe）
//
// 原理：远程线程注入
//   1) 按 config.ini 的 [game] windowTitle 找游戏窗口 → 拿 pid
//   2) OpenProcess 要 CREATE_THREAD | VM_OPERATION | VM_WRITE | VM_READ | QUERY
//   3) VirtualAllocEx 在游戏进程里分配一块 RW 内存，把 DLL 的**绝对路径**写进去
//   4) CreateRemoteThread(LoadLibraryW, remote)
//   5) GetExitCodeThread 拿 HMODULE，非 0 就是注入成功
//
// ★ 必须是绝对路径：LoadLibraryW 是在游戏进程里执行的，
//   它的当前目录是游戏安装目录，相对路径根本解析不到。
// ★ 必须先备份游戏存档再折腾？不需要 —— 这里只 LoadLibrary，不改数据。
// ============================================================

// 找游戏窗口 → pid。找不到返回 0。
static DWORD FindGamePid(std::wstring& winTitleOut)
{
    const app::AppConfig& ac = app::Config();
    if (ac.targetPid) {
        HWND window = ac.TargetWindow();
        wchar_t title[1024]{};
        if (window) GetWindowTextW(window, title, int(_countof(title)));
        winTitleOut = title;
        return window ? ac.targetPid : 0;
    }
    winTitleOut = std::wstring(ac.gameWindowTitle.begin(), ac.gameWindowTitle.end());

    // 窗口标题必须来自 config.ini [game] windowTitle —— 架构守卫禁止硬编码
    // 游戏标题（补丁/语言一变就失效，且散落在多处的标题迟早互相矛盾）。
    // 没配标题时不猜，直接走下面的类名兜底。
    HWND hGame = nullptr;
    if (!winTitleOut.empty()) {
        hGame = FindWindowW(nullptr, winTitleOut.c_str());
    }
    if (!hGame) {
        // 退一步：按类名找（POE2 的窗口类名固定，标题会被语言/补丁改）
        hGame = FindWindowW(L"POEWindowClass", nullptr);
    }
    if (!hGame) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(hGame, &pid);
    return pid;
}

// Return the mapped module, never infer its build from its current disk file.
static uint64_t FindRemoteModule(HANDLE hProc, DWORD pid, const wchar_t* dllFileName)
{
    (void)hProc;
    const auto module = monitorimage::Find(pid, dllFileName);
    return !module.error && module.matches == 1 ? module.base : 0;
}

// Log full identities only when they change. Startup failures happen before
// the main window exists, so AddLog alone would lose the useful evidence.
static void RecordMonitorDiagnostic(DWORD pid, const std::string& diagnostic)
{
    static std::mutex mutex;
    static std::string previous;
    const std::string entry = "PID=" + std::to_string(pid) + " " + diagnostic;
    std::lock_guard<std::mutex> lock(mutex);
    if (entry == previous) return;
    previous = entry;
    std::istringstream lines(entry);
    std::string line;
    while (std::getline(lines, line)) LOGI("[DLL校验] %s", line.c_str());
}

// 远程调一个"单参数" kernel32 导出（LoadLibraryW / FreeLibrary 都是这种形态）
//   成功返回 true，*exitCode 是远程线程返回值
static bool RemoteCallOne(HANDLE hProc, FARPROC fn, void* arg, DWORD waitMs, DWORD* exitCode)
{
    if (exitCode) *exitCode = 0;
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)fn, arg, 0, nullptr);
    if (!hThread) return false;

    const DWORD w = WaitForSingleObject(hThread, waitMs);
    bool ok = false;
    if (w == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeThread(hThread, &code);
        if (exitCode) *exitCode = code;
        ok = true;
    }
    CloseHandle(hThread);
    return ok;
}

// ============================================================
// 注入状态探测（启动时 + 运行期定时）
//
// 需求：程序一开就自己判断 Monitor.dll 有没有注入，据此决定按钮能不能点。
//
// 探测方法（只读，不碰游戏数据）：
//   1) FindGamePid()            游戏在不在
//   2) FindRemoteModule()       Toolhelp32 枚举游戏进程模块，找 Monitor.dll
//   3) WaitNamedPipe(管道名, 0)  Monitor 的管道服务在不在
//
// 为什么要第 3 步：
//   Monitor.dll 有可能"加载了但没跑起来"（DllMain 初始化失败、被反作弊拦了
//   一部分、或者 DLL 是旧版本）。这种状态下按钮要是也变暗，用户就彻底
//   没有恢复手段了。所以把它单独做成一个状态，允许重新注入。
// ============================================================
enum class InjectUiState {
    Unknown,        // 探测失败（权限不足等）：保持可点，让用户自己试
    NoGame,         // 游戏没运行
    NotInjected,    // 游戏在跑，Monitor.dll 没加载  → 可点
    Injected,       // 已加载 + 管道可用            → 变暗不可点
    LoadedNoPipe,   // 已加载但管道没起              → 可点（重新注入）
    ControllerBusy, // 管道可用，但 Controller 已被其他 GUI 占用
    ManagedWait,    // 管道可用，当前 GUI 正在等待三条角色连接完成
    Incompatible,   // DLL 接口不兼容，或旧 DLL 无法证明是同一精确构建
    VersionUnknown, // 无法读取接口或构建标识，不等同于确定不兼容
};

// 当前状态只由 UI 线程读写，不需要原子
InjectUiState g_injectUi = InjectUiState::Unknown;
int           g_injectProbeTick = 0;      // 定时探测的节流计数（WM_TIMER 计数）

// Monitor 的管道是否有人在监听。
//   用 WaitNamedPipeW(名字, 0)：只查有没有服务实例可用，不建句柄，
//   不会占用 Monitor 那 4 个实例池槽位，绝对没有副作用。
static bool ProbePipeAlive()
{
    const std::wstring full = app::Config().PipeFullName();
    if (full.empty()) return false;
    return WaitNamedPipeW(full.c_str(), 0) != FALSE;
}

// 一次完整探测。detail 回传人类可读的补充说明。
static InjectUiState ProbeInjection(std::string& detail)
{
    detail.clear();

    std::wstring title;
    const DWORD pid = FindGamePid(title);
    if (!pid) {
        detail = "游戏没运行";
        return InjectUiState::NoGame;
    }

    const auto module = monitorimage::Find(pid, L"Monitor.dll");
    if (module.error || module.matches > 1) {
        detail = "无法枚举唯一的 Monitor.dll（stage=enumerate-modules err=" + std::to_string(module.error) +
                 " matches=" + std::to_string(module.matches) + "）；详情见当前会话 navgui_log.txt";
        RecordMonitorDiagnostic(pid, detail);
        return InjectUiState::VersionUnknown;
    }
    const uint64_t base = module.base;
    if (!base) {
        char b[96];
        sprintf_s(b, "游戏 pid=%lu，Monitor.dll 未注入", (unsigned long)pid);
        detail = b;
        return InjectUiState::NotInjected;
    }

    const std::wstring localDll = ModuleDirW() + L"Monitor.dll";
    const auto local = monitorimage::ReadLocal(localDll);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        monitorimage::Identity remote;
        remote.path = module.path;
        remote.base = module.base;
        remote.error = GetLastError();
        remote.stage = "open-remote-process";
        detail = "Monitor.dll 已加载，但无法验证接口（stage=" + remote.stage +
                 " err=" + std::to_string(remote.error) + "）；详情见当前会话 navgui_log.txt";
        RecordMonitorDiagnostic(pid, monitorimage::Compare(local, remote).diagnostic);
        return InjectUiState::VersionUnknown;
    }
    const auto remote = monitorimage::ReadRemote(hProc, module);
    const auto check = monitorimage::Compare(local, remote);
    CloseHandle(hProc);
    if (check.compatibility != monitorimage::Compatibility::Compatible)
        RecordMonitorDiagnostic(pid, check.diagnostic);
    if (check.compatibility == monitorimage::Compatibility::Unknown) {
        detail = "无法验证 DLL 接口（" + check.reason + "）；详情见当前会话 navgui_log.txt";
        return InjectUiState::VersionUnknown;
    }
    if (check.compatibility == monitorimage::Compatibility::Incompatible) {
        detail = "游戏中的 DLL 接口不兼容，或旧 DLL 缺少接口标识（" + check.reason +
                 "）；请重启此游戏并注入新包，详情见当前会话 navgui_log.txt";
        return InjectUiState::Incompatible;
    }

    const std::wstring pipeName = app::Config().PipeFullName();
    if (!ProbePipeAlive()) {
        RecordMonitorDiagnostic(pid, check.diagnostic + "\nhandshake=pipe-unavailable");
        char b[128];
        sprintf_s(b, "Monitor.dll 已加载 0x%llX，但管道没起来",
                  (unsigned long long)base);
        detail = b;
        return InjectUiState::LoadedNoPipe;
    }

    // “管道存在”只说明 Monitor 在监听，不能证明当前 GUI 拿到了
    // Controller/Tracker/Data 三条实际通道。刷新地图和 WalkCall 都依赖
    // 这组三角色连接；尤其 Controller 是独占的，另一个 NavGUI 占用时，
    // 旧逻辑仍会把状态误报成“已注入”。启动一次短等待，让异步注册结果
    // 落到 ConnectionSnapshot，再按真实控制权显示状态。
    (void)pipe::StartManagedConnections(pipeName, 150);
    const auto connection = pipe::GetConnectionSnapshot();
    // One combined record per probe avoids alternating identity/handshake logs
    // on persistent failures. Connection IDs are stable between reconnects.
    char handshake[256];
    sprintf_s(handshake, "\nhandshake=%s controllerError=%u gamePid=%u session=%llu Controller=%llu Tracker=%llu Data=%llu",
        connection.ready ? "ready" : "waiting", static_cast<unsigned>(connection.controllerError), connection.gamePid,
        static_cast<unsigned long long>(connection.sessionId), static_cast<unsigned long long>(connection.controllerId),
        static_cast<unsigned long long>(connection.trackerId), static_cast<unsigned long long>(connection.dataId));
    RecordMonitorDiagnostic(pid, check.diagnostic + handshake);
    if (connection.ready) {
        char b[128];
        sprintf_s(b, "Monitor.dll 已注入 0x%llX，Controller/Tracker/Data 已连接",
                  (unsigned long long)base);
        detail = b;
        return InjectUiState::Injected;
    }
    if (connection.controllerError == managed::Error::ControllerOwned) {
        detail = "Monitor.dll 已加载，但 Controller 已被其他 NavGUI 占用；请关闭其他 GUI 后重试";
        return InjectUiState::ControllerBusy;
    }
    if (connection.controllerError == managed::Error::BadProtocol) {
        detail = "DLL 接口标识通过，但通信握手拒绝协议（BadProtocol）；详情见当前会话 navgui_log.txt";
        return InjectUiState::Incompatible;
    }

    char b[160];
    sprintf_s(b, "Monitor.dll 已加载 0x%llX，正在等待当前 GUI 建立控制连接",
              (unsigned long long)base);
    detail = b;
    return InjectUiState::ManagedWait;
}

// 把 g_injectUi 落到界面：按钮文字 + 是否可点 + 状态小字。
//   ★ 只改"按钮能不能点"这一件事，不写日志 —— 它每 2 秒跑一次，
//     写日志会把日志区刷爆。状态真的变了才由调用方决定要不要记。
static void ApplyInjectUi()
{
    if (!g_btnInject.hwnd) return;

    const wchar_t* text = L"注入 DLL";
    bool enable = true;

    switch (g_injectUi) {
    case InjectUiState::Injected:
        text = L"已注入";
        enable = false;          // 需求：已注入则变暗不可点
        break;
    case InjectUiState::NoGame:
        text = L"未运行游戏";
        enable = false;
        break;
    case InjectUiState::NotInjected:
        text = L"注入 DLL";
        enable = true;
        break;
    case InjectUiState::LoadedNoPipe:
        text = L"重新注入";
        enable = true;
        break;
    case InjectUiState::ControllerBusy:
        text = L"控制端被占用";
        enable = false;
        break;
    case InjectUiState::ManagedWait:
        text = L"等待控制连接";
        enable = false;
        break;
    case InjectUiState::Incompatible:
        text = L"重启游戏以更新 DLL";
        enable = false;
        break;
    case InjectUiState::VersionUnknown:
        text = L"重新验证 DLL";
        enable = true;
        break;
    case InjectUiState::Unknown:
    default:
        text = L"注入 DLL";
        enable = true;           // 探测不出来就别拦着用户
        break;
    }

    // 注入进行中时不要被定时探测覆盖掉"注入中…"
    if (g_injectBusy) return;

    SetButtonText(g_btnInject, text);
    EnableWindow(g_btnInject.hwnd, enable ? TRUE : FALSE);
}

// 执行一次探测 + 应用状态。changed 回传状态是否发生变化（调用方用它决定写日志）。
static InjectUiState RefreshInjectState(bool* changed, std::string* detailOut)
{
    std::string detail;
    const InjectUiState st = ProbeInjection(detail);
    const bool diff = (st != g_injectUi);
    g_injectUi = st;
    g_monitorIncompatible.store(st == InjectUiState::Incompatible);

    // 状态码 → 给 PaintChrome 画在按钮下面那一行
    int code = kInjectUnknown;
    switch (st) {
    case InjectUiState::Injected:     code = kInjectOk;     break;
    case InjectUiState::NoGame:       code = kInjectNoGame; break;
    case InjectUiState::NotInjected:  code = kInjectNone;   break;
    case InjectUiState::LoadedNoPipe: code = kInjectNoPipe; break;
    case InjectUiState::ControllerBusy: code = kInjectBusy; break;
    case InjectUiState::ManagedWait: code = kInjectWait; break;
    case InjectUiState::Incompatible: code = kInjectIncompatible; break;
    case InjectUiState::VersionUnknown: code = kInjectVersionUnknown; break;
    default:                          code = kInjectUnknown; break;
    }
    g_injectCode.store(code);

    ApplyInjectUi();

    if (changed) *changed = diff;
    if (detailOut) *detailOut = detail;
    if (diff && MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
    return st;
}

// 「注入 DLL」按钮的干活函数。
//   reloadIfPresent = true 时，如果 Monitor.dll 已加载，先 FreeLibrary 再重新 Load
//   （方便改完 DLL 直接热更，不用重启游戏）。
// 返回 0 成功；其它值是失败码，日志里已经写清楚。
//
// ★★ 线程约束（重要）：本函数跑在 detach 出去的**工作线程**里，
//    绝不允许直接动控件：
//      · SetButtonText 改的是 Button::text（wchar_t[64]），
//        UI 线程的 PaintButton 正在读它 → 数据竞争，可能撕出乱码文字
//      · EnableWindow 跨线程改窗口状态，时序上和 PaintButton 打架
//    所以这里只做"进程操作 + 原子状态 + 日志"，界面更新统一由
//    PostMessage(WM_APP_INJECT_UI) 交给 UI 线程处理。
#define WM_APP_INJECT_UI (WM_APP + 41)

int DoInjectDll(bool reloadIfPresent)
{
    if (!g_running.load()) return -1;

    if (g_injectBusy.exchange(true)) {
        AddLog("注入：上一次还没结束");
        return -2;
    }

    // 让 UI 线程把按钮切成"注入中…"并置灰
    if (MainHwnd()) PostMessageW(MainHwnd(), WM_APP_INJECT_UI, 0, 0);
    AddLog("注入：开始（找游戏进程…）");

    auto finish = [&](int code, const std::string& status, const std::string& detail,
                      int stateCode) {
        g_injectCode.store(stateCode);
        SetStatus(status, detail, code == 0 ? 2 : 3);
        AddLog(status + (detail.empty() ? std::string() : ("：" + detail)));
        g_injectBusy.store(false);
        // 交回 UI 线程收尾（按钮文字/可点性由它按真实探测结果决定）
        if (MainHwnd()) PostMessageW(MainHwnd(), WM_APP_INJECT_UI, 0, 0);
        return code;
    };

    // ---- 1) 找游戏进程 ----
    std::wstring title;
    const DWORD pid = FindGamePid(title);
    if (!pid) {
        return finish(1, "注入失败：没找到游戏窗口",
                      "请先启动游戏（窗口标题见 config.ini [game] windowTitle）",
                      kInjectFail);
    }

    // ---- 2) DLL 绝对路径 ----
    const std::wstring dllPath = ModuleDirW() + L"Monitor.dll";
    {
        const DWORD attr = GetFileAttributesW(dllPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            return finish(2, "注入失败：找不到 Monitor.dll",
                          "请确认 Monitor.dll 与 NavGUI.exe 位于同一目录", kInjectFail);
        }
    }

    // ---- 3) 打开进程 ----
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) {
        char b[160];
        sprintf_s(b, "OpenProcess 失败 err=%lu（需要管理员权限 / 反作弊拦截）",
                  GetLastError());
        return finish(3, "注入失败：打不开进程", b, kInjectFail);
    }

    // ---- 4) 已注入？按需先卸载 ----
    const auto existingModule = monitorimage::Find(pid, L"Monitor.dll");
    if (existingModule.error || existingModule.matches > 1) {
        const std::string reason = "stage=enumerate-modules err=" + std::to_string(existingModule.error) +
                                   " matches=" + std::to_string(existingModule.matches);
        RecordMonitorDiagnostic(pid, reason);
        CloseHandle(hProc);
        return finish(4, "无法核对已加载模块", reason, kInjectVersionUnknown);
    }
    const uint64_t existedBase = existingModule.base;
    const auto localIdentity = monitorimage::ReadLocal(dllPath);
    // Validate before unloading anything. A compatible mapped DLL can reconnect
    // even if the file beside the GUI has since been removed or replaced.
    if ((!existedBase || reloadIfPresent) &&
        (!localIdentity.headers || !localIdentity.exports || !localIdentity.interfaceRead ||
         !monitorabi::Compatible(localIdentity.interfaceInfo))) {
        RecordMonitorDiagnostic(pid, "fresh-load/local-interface-rejected\nlocal " + monitorimage::Describe(localIdentity));
        CloseHandle(hProc);
        const bool knownIncompatible = localIdentity.interfaceRead && !monitorabi::Compatible(localIdentity.interfaceInfo);
        return finish(4, "本地 DLL 接口无法验证或不兼容",
                      "请使用同一运行包中的 NavGUI.exe 与 Monitor.dll；详情见当前会话 navgui_log.txt",
                      knownIncompatible ? kInjectIncompatible : kInjectVersionUnknown);
    }
    if (existedBase) {
        if (!reloadIfPresent) {
            const auto check = monitorimage::Compare(localIdentity, monitorimage::ReadRemote(hProc, existingModule));
            RecordMonitorDiagnostic(pid, check.diagnostic);
            if (check.compatibility != monitorimage::Compatibility::Compatible) {
                CloseHandle(hProc);
                return finish(4, "已加载 DLL 未通过接口校验", check.reason,
                    check.compatibility == monitorimage::Compatibility::Unknown ? kInjectVersionUnknown : kInjectIncompatible);
            }
            char b[96];
            sprintf_s(b, "Monitor.dll 已在 0x%llX，无需重复注入",
                      (unsigned long long)existedBase);
            CloseHandle(hProc);
            return finish(0, "已注入（跳过）", b, kInjectOk);
        }

        // Reconnection accepts a compatible ABI; unloading requires the exact
        // CodeView build as well. Resolve the entry from the remote export table.
        const auto remote = monitorimage::ReadRemote(hProc, existingModule);
        const auto check = monitorimage::Compare(localIdentity, remote);
        RecordMonitorDiagnostic(pid, check.diagnostic);
        const uint64_t shutdownRva = remote.shutdownRva;
        const bool compatible = shutdownRva && check.sameBuild &&
                                check.compatibility == monitorimage::Compatibility::Compatible;
        if(!compatible){CloseHandle(hProc);return finish(4,"需要重启游戏后注入新版 DLL",
            "协作卸载要求相同精确构建和远程关闭导出；详情见当前会话 navgui_log.txt",kInjectFail);}
        DWORD shutdownCode=0;
        if(!RemoteCallOne(hProc,(FARPROC)(existedBase+shutdownRva),nullptr,35000,&shutdownCode)||shutdownCode!=1){
            CloseHandle(hProc);return finish(4,"旧 DLL 停止失败","未卸载，不继续重复注入",kInjectFail);}
        HMODULE k32=GetModuleHandleW(L"kernel32.dll");FARPROC freeLib=GetProcAddress(k32,"FreeLibrary");DWORD fc=0;
        if(!freeLib||!RemoteCallOne(hProc,freeLib,(void*)existedBase,3000,&fc)||!fc){
            CloseHandle(hProc);return finish(4,"旧 DLL 卸载失败","未继续加载",kInjectFail);}
        Sleep(100);
    }

    // ---- 5) 写远程内存 ----
    //   路径按 UTF-16 写，配 LoadLibraryW —— 中文/空格路径不会炸。
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);

    LPVOID remote = VirtualAllocEx(hProc, nullptr, bytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        char b[128];
        sprintf_s(b, "VirtualAllocEx 失败 err=%lu", GetLastError());
        CloseHandle(hProc);
        return finish(4, "注入失败：分配远程内存", b, kInjectFail);
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remote, dllPath.c_str(), bytes, &written) || written != bytes) {
        char b[128];
        sprintf_s(b, "WriteProcessMemory 失败 err=%lu", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return finish(5, "注入失败：写远程内存", b, kInjectFail);
    }

    // ---- 6) 远程 LoadLibraryW ----
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadLib) {
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return finish(6, "注入失败：取不到 LoadLibraryW", "kernel32 异常", kInjectFail);
    }

    DWORD exitCode = 0;
    const bool waited = RemoteCallOne(hProc, loadLib, remote, 20000, &exitCode);
    const uint64_t loadedBase = waited ? FindRemoteModule(nullptr, pid, L"Monitor.dll") : 0;

    // A timed-out loader thread may still be reading its argument.
    // Retain the small path allocation until process exit in that case.
    if (waited) VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (!waited) {
        return finish(7, "注入等待超时",
                      "远程线程 20 秒没结束（DllMain 卡住了？看 Monitor 自己的日志）",
                      kInjectFail);
    }
    if (!loadedBase) {
        return finish(8, "注入失败：LoadLibraryW 返回 0",
                      "常见原因：位数不匹配 / 依赖 DLL 缺失 / 被安全软件拦截",
                      kInjectFail);
    }

    char b[128];
    sprintf_s(b, "Monitor.dll 已加载 base=0x%llX pid=%lu thread=%lu",
              (unsigned long long)loadedBase, (unsigned long)pid, (unsigned long)exitCode);
    return finish(0, "DLL 已加载，正在验证通信", b, kInjectOk);
}

// ============================================================
// 裸走路 call 标定
//
// ★ 为什么做成常驻功能而不是单独小工具：
//   标定要边看游戏边试，来回切窗口启动 exe 太慢。放在寻路页上，
//   输入坐标 → 点一次 → 立刻看到距离变化，随时能做。
//
// ★ 为什么不做去重/冷却：
//   这是标定工具，要看的就是"给任意坐标后角色真实反应"。加了冷却
//   反而测不到连续调用时的行为（比如"上一次还没走完就再给一个"）。
//   唯一的保护是按钮在调用期间禁用，防止一次操作里重复发包。
// ============================================================
void DoWalkCallOnce()
{
    struct ReleaseBusy { ~ReleaseBusy() { g_walkCallBusy = false; } } releaseBusy;
    if (!g_running.load() || follow::MainRunner().Running()) return;

    wchar_t bufX[32] = { 0 };
    wchar_t bufY[32] = { 0 };
    if (g_editWalkX) GetWindowTextW(g_editWalkX, bufX, 32);
    if (g_editWalkY) GetWindowTextW(g_editWalkY, bufY, 32);

    const int32_t tx = (int32_t)wcstol(bufX, nullptr, 10);
    const int32_t ty = (int32_t)wcstol(bufY, nullptr, 10);

    SetButtonText(g_btnWalkCall, L"调用中…");
    EnableWindow(g_btnWalkCall.hwnd, FALSE);
    SetStatus("调走路 call…", "目标 (" + std::to_string(tx) + "," + std::to_string(ty) + ")", 1);

    WalkCallAck ack{};
    const int rc = usecase::WalkCallOnce(tx, ty, &ack);

    // 无论成败都把回执里的数据存下来（失败时也能看到前后坐标）
    g_wcTargetX    = ack.targetX ? ack.targetX : tx;
    g_wcTargetY    = ack.targetY ? ack.targetY : ty;
    g_wcPlayerX    = ack.playerX;
    g_wcPlayerY    = ack.playerY;
    g_wcDistBefore = ack.distBefore;
    g_wcDistAfter  = ack.distAfter;
    g_wcElapsed    = ack.elapsedMs;
    g_wcResult     = ack.result;
    g_wcHaveResult = true;

    char line[320];
    switch (rc) {
    case 0:
        sprintf_s(line, "走路 call OK：目标(%d,%d) 角色(%d,%d) 距离 %d -> %d（%ums）",
                  g_wcTargetX, g_wcTargetY, g_wcPlayerX, g_wcPlayerY,
                  g_wcDistBefore, g_wcDistAfter, (unsigned)g_wcElapsed);
        AddLog(line);
        SetStatus("call 完成", "距离 " + std::to_string(g_wcDistBefore) +
                              " -> " + std::to_string(g_wcDistAfter), 2);
        break;
    case 1:
        AddLog("走路 call 失败：管道连不上（Monitor.dll 注入了吗？）");
        SetStatus("管道连不上", "Monitor.dll 注入了吗？", 3);
        break;
    case 2:
        AddLog("走路 call 失败：导航未就绪（角色还没进场景？）");
        SetStatus("未就绪", "角色还没进场景 / 动作系统解链失败", 3);
        break;
    case 3:
        sprintf_s(line, "走路 call 失败：result=%d（看 monitor_log.txt 的 [WalkCall]）",
                  ack.result);
        AddLog(line);
        SetStatus("call 失败", "result=" + std::to_string(ack.result), 3);
        break;
    default:
        AddLog("走路 call 失败：3 秒内没收到回执");
        SetStatus("无回执", "看 monitor_log.txt", 3);
        break;
    }

    SetButtonText(g_btnWalkCall, L"走一次");
    EnableWindow(g_btnWalkCall.hwnd, TRUE);
    if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
}

// 「取当前角色坐标」：把追踪器最新坐标填进两个输入框。
//   标定时的常用动作是"拿角色脚下当起点，再手改 y 加个距离"，
//   所以给个一键填。追踪器没数据时不覆盖输入框。
void DoWalkCallUsePlayerPos()
{
    int32_t px = 0, py = 0;
    if (!usecase::GetLastPlayerPos(px, py)) {
        AddLog("取角色坐标失败：追踪器还没数据（先点「刷新地图」启动追踪）");
        SetStatus("没有角色坐标", "先点「刷新地图」", 0);
        return;
    }

    wchar_t buf[32];
    if (g_editWalkX) {
        swprintf_s(buf, L"%d", px);
        SetWindowTextW(g_editWalkX, buf);
    }
    if (g_editWalkY) {
        swprintf_s(buf, L"%d", py);
        SetWindowTextW(g_editWalkY, buf);
    }
    SetStatus("已填入角色坐标", "(" + std::to_string(px) + "," + std::to_string(py) + ")", 2);
    AddLog("填入角色坐标 (" + std::to_string(px) + "," + std::to_string(py) + ")");
}

// ============================================================
// 导航测试状态只由主界面启动；两种模式共享同一条导航线程。
// g_busy 在 UI 线程预先占用，避免连续点击启动多个 worker。
void UpdateNavigationButtons()
{
    bool busy = false, hasGoal = false;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        busy = g_busy;
        hasGoal = g_hasGoal && !g_grid.Empty();
    }
    const auto test = g_navigationTest.load();
    const bool following = follow::MainRunner().Running() || follow::PartyTeleportPending();
    const bool canStart = !busy && !g_walkCallBusy.load() && hasGoal && test == NavigationTest::None && !following;
    EnableWindow(g_btnGo.hwnd, canStart);
    EnableWindow(g_btnNavComboStart.hwnd, canStart);
    EnableWindow(g_btnStop.hwnd, test == NavigationTest::NavigationOnly);
    EnableWindow(g_btnNavComboStop.hwnd, test == NavigationTest::NavigationAndCombo);
    if (following) {
        EnableWindow(g_btnWalkCall.hwnd, FALSE);
        EnableWindow(g_btnInject.hwnd, FALSE);
    } else {
        EnableWindow(g_btnWalkCall.hwnd, !busy && !g_walkCallBusy.load() && test == NavigationTest::None);
    }
}

void DoWalk(NavigationTest test, std::vector<combo::Row> comboRows, ritualui::Target target, bool fromRitual)
{
    struct Cleanup {
        bool withCombo;
        bool comboStopped = false;
        bool comboStarted = false;
        bool stopConfirmed = true;
        bool StopSkills()
        {
            if (!withCombo || !comboStarted || comboStopped) return stopConfirmed;
            combo::MainRunner().Stop();
            combo::MainRunner().Join(); // 等待当前蓄力松开后，才允许下一次测试。
            comboStopped = true;
            stopConfirmed = combo::MainRunner().Status().stopConfirmed;
            return stopConfirmed;
        }
        ~Cleanup()
        {
            g_stopNav = true;
            StopSkills();
            g_navigationComboStarted = false;
            combo::SetNavigationTestActive(false);
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                g_busy = false;
            }
            g_navigationTest = NavigationTest::None;
            g_navigationFromRitual = false;
        }
    } cleanup{test == NavigationTest::NavigationAndCombo};

    const auto cancelled = [&cleanup] {
        if (!g_running.load()) return true;
        if (!g_stopNav.load()) return false;
        const bool stopped = cleanup.StopSkills();
        SetStatus(stopped ? "已停止" : "技能停止未确认", stopped ? "导航测试已取消" : combo::MainRunner().Status().text, stopped ? 0 : 3);
        AddLog("导航测试已取消，未继续启动");
        return true;
    };
    try {
        if (cancelled()) return;
        usecase::NavigateOptions opt;
        usecase::LoadNavOptionsFromIni(opt);
        opt.dryRun = false;
        if (opt.mapBmpPath.empty()) opt.mapBmpPath = app::Config().BmpPath();

        // 独立地图副本与目标快照在整个 Navigate 调用期间保持有效。
        mapdata::CostBuffer costCopy;
        opt.goalX = target.x;
        opt.goalY = target.y;
        if (fromRitual) {
            SetStatus("正在读取祭祀寻路地图…", "目标 (" + std::to_string(target.x) + "," + std::to_string(target.y) + ")", 1);
            const auto connection = pipe::GetConnectionSnapshot();
            if (!connection.ready) throw std::runtime_error("通信未就绪，请先连接当前角色的 Monitor.dll");
            GameSession session;
            poe::WorldAreaSnapshot before, after;
            std::string error;
            if (!InitializeSession(session, &error) ||
                !poe::ReadWorldArea(session.memory, session.moduleBase, before, error))
                throw std::runtime_error("无法校验当前地图：" + error);
            if (target.area.Available() && !target.area.SameMap(before))
                throw std::runtime_error("候选对象来自旧地图实例，请重新扫描后选择目标");
            if (cancelled()) return;
            const int fetchResult = usecase::FetchCostBuffer(costCopy, "Ritual", &g_stopNav);
            if (cancelled()) return;
            if (fetchResult != 0 || costCopy.Empty())
                throw std::runtime_error("无法获取当前地图代价表，请确认角色已进入场景");
            if (!poe::ReadWorldArea(session.memory, session.moduleBase, after, error) || !before.SameMap(after) ||
                !connection.Same(pipe::GetConnectionSnapshot()) || !app::Config().TargetAlive())
                throw std::runtime_error("获取地图期间场景或通信会话变化，请重新扫描后再寻路");
            if (!costCopy.Geo().Contains(target.x, target.y))
                throw std::runtime_error("目标坐标超出当前地图范围，请检查 X / Y");
            if (!costCopy.At(target.x, target.y))
                throw std::runtime_error("目标坐标在地图阻挡区，请手动调整为对象附近可走的坐标");
            opt.presetSessionId = connection.sessionId;
        } else {
            std::lock_guard<std::mutex> lock(g_mtx);
            opt.presetSessionId = g_mapSessionId;
            costCopy = g_cost;
        }
        if (costCopy.Empty()) throw std::runtime_error("没有地图，请先刷新地图");
        const auto connection = pipe::GetConnectionSnapshot();
        if (!connection.ready || !opt.presetSessionId || opt.presetSessionId != connection.sessionId)
            throw std::runtime_error("地图不属于当前游戏会话，请刷新地图后再导航");
        opt.presetCost = &costCopy;
        if (cancelled()) return;

        if (cleanup.withCombo) {
            std::string error;
            combo::StartOptions start;
            start.resetSingleUse = true; // 每次手动开始测试，开场/单次技能重新执行一次。
            start.cancel = &g_stopNav;
            if (!combo::MainRunner().Start(std::move(comboRows), error, start)) {
                if (cancelled()) return;
                throw std::runtime_error("技能循环启动失败：" + error);
            }
            cleanup.comboStarted = true;
            g_navigationComboStarted = true;
            // Stop 可能在 Start 之前到达；此处再次检查，取消信号不被启动覆盖。
            if (cancelled()) return;
        }
        SetStatus(cleanup.withCombo ? "寻路+技能循环中…" : fromRitual ? "祭祀坐标寻路中…" : "单寻路测试中…",
                  "目标 (" + std::to_string(target.x) + "," + std::to_string(target.y) + ")", 1);
        AddLog(std::string(cleanup.withCombo ? "启动寻路+技能循环测试" : fromRitual ? "启动祭祀坐标单寻路" : "启动单寻路测试") +
            "，目标 (" + std::to_string(target.x) + "," + std::to_string(target.y) + ")");

        usecase::NavTelemetry tele;
        const int rc = usecase::Navigate(opt, &tele, &g_stopNav, &g_navStopReason);
        g_stopNav = true;
        if (cleanup.withCombo) {
            SetStatus("正在收尾…", "寻路已结束，正在停止连招并松开技能", 1);
            if (!cleanup.StopSkills()) {
                const auto state = combo::MainRunner().Status();
                SetStatus("技能停止未确认", state.text, 3);
                AddLog("寻路已结束，但游戏内技能停止未确认：" + state.text);
                return;
            }
        }
        if (rc == 0) {
            SetStatus("已抵达", cleanup.withCombo ? "角色已到达目标点，技能循环已停止" : "角色已到达目标点", 2);
            AddLog(cleanup.withCombo ? "已抵达，寻路与技能循环均已结束" : "单寻路测试已抵达");
        } else if (rc == 3) {
            const auto reason = g_navStopReason.load();
            if (cleanup.withCombo && reason == usecase::NavStopReason::None) {
                const auto state = combo::MainRunner().Status();
                SetStatus("技能循环已结束", state.text + "；寻路已停止", 3);
                AddLog("技能循环结束，联动停止寻路：" + state.text);
            } else {
                SetStatus("已停止", cleanup.withCombo ? "寻路与技能循环已停止" :
                          (tele.note.empty() ? "单寻路测试已停止" : tele.note), 0);
                AddLog(cleanup.withCombo ? "寻路与技能循环已停止：" + tele.note : "单寻路测试已停止：" + tele.note);
            }
        } else {
            const auto message = "寻路失败（rc=" + std::to_string(rc) + "）";
            SetStatus(message, tele.note.empty() ? "看下方日志的详细输出" : tele.note, 3);
            AddLog(message + "：" + tele.note + (cleanup.withCombo ? "；技能循环已停止" : ""));
        }
    } catch (const std::exception& error) {
        const bool stopped = cleanup.StopSkills();
        SetStatus("导航测试失败", std::string(error.what()) + (stopped ? "" : "；游戏内技能停止未确认"), 3);
        AddLog(std::string("导航测试失败：") + error.what());
    } catch (...) {
        const bool stopped = cleanup.StopSkills();
        SetStatus("导航测试失败", stopped ? "未知异常，测试已结束" : "未知异常；游戏内技能停止未确认", 3);
        AddLog("导航测试出现未知异常，已结束");
    }
    if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
}

bool StartNavigationTest(NavigationTest test, const ritualui::Target* explicitTarget = nullptr)
{
    if (!g_running.load() || g_walkCallBusy.load() || g_navigationTest.load() != NavigationTest::None ||
        follow::MainRunner().Running() || follow::PartyTeleportPending() ||
        (explicitTarget && (g_injectBusy.load() || g_monitorIncompatible.load() || combo::MainRunner().Status().running))) {
        if (explicitTarget) SetStatus("暂不能启动坐标寻路", "请先停止跟随或技能操作，并等待当前操作结束", 0);
        return false;
    }
    std::vector<combo::Row> rows;
    if (test == NavigationTest::NavigationAndCombo) {
        std::string error;
        if (!combo::LoadConfig(combo::DefaultConfigPath(), rows, error)) {
            SetStatus("连招配置读取失败", error, 3);
            AddLog("连招配置读取失败：" + error);
            return false;
        }
        if (rows.empty()) {
            SetStatus("还没有技能连招", "先在「技能连招」中添加并保存技能", 0);
            AddLog("组合测试未启动：连招列表为空");
            return false;
        }
    }
    bool ready = false;
    ritualui::Target target;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        ready = !g_busy && (explicitTarget || (g_hasGoal && !g_grid.Empty()));
        if (ready) {
            g_busy = true;
            if (explicitTarget) target = *explicitTarget;
            else { target.x = g_goalX; target.y = g_goalY; }
        }
    }
    if (!ready) {
        SetStatus("暂不能启动测试", "请先等待当前操作结束，并在地图上选择目标", 0);
        AddLog("测试未启动：当前操作尚未结束，或未选择有效地图目标");
        return false;
    }
    if (g_navigationThread.joinable()) g_navigationThread.join();
    g_navStopReason = usecase::NavStopReason::None;
    g_stopNav = false; // 只在 UI 启动点清除，worker 不会覆盖中途的停止请求。
    g_navigationComboStarted = false;
    g_navigationTest = test;
    g_navigationFromRitual = explicitTarget != nullptr;
    combo::SetNavigationTestActive(test == NavigationTest::NavigationAndCombo);
    UpdateNavigationButtons();
    SetStatus("正在启动测试…", test == NavigationTest::NavigationAndCombo ? "寻路+技能循环" : "单寻路", 1);
    try {
        g_navigationThread = std::thread(DoWalk, test, std::move(rows), std::move(target), explicitTarget != nullptr);
    } catch (const std::exception& error) {
        g_stopNav = true;
        combo::SetNavigationTestActive(false);
        { std::lock_guard<std::mutex> lock(g_mtx); g_busy = false; }
        g_navigationTest = NavigationTest::None;
        g_navigationFromRitual = false;
        UpdateNavigationButtons();
        SetStatus("测试启动失败", error.what(), 3);
        AddLog(std::string("无法创建导航线程：") + error.what());
        return false;
    }
    return true;
}

bool StartRitualAutomation(ritual::Options options, std::string& error)
{
    error.clear();
    if (!g_running.load() || g_walkCallBusy.load() || g_injectBusy.load() || g_monitorIncompatible.load() ||
        g_navigationTest.load() != NavigationTest::None || follow::MainRunner().Running() ||
        follow::PartyTeleportPending() || combo::MainRunner().Status().running || packetui::Busy()) {
        error = "请先停止跟随、寻路和技能操作，并等待当前操作结束"; return false;
    }
    std::vector<combo::Row> rows;
    if (!options.planOnly && (!combo::LoadConfig(combo::DefaultConfigPath(), rows, error) || rows.empty())) {
        if (error.empty()) error = "请先在技能连招中添加并保存技能；祭祀沿用寻路的技能循环配置";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_busy) { error = "当前操作尚未结束，请稍候"; return false; }
        g_busy = true;
    }
    if (!ritual::MainAutomation().Prepare(options, error)) {
        std::lock_guard<std::mutex> lock(g_mtx); g_busy = false; return false;
    }
    if (g_navigationThread.joinable()) g_navigationThread.join();
    g_stopNav = false; g_navStopReason = usecase::NavStopReason::None;
    g_navigationTest = NavigationTest::Ritual; g_navigationFromRitual = true;
    g_navigationComboStarted = false; combo::SetNavigationTestActive(!options.planOnly);
    SetStatus(options.planOnly ? "正在规划祭祀路线…" : "正在启动祭祀巡回…", "读取当前地图与祭祀位置", 1);
    UpdateNavigationButtons();
    try {
        g_navigationThread = std::thread([options = std::move(options), rows = std::move(rows)] {
            int rc = 1;
            try { rc = ritual::MainAutomation().Run(options, rows, g_stopNav, &g_navStopReason); }
            catch (const std::exception& exception) { ritual::MainAutomation().StartFailed(exception.what()); }
            catch (...) { ritual::MainAutomation().StartFailed("祭祀任务发生未知异常"); }
            const auto status = ritual::MainAutomation().GetStatus();
            SetStatus(rc == 0 ? (options.planOnly ? "祭祀路线已规划" : "祭祀巡回结束") :
                      rc == 3 ? "祭祀巡回已停止" : "祭祀巡回待处理", status.text, rc == 0 ? 2 : rc == 3 ? 0 : 3);
            AddLog("[祭祀] " + status.text);
            g_stopNav = true; combo::SetNavigationTestActive(false);
            { std::lock_guard<std::mutex> lock(g_mtx); g_busy = false; }
            g_navigationFromRitual = false; g_navigationTest = NavigationTest::None;
        });
    } catch (const std::exception& exception) {
        error = std::string("祭祀线程启动失败：") + exception.what(); ritual::MainAutomation().StartFailed(error);
        g_stopNav = true; combo::SetNavigationTestActive(false);
        { std::lock_guard<std::mutex> lock(g_mtx); g_busy = false; }
        g_navigationFromRitual = false; g_navigationTest = NavigationTest::None;
        UpdateNavigationButtons(); return false;
    }
    return true;
}

void StopNavigationTest(NavigationTest test)
{
    if (g_navigationTest.load() != test) return;
    g_navStopReason = usecase::NavStopReason::User;
    g_stopNav = true;
    if (test == NavigationTest::NavigationAndCombo || test == NavigationTest::Ritual) combo::MainRunner().Stop();
    if (test == NavigationTest::Ritual) {
        SetStatus("正在停止祭祀巡回…", "等待当前操作回执并松开技能，保留本地图进度", 1);
        AddLog("[祭祀] 已请求停止巡回"); return;
    }
    LOGI("[GUI] navigation test stop requested mode=%s", test == NavigationTest::NavigationAndCombo ? "navigation+combo" : "navigation");
    SetStatus("正在停止…", test == NavigationTest::NavigationAndCombo ? "正在停止寻路和技能循环，并松开技能" : "正在停止单寻路测试", 1);
    AddLog(test == NavigationTest::NavigationAndCombo ? "已请求停止寻路+技能循环" : "已请求停止单寻路测试");
}

// ============================================================
// 地图控件子类化
// ============================================================
LRESULT CALLBACK MapProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        PaintMap(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_CROSS));
        return TRUE;

    case WM_LBUTTONDOWN: {
        const int vx = GET_X_LPARAM(lp);
        const int vy = GET_Y_LPARAM(lp);
        LOGI("[GUI] 点击 客户区(%d,%d) 地图=%dx%d", vx, vy, g_mapW, g_mapH);
        AddLog("收到点击 " + std::to_string(vx) + "," + std::to_string(vy));

        int32_t wx = 0, wy = 0;
        bool inMini = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_mapW == 0) return 0;
            if (g_busy) return 0;

            // ★ 这里是地图控件的消息，坐标已经是控件内部坐标（0..g_miniSize），
            //   不能用 g_miniX/g_miniY（那是主窗口坐标），否则永远判断"点在界外"。
            if (vx < 0 || vy < 0 || vx >= g_miniSize || vy >= g_miniSize) {
                LOGI("[GUI] 点在控件外 %d,%d (size=%dx%d)", vx, vy, g_miniSize, g_miniSize);
                return 0;
            }

            // ★ 地图按比例居中后四周会有留白（长条图特别明显）。
            //   点在留白上不该算出目标：那样 MiniToWorld 会给出负坐标或
            //   超出世界边界的坐标，算路必然失败，用户还看不出为什么。
            {
                RECT mr;
                MapDrawRect(mr);
                if (vx < mr.left || vy < mr.top || vx >= mr.right || vy >= mr.bottom) {
                    LOGI("[GUI] 点在地图留白区 %d,%d（有效区域 %d,%d-%d,%d），忽略",
                         vx, vy, (int)mr.left, (int)mr.top, (int)mr.right, (int)mr.bottom);
                    return 0;
                }
            }

            inMini = true;
            MiniToWorld(vx, vy, wx, wy);

            g_goalX = wx;
            g_goalY = wy;
            g_hasGoal = true;
        }

        char buf[160];
        sprintf_s(buf, "选点：(%d, %d)", wx, wy);
        AddLog(buf);
        SetStatus("正在算路…", buf, 1);

        // 立刻重绘两个视图（目标点/视野框要马上可见，不等算路结果）
        InvalidateRect(hwnd, nullptr, FALSE);
        // 领一个代次号：算完回写时用它判断自己是否已被更新的一次点击取代
        const uint32_t myEpoch = g_planEpoch.fetch_add(1) + 1;
        std::thread(DoPlanPath, wx, wy, myEpoch).detach();
        return 0;
    }


    default:
        break;
    }
    return CallWindowProcW(g_oldMapProc, hwnd, msg, wp, lp);
}

// ============================================================
// 按钮窗口过程（自绘 + 悬停跟踪）
// ============================================================
LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 找到对应的 Button 结构
    Button* b = nullptr;
    // ★ 这里必须列出**所有**自绘按钮：漏掉一个，那个按钮就点不动。
    //   自绘按钮必须全部加入映射，否则不会转发 WM_COMMAND。
    for (Button* p : { &g_btnRefresh, &g_btnGo, &g_btnStop, &g_btnScan, &g_btnSkills, &g_btnNavComboStart, &g_btnNavComboStop, &g_btnNavSettings, &g_btnCombo, &g_btnWalkCall, &g_btnWalkUsePlayer, &g_btnInject }) {
        if (p->hwnd == hwnd) { b = p; break; }
    }
    if (!b) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_MOUSEMOVE:
        if (!b->hot) { b->hot = true; InvalidateRect(hwnd, nullptr, FALSE); }
        { TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme); }
        return 0;

    case WM_MOUSELEAVE:
        b->hot = false;
        b->down = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
        b->down = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONUP:
        if (b->down) {
            b->down = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            // 点完直接转发给父窗口 WM_COMMAND
            SendMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(b->id, BN_CLICKED), (LPARAM)hwnd);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        // 背景用面板色，让按钮像"嵌在卡片里"
        HBRUSH bg = CreateSolidBrush(col::kPanel);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        const bool enabled = IsWindowEnabled(hwnd) != FALSE;
        PaintButton(*b, hdc, rc, enabled);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// 改按钮上显示的文字
//
// ★ 必须改 Button::text 再重绘，不能用 SetWindowTextW。
//   PaintButton 画的是 Button::text 这个成员，**不读窗口文字**
//   （窗口文字从来没人读，因为 L"Poe2Button" 这个类是自绘的）。
//   所以原来满屏的 SetWindowTextW(btn, L"释放中…") 全是空操作 ——
//   用户点了按钮看不到任何变化，自然以为"点了没反应"。
// ============================================================
void SetButtonText(Button& b, const wchar_t* text)
{
    if (!text) return;
    if (wcscmp(b.text, text) == 0) return;   // 没变就别重绘，省闪烁
    wcsncpy_s(b.text, text, _TRUNCATE);
    if (b.hwnd) InvalidateRect(b.hwnd, nullptr, FALSE);
}

// ============================================================
// 创建按钮
// ============================================================
void CreateButton(Button& b, HWND parent, int id, int kind, const wchar_t* text)
{
    b.id = id;
    b.kind = kind;
    wcsncpy_s(b.text, text, _TRUNCATE);

    b.hwnd = CreateWindowExW(0, L"Poe2Button", L"",
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             0, 0, 100, 36,
                             parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SetWindowLongPtrW(b.hwnd, GWLP_WNDPROC, (LONG_PTR)BtnProc);
}

// ============================================================
// 创建控件
// ============================================================
void CreateControls(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    ComputeLayout(rc.right - rc.left, rc.bottom - rc.top);
    ritualui::CreatePanel(hwnd, [](const ritualui::Target& target, std::string& error) {
        const bool started = StartNavigationTest(NavigationTest::NavigationOnly, &target);
        if (!started) { std::lock_guard<std::mutex> lock(g_mtx); error = g_status + "：" + g_detail; }
        return started;
    }, [] {
        if (g_navigationFromRitual.load()) StopNavigationTest(g_navigationTest.load());
    }, [] {
        ritualui::NavigationState state;
        std::lock_guard<std::mutex> lock(g_mtx);
        state.running = g_navigationFromRitual.load() && g_navigationTest.load() != NavigationTest::None;
        state.canStart = g_running.load() && !g_busy && !g_walkCallBusy.load() && !g_injectBusy.load() &&
            !g_monitorIncompatible.load() && g_navigationTest.load() == NavigationTest::None &&
            !follow::MainRunner().Running() && !follow::PartyTeleportPending() && !combo::MainRunner().Status().running && !packetui::Busy();
        state.text = g_status; state.detail = g_detail;
        return state;
    }, [](const ritual::Options& options, std::string& error) { return StartRitualAutomation(options, error); });
    auto mayChangeView = [](std::string& reason) {
        if (!g_running.load() || g_injectBusy.load() || g_monitorIncompatible.load() || follow::PartyTeleportPending()) {
            reason = "请等待当前 DLL 或队友传送操作结束"; return false;
        }
        return true;
    };
    recoveryui::CreatePanel(hwnd, mayChangeView, [](const std::wstring& text) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string value(size_t((std::max)(0, size)), '\0');
        if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), value.data(), size, nullptr, nullptr);
        AddLog(value);
    });
    viewui::CreatePanel(hwnd, mayChangeView, [](const std::string& text) { AddLog(text); });
    packetui::CreatePanel(hwnd, [](std::string& reason) {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (!g_running.load() || g_busy || g_walkCallBusy.load() || g_injectBusy.load() || g_monitorIncompatible.load() ||
            g_navigationTest.load() != NavigationTest::None || follow::MainRunner().Running() ||
            follow::PartyTeleportPending() || combo::MainRunner().Status().running) {
            reason = "请先停止跟随、寻路和技能操作，并等待当前操作结束后再手动发包"; return false;
        }
        return true;
    });
    follow::CreatePanel(hwnd, [](std::string& reason) {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (!g_running.load() || g_busy || g_walkCallBusy.load() || g_injectBusy.load() || g_monitorIncompatible.load() ||
            g_navigationTest.load() != NavigationTest::None || combo::MainRunner().Status().running) {
            reason = "请先停止寻路/技能测试，并等待当前操作结束"; return false;
        }
        return true;
    });

    // 地图控件 = 右上角那块正方形（面板在左）
    const HWND hMapNew = CreateWindowExW(0, L"STATIC", L"",
                                         WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                         g_miniX, g_miniY, g_miniSize, g_miniSize,
                                         hwnd, (HMENU)1001, nullptr, nullptr);
    g_hMapStore.store(hMapNew);
    if (hMapNew) {
        g_oldMapProc = (WNDPROC)SetWindowLongPtrW(hMapNew, GWLP_WNDPROC, (LONG_PTR)MapProc);
    }

    const int px = g_pad;
    const int panelW = rc.right - g_pad * 2 - g_miniSize - g_pad;

    // 标题56 + 状态82 -> 按钮起点
    // ★ 顺序：先建按钮、再摆位。
    //   原来先 MoveButtons 后 CreateButton，那时 g_btnXxx.hwnd 还是 nullptr，
    //   SetWindowPos(NULL,...) 静默失败，按钮会停在默认 (0,0,100,36)，
    //   直到下一次 WM_SIZE 才被摆到正确位置（首次显示会看到一闪）。


    CreateButton(g_btnInject,  hwnd, 2201, 1, L"注入 DLL");
    CreateButton(g_btnGo,      hwnd, 2003, 0, L"单寻路测试");
    CreateButton(g_btnStop,    hwnd, 2004, 2, L"停止寻路测试");
    CreateButton(g_btnScan,    hwnd, 2005, 0, L"扫描");
    CreateButton(g_btnSkills, hwnd, 2012, 0, L"获取 / 刷新技能槽");
    CreateButton(g_btnNavComboStart, hwnd, 2014, 0, L"寻路+技能循环测试");
    CreateButton(g_btnNavComboStop, hwnd, 2015, 2, L"停止寻路+技能循环");
    CreateButton(g_btnNavSettings, hwnd, 2010, 1, L"寻路设置");
    CreateButton(g_btnCombo, hwnd, 2013, 0, L"技能连招");
    CreateButton(g_btnRefresh, hwnd, 2001, 1, L"刷新地图");

    // ---- 走路 call 标定控件 ----
    //
    // 输入框用 EDIT（比自绘省事，且能直接敲数字 / 选中复制）。
    //   ES_NUMBER 会禁掉负号，而地图坐标理论上不会是负的，所以开。
    //   但实测过"读到 -1 / 天文数字"的脏数据，为保证能手动输入负数调试，
    //   **不开 ES_NUMBER**，只在 DoWalkCallOnce 里做范围提示。
    g_editWalkX = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 80, 26, hwnd, (HMENU)2101, nullptr, nullptr);
    g_editWalkY = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 80, 26, hwnd, (HMENU)2102, nullptr, nullptr);
    if (g_editWalkX) {
        SendMessageW(g_editWalkX, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        SendMessageW(g_editWalkX, EM_SETLIMITTEXT, 12, 0);
    }
    if (g_editWalkY) {
        SendMessageW(g_editWalkY, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        SendMessageW(g_editWalkY, EM_SETLIMITTEXT, 12, 0);
    }

    CreateButton(g_btnWalkCall,      hwnd, 2103, 0, L"走一次");
    CreateButton(g_btnWalkUsePlayer, hwnd, 2104, 1, L"取角色");

    // ---- 注入 DLL ----


    MoveButtons(hwnd, px, panelW, g_pad + kTabH + 64 + 82);

    // 页签位置先算好（主窗口 WM_PAINT 里要用）
    ComputeTabs(rc.right);

    // 重新摆位（按钮对象建好后才有 hwnd）
    // ★ 顺序：先建按钮、再摆位。
    //   原来先 MoveButtons 后 CreateButton，那时 g_btnXxx.hwnd 还是 nullptr，
    //   SetWindowPos(NULL,...) 静默失败，按钮会停在默认 (0,0,100,36)，
    //   直到下一次 WM_SIZE 才被摆到正确位置（首次显示会看到一闪）。


    // 两种测试互斥；各自的停止按钮只在对应测试期间启用。
    EnableWindow(g_btnStop.hwnd, FALSE);
    EnableWindow(g_btnGo.hwnd, FALSE);
    EnableWindow(g_btnNavComboStart.hwnd, FALSE);
    EnableWindow(g_btnNavComboStop.hwnd, FALSE);


    MoveButtons(hwnd, px, panelW, g_pad + kTabH + 64 + 82);

    // 初始按钮可见性（寻路页显示两组测试与设置入口，扫描按钮藏起来）
    MovePageButtons(hwnd, rc.right, rc.bottom);

    // ---- 启动即探测注入状态（需求：开程序先判断，没注入才给能点的按钮）----
    //
    // 放在这里（控件都建好、MoveButtons/MovePageButtons 都跑完之后）：
    //   早于 CreateButton 调用会因为没有 hwnd 而被 ApplyInjectUi 直接 return；
    //   早于 MovePageButtons 则按钮可能被后面那次 show() 重置可见性。
    // 探测结果会：改按钮文字（注入 DLL / 已注入 / 未运行游戏 / 重新注入）
    // 和可点性（已注入 → 变暗不可点）。
    {
        bool changed = false;
        std::string detail;
        RefreshInjectState(&changed, &detail);
        AddLog(std::string("[注入检测] ") + (detail.empty() ? "已检测" : detail));
    }

    AddLog("界面已启动");
    AddLog("正在自动拉取地图…");

    std::thread([] { Sleep(300); DoRefreshMap(); }).detach();

    SetTimer(hwnd, 1, 50, nullptr);
}

// ============================================================
// 窗口过程
// ============================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_panelSession.HandleMessage(msg, wp, lp)) return 0;
    switch (msg) {
    case WM_CREATE: {
        auto* lifetime = static_cast<sessions::PanelGuard*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        if (!lifetime || !g_panelSession.Initialize(hwnd, *lifetime)) return -1;
        g_hMainStore.store(hwnd);
        g_animT0 = GetTickCount64();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 850;
        mmi->ptMinTrackSize.y = 680;
        return 0;
    }

    case WM_SIZE: {
        g_copyCells.clear();
        if (!MapHwnd()) break;

        const int cw = LOWORD(lp);
        const int ch = HIWORD(lp);
        if (cw <= 0 || ch <= 0) break;

        ComputeLayout(cw, ch);
        // ★ WM_SIZE 在窗口拖动时每个像素都会来一次（WM_ENTERSIZEMOVE/EXITSIZEMOVE
        //   之间尤其密集）。如果每次都无脑 SetWindowPos，即使矩形完全没变，
        //   子窗口也会被重新摆放 + 重绘 —— 这是"窗口一拖动整个界面就在闪"的
        //   主要来源之一。这里先算目标矩形，只有真的变了才动。
        auto placeIfMoved = [](HWND h, int x, int y, int w, int hgt) {
            if (!h || w <= 0 || hgt <= 0) return;
            RECT cur{};
            GetWindowRect(h, &cur);
            POINT tl{ cur.left, cur.top };
            HWND parent = GetParent(h);
            ScreenToClient(parent, &tl);
            if (tl.x == x && tl.y == y &&
                (cur.right - cur.left) == w && (cur.bottom - cur.top) == hgt) {
                return;      // 位置尺寸都没变，别碰它
            }
            SetWindowPos(h, nullptr, x, y, w, hgt,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        };

        placeIfMoved(MapHwnd(), g_miniX, g_miniY, g_miniSize, g_miniSize);

        MovePageButtons(hwnd, cw, ch);

        // ★ 第三参数用 FALSE（不擦背景）。
        //   原来写 TRUE：会让整窗先走一遍 WM_ERASEBKGND 再整窗重绘，
        //   和下面 MapHwnd 的失效叠加，窗口拖动/布局变化时闪烁非常明显。
        //   这里所有像素都是 PaintChrome 自己填的，不需要系统擦背景。
        InvalidateRect(hwnd, nullptr, FALSE);
        if (MapHwnd()) InvalidateRect(MapHwnd(), nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // 全部自绘，避免闪烁

    case WM_PAINT: {
        // The hit cache and displayed rows must come from the same frame,
        // including when Windows initially requests only a status-bar repaint.
        if (g_tab == 2 || g_tab == 3) InvalidateRect(hwnd, nullptr, FALSE);
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // 双缓冲：先画到内存位图，再一次性 BitBlt
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);

        PaintChrome(hwnd, mem);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        if (HWND(wp) == hwnd && LOWORD(lp) == HTCLIENT) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point);
            if (HitCopyCell(point)) {
                SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_HAND)); return TRUE;
            }
        }
        break;

    // ---------- 页签点击 / 单元格复制 / 列表滚动 ----------
    case WM_LBUTTONDOWN: {
        const int mx = GET_X_LPARAM(lp);
        const int my = GET_Y_LPARAM(lp);

        // 页签
        for (int i = 0; i < kPageCount; ++i) {
            const RECT& r = g_tabRc[i];
            if (r.right <= r.left) continue;
            if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                if (g_tab != i) {
                    g_tab = i;
                    g_copyCells.clear();
                    // 切页时重排页签按钮（寻路页的按钮只在 tab 0 显示）
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    MovePageButtons(hwnd, rc.right, rc.bottom);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        if (const auto* cell = HitCopyCell({mx, my})) {
            const auto text = cell->text;
            const bool skill = cell->page == 3;
            const bool copied = CopyToClipboard(hwnd, text);
            g_copyFeedback = copied ? std::wstring(skill ? L"已复制技能 ID：" : L"已复制名称：") + Utf8ToWide(text)
                                    : L"复制失败：剪贴板当前不可用";
            g_copyFeedbackUntil = GetTickCount64() + 2500;
            RECT bar{}; GetClientRect(hwnd, &bar); bar.top = bar.bottom - 30;
            InvalidateRect(hwnd, &bar, FALSE); SetFocus(hwnd);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        g_copyCells.clear();
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const int lines = -delta / WHEEL_DELTA * 3;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_tab == 1 && !g_inv.empty()) {
            g_invScroll += lines;
            if (g_invScroll < 0) g_invScroll = 0;
            if (g_invScroll > (int)g_inv.size() - 1) g_invScroll = (int)g_inv.size() - 1;
        }
        else if (g_tab == 2 && !g_around.empty()) {
            g_aroundScroll += lines;
            if (g_aroundScroll < 0) g_aroundScroll = 0;
            if (g_aroundScroll > (int)g_around.size() - 1) g_aroundScroll = (int)g_around.size() - 1;
        }
        else if (g_tab == 3 && !g_skills.slots.empty()) {
            g_skillScroll = std::clamp(g_skillScroll + lines, 0,
                static_cast<int>(g_skills.slots.size()) - 1);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER: {
        if (wp != 1) break;
        if (!g_copyFeedback.empty() && GetTickCount64() >= g_copyFeedbackUntil) {
            g_copyFeedback.clear();
            RECT bar{}; GetClientRect(hwnd, &bar); bar.top = bar.bottom - 30;
            InvalidateRect(hwnd, &bar, FALSE);
        }
        g_panelSession.Tick();
        if (g_skillDirty.exchange(false)) {
            const bool reading = g_skillBusy.load();
            SetButtonText(g_btnSkills, reading ? L"读取中…" : L"获取 / 刷新技能槽");
            EnableWindow(g_btnSkills.hwnd, reading ? FALSE : TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        {
            static pipe::ConnectionSnapshot last;
            const auto current = pipe::GetConnectionSnapshot();
            if (last.ready && !last.Same(current)) {
                g_navStopReason = usecase::NavStopReason::ConnectionChanged;
                g_stopNav = true;
                AddLog("[通信] 连接或会话变化，停止旧导航；不自动重发 WalkCall");
            }
            if (current.ready && !last.Same(current)) {
                char line[256];
                sprintf_s(line, "[通信] PID=%u session=%llu Controller=%llu Tracker=%llu Data=%llu",
                          current.gamePid, current.sessionId, current.controllerId, current.trackerId, current.dataId);
                AddLog(line);
            } else if (!current.ready && last.ready) {
                AddLog("[通信] 通信断开，等待当前游戏重新连接");
            }
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                if (!current.ready) g_havePlayer=false;
                if (current.ready && g_mapSessionId && g_mapSessionId!=current.sessionId) {
                    g_cost={};g_grid={};g_mapGray.clear();g_path.clear();
                    g_mapW=g_mapH=0;g_hasGoal=false;g_havePlayer=false;g_mapSessionId=0;
                    g_mapArea = {};
                    g_layoutDirty=true;
                }
            }
            last=current;
        }

        // ★ 地图尺寸变了要重算布局（换图后控件形状要跟着变）。
        //   后台线程只置 g_layoutDirty，真正的 SetWindowPos 在这里做 ——
        //   窗口操作必须在创建它的线程里执行。
        if (g_layoutDirty.exchange(false)) {
            RECT crc;
            GetClientRect(hwnd, &crc);
            ComputeLayout(crc.right - crc.left, crc.bottom - crc.top);
            if (MapHwnd()) {
                SetWindowPos(MapHwnd(), nullptr, g_miniX, g_miniY, g_miniSize, g_miniSize,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            const int panelW = crc.right - g_pad * 3 - g_miniSize;
            if (panelW > 80)
                MoveButtons(hwnd, g_pad, panelW, g_pad + kTabH + 64 + 82);
            LOGI("[GUI] 地图尺寸变化，重新布局: 地图 %ux%u 控件 %dx%d",
                 g_mapW, g_mapH, g_miniSize, g_miniSize);
            if (MapHwnd()) InvalidateRect(MapHwnd(), nullptr, FALSE);
            if (MainHwnd()) InvalidateRect(MainHwnd(), nullptr, FALSE);
        }

        // ---- 注入状态轮询 ----
        //
        // 需求：程序一开就自己判断 DLL 在不在，不在就给个能点的注入按钮，
        //       已在就变暗不可点。所以这里每 ~2 秒（50ms × 40）探一次：
        //   · 用户可能先开 GUI 再开游戏 → 状态要从 NoGame 变成 NotInjected
        //   · 用户可能重启游戏 → Monitor.dll 跟着没了，按钮要重新亮起来
        //   · 用户可能用外部 inject.ps1 注入 → 按钮要自己变暗
        //
        // 探测本身很轻：Toolhelp32 快照 + WaitNamedPipe(0)，都是微秒级，
        // 不会拖慢 UI 线程。注入进行中（g_injectBusy）时跳过，免得
        // 把"注入中…"的按钮文字冲掉。
        if (!g_injectBusy) {
            if (++g_injectProbeTick >= 40) {
                g_injectProbeTick = 0;

                bool changed = false;
                std::string detail;
                const InjectUiState before = g_injectUi;
                RefreshInjectState(&changed, &detail);

                // 只在状态真的翻转时记一行日志，避免每 2 秒刷屏
                if (changed) {
                    const char* what = "未知";
                    switch (g_injectUi) {
                    case InjectUiState::Injected:     what = "已注入"; break;
                    case InjectUiState::NoGame:       what = "游戏未运行"; break;
                    case InjectUiState::NotInjected:  what = "未注入"; break;
                    case InjectUiState::LoadedNoPipe: what = "已加载但管道未就绪"; break;
                    case InjectUiState::Incompatible: what = "DLL 接口不兼容，需更新"; break;
                    case InjectUiState::VersionUnknown: what = "无法验证 DLL 接口"; break;
                    default: break;
                    }
                    (void)before;
                    AddLog(std::string("[注入检测] ") + what +
                           (detail.empty() ? std::string() : ("：" + detail)));
                }
            }
        }

        // Repaint only when the ritual circle appears, moves, changes radius or disappears.
        {
            static bool previousVisible = false;
            static ritualcombat::Circle previousCircle;
            ritualcombat::Circle circle;
            bool visible = false;
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                visible = ritual::MainAutomation().GetCombatCircle(g_mapArea, circle);
            }
            if (visible != previousVisible || (visible && (circle.center.x != previousCircle.center.x ||
                circle.center.y != previousCircle.center.y || circle.radius != previousCircle.radius))) {
                if (MapHwnd()) InvalidateRect(MapHwnd(), nullptr, FALSE);
                previousVisible = visible; previousCircle = circle;
            }
        }

        // 角色位置轮询
        int32_t px = 0, py = 0;
        if (usecase::GetLastPlayerPos(px, py)) {
            bool changed = false;
            const bool hadPlayer = g_havePlayer;
            int32_t movedPx = 0, movedPy = 0;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                movedPx = px - g_playerX.load();
                movedPy = py - g_playerY.load();
                if (px != g_playerX.load() || py != g_playerY.load() || !g_havePlayer) {
                    g_playerX.store(px);
                    g_playerY.store(py);
                    g_havePlayer = true;
                    changed = true;
                }
            }

            // ★ 角色移动时刷新地图也要限频 + 限量。
            //
            //   原来只要坐标变一个像素就 Invalidate 整块地图，角色走路时
            //   等于每 50ms 全刷一次（含一次 1725x2139 的 StretchBlt），
            //   这就是地图"一直闪"的另一个来源。
            //
            //   现在的规则：
            //     · 至少隔 kMoveRepaintMs 才刷一次（默认 100ms = 10fps）
            //     · 且位移要够大（>= kMoveRepaintPx，默认 3 世界坐标）
            //   角色标记本身只有几个像素大，10fps 在视觉上完全够用；
            //   真要看清细节可以用「取角色」把坐标填进标定框。
            if (changed && MapHwnd()) {
                constexpr uint64_t kMoveRepaintMs = 100;
                constexpr int32_t  kMoveRepaintPx = 3;
                static uint64_t s_lastMoveRepaint = 0;
                const uint64_t nowTick = GetTickCount64();
                const int32_t dist2 = movedPx * movedPx + movedPy * movedPy;

                const bool dueTime = (nowTick - s_lastMoveRepaint) >= kMoveRepaintMs;
                const bool dueDist = dist2 >= kMoveRepaintPx * kMoveRepaintPx;
                // 首次拿到坐标必须立刻画出来，不能等 100ms
                if (!hadPlayer || (dueTime && dueDist)) {
                    s_lastMoveRepaint = nowTick;
                    InvalidateRect(MapHwnd(), nullptr, FALSE);
                }
            }
        }

        // 连招先结束时也终止组合测试，避免悄悄退化成只寻路。
        if (g_navigationTest.load() == NavigationTest::NavigationAndCombo &&
            g_navigationComboStarted.load() && !g_stopNav.load()) {
            const auto comboState = combo::MainRunner().Status();
            if (!comboState.running) {
                g_stopNav = true;
                AddLog("连招已结束，正在联动停止寻路：" + comboState.text);
            }
        }
        UpdateNavigationButtons();

        // ---------- 动画重绘：限频 ----------
        //
        // ★ 原来这里是"busy 就每帧(50ms)整窗 + 地图全刷一次"。两个问题：
        //
        //   1) 地图控件每 50ms 重画一次，每次都要把 1725x2139 的底图
        //      StretchBlt 一遍 —— 这是整个界面最贵的一步（几十毫秒），
        //      叠加窗口重绘就是肉眼可见的闪烁 + CPU 占用高。
        //   2) 主窗口和地图控件分开 Invalidate，两者刷新不同步，
        //      会出现"地图更新了、外框还是上一帧"的撕裂感。
        //
        //   现在：动画相位按 200ms 推进（呼吸灯不需要 20fps，8fps 足够
        //   顺滑），且**只在相位真的变了**才重绘。角色走动时靠上面的
        //   "坐标变了"分支单独刷地图，不依赖这里。
        {
            constexpr uint64_t kAnimFrameMs = 200;
            static uint64_t s_lastAnimFrame = 0;
            const uint64_t nowTick = GetTickCount64();
            bool busy = false;
            { std::lock_guard<std::mutex> lock(g_mtx); busy = g_busy; }
            if (busy && (nowTick - s_lastAnimFrame) >= kAnimFrameMs) {
                s_lastAnimFrame = nowTick;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (MapHwnd()) InvalidateRect(MapHwnd(), nullptr, FALSE);
            }
        }
        return 0;
    }

    // ---- 快捷键：Ctrl+C 复制当前页 / Ctrl+S 导出 ----
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!ctrl) break;
        if (g_tab >= 4) return 0;

        if (wp == 'C') {
            const std::string text = BuildPageText();
            if (CopyToClipboard(hwnd, text)) {
                char buf[96];
                sprintf_s(buf, "已复制到剪贴板（%zu 字节，制表符分隔）", text.size());
                AddLog(buf);
            }
            else {
                AddLog("复制失败：剪贴板被占用");
            }
            return 0;
        }

        if (wp == 'S') {
            if (g_tab == 3) return 0; // 技能槽页无需导出。
            const std::string text = BuildPageText();

            wchar_t path[MAX_PATH] = { 0 };
            const wchar_t* name = (g_tab == 1) ? L"inventory.txt"
                                : (g_tab == 2) ? L"around.txt"
                                               : L"nav.txt";

            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
            wcscpy_s(path, name);
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"导出当前页数据";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

            if (GetSaveFileNameW(&ofn)) {
                if (ExportToFile(text, path)) {
                    AddLog("已导出：" + std::string("(见文件对话框选的路径)"));
                }
                else {
                    AddLog("导出失败：无法写入文件");
                }
            }
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 2001:
            std::thread(DoRefreshMap).detach();
            break;

        case 2003:
            StartNavigationTest(NavigationTest::NavigationOnly);
            break;

        case 2004:
            StopNavigationTest(NavigationTest::NavigationOnly);
            break;

        case 2014:
            StartNavigationTest(NavigationTest::NavigationAndCombo);
            break;

        case 2015:
            StopNavigationTest(NavigationTest::NavigationAndCombo);
            break;

        case 2005:
            // 按当前页签决定扫哪个（两个功能互不干扰）
            if (g_tab == 1)      std::thread(DoScanInventory).detach();
            else if (g_tab == 2) std::thread(DoScanAround).detach();
            break;

        case 2012:
            StartSkillSlotsScan();
            SetButtonText(g_btnSkills, g_skillBusy ? L"读取中…" : L"获取 / 刷新技能槽");
            EnableWindow(g_btnSkills.hwnd, g_skillBusy ? FALSE : TRUE);
            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case 2010:
            ShowNavSettings(hwnd);
            break;

        case 2013:
            combo::ShowSettings(hwnd);
            break;

        case 2103:
            // 走路 call 标定：真实调一次，回距离
            if (!g_running.load() || follow::MainRunner().Running() || follow::PartyTeleportPending() ||
                g_navigationTest.load() != NavigationTest::None) break;
            { std::lock_guard<std::mutex> lock(g_mtx); if (g_busy) break; }
            if (g_walkCallBusy.exchange(true)) break;
            if (g_walkCallThread.joinable()) g_walkCallThread.join();
            try { g_walkCallThread = std::thread(DoWalkCallOnce); }
            catch (const std::exception& error) {
                g_walkCallBusy = false;
                SetStatus("无法启动走路调用", error.what(), 3);
            }
            UpdateNavigationButtons();
            break;

        case 2104:
            // 把追踪器最新角色坐标填进两个输入框
            DoWalkCallUsePlayerPos();
            break;

        case 2201:
            if (follow::MainRunner().Running() || follow::PartyTeleportPending()) break;
            // 注入 Monitor.dll：放后台线程 —— LoadLibraryW 在游戏里初始化
            // 可能要十几秒，卡在 UI 线程会把窗口冻住。
            //
            // reloadIfPresent=true：已经注入过的话先 FreeLibrary 再重新 Load，
            // 这样改完 Monitor.dll 直接点按钮就能热更，不用重启游戏。
            std::thread([] {
                DoInjectDll(true);
            }).detach();
            break;


        default:
            break;
        }
        return 0;

    // ---- 注入线程 → UI 线程的回调 ----
    //
    // DoInjectDll 在后台线程里跑，它不能直接碰控件（数据竞争），
    // 所以只 PostMessage 过来，由这里统一更新按钮。
    //   · 注入进行中：按钮显示"注入中…"并置灰
    //   · 注入结束：按真实探测结果决定文字/可点性（已注入则变暗不可点）
    case WM_APP_INJECT_UI: {
        if (g_injectBusy.load()) {
            SetButtonText(g_btnInject, L"注入中…");
            if (g_btnInject.hwnd) EnableWindow(g_btnInject.hwnd, FALSE);
        }
        else {
            // 重新探一次再应用：注入刚结束时管道可能还没起来，
            // 直接按 g_injectUi 应用会把状态定死。
            bool changed = false;
            RefreshInjectState(&changed, nullptr);
            ApplyInjectUi();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DESTROY:
        // ★ 这几个标志是给后台线程看的"该收工了"信号。
        //   g_running 原来只被赋值、从来没被任何线程读取（等于摆设），
        //   DoRefreshMap 等旧 worker 曾在窗口销毁后
        //   继续跑，InvalidateRect 打到悬空 HWND，并在进程退出时碰已析构
        //   的全局对象（g_mtx / g_path）。现在 worker 入口会检查它。
        g_running = false;
        g_navStopReason = usecase::NavStopReason::WindowClosed;
        g_stopNav = true;
        KillTimer(hwnd, 1);
        recoveryui::Shutdown();
        viewui::Shutdown();
        follow::MainRunner().Stop();
        follow::MainRunner().StopPlayerScan();
        combo::MainRunner().Stop();
        g_panelSession.Shutdown();
        follow::ShutdownPanel();
        if (g_walkCallThread.joinable()) g_walkCallThread.join();

        // ★ 显式停掉坐标追踪器：否则它的管道客户端读线程会随进程被强杀，
        //   Monitor 端留下一个占着实例池槽位的僵尸连接（要等 RecycleSlot
        //   才回收）。放在 WM_DESTROY 里收尾是唯一正确的时机。
        if (g_navigationThread.joinable()) g_navigationThread.join();
        if (g_skillThread.joinable()) g_skillThread.join();
        ritualui::Shutdown();
        combo::ShutdownCombo();
        packetui::Shutdown();
        if(g_pipeStartupThread.joinable()) g_pipeStartupThread.join();
        usecase::StopPlayerTracker();
        pipe::StopManagedConnections();

        // ★ 先置空句柄，worker 里的 `if (MapHwnd())` 判断才有意义。
        g_hMapStore.store(nullptr);
        g_hMainStore.store(nullptr);

        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ============================================================
// WinMain
// ============================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    app::AppConfig& cfg = app::Config();
    cfg.LoadFromFile("config.ini");
    cfg.ApplyEnvironmentOverrides();
    sessions::Selection selection;
    std::wstring launchError;
    if (!sessions::ParseSelection(selection, launchError)) {
        MessageBoxW(nullptr, launchError.c_str(), L"启动参数错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!selection.pid) return sessions::RunLauncher(hInst, nShow);
    sessions::PanelGuard panelGuard;
    if (!panelGuard.Acquire(selection, launchError)) {
        MessageBoxW(nullptr, launchError.c_str(), L"独立面板", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    std::string bindError;
    if (!cfg.BindTarget(selection.pid, selection.start, selection.window, bindError)) {
        MessageBoxW(nullptr, winutil::Utf8ToWide(bindError).c_str(), L"目标绑定失败", MB_OK | MB_ICONERROR);
        return 1;
    }
    poelog::Init(poelog::Target::File, "navgui_log.txt", gametarget::Ansi(cfg.sessionDirectory));
    pipe::SetExpectedGameIdentity(cfg.targetPid, cfg.targetStart);
    struct PipeCleanup { ~PipeCleanup() { pipe::StopManagedConnections(); poelog::Shutdown(); } } pipeCleanup;
    const bool connected = sessions::RunConnectDialog(hInst, panelGuard, [](std::atomic_bool& cancelled, std::wstring& error) {
        const auto& target = app::Config();
        const auto unlocked = instance::Release(target.targetPid, target.targetStart);
        if (!unlocked.ok) AddLog("多开互斥体处理失败，请检查列表状态");
        if (cancelled) return false;
        std::string detail;
        auto state = ProbeInjection(detail);
        if (state == InjectUiState::NotInjected) {
            if (DoInjectDll(false) != 0) { error = L"DLL 加载失败，请检查此进程 profiles 目录内的 navgui_log.txt。"; return false; }
        }
        const auto deadline = GetTickCount64() + 12000;
        while (!cancelled && GetTickCount64() < deadline) {
            if (!target.TargetAlive()) { error = L"所选游戏已退出，请在列表中重新选择。"; return false; }
            state = ProbeInjection(detail);
            if (state == InjectUiState::Injected) {
                const auto connection = pipe::GetConnectionSnapshot();
                if (connection.ready && connection.gamePid == target.targetPid) return true;
            }
            if (state == InjectUiState::Incompatible || state == InjectUiState::VersionUnknown ||
                state == InjectUiState::ControllerBusy || state == InjectUiState::NoGame) {
                error = winutil::Utf8ToWide(detail); return false;
            }
            Sleep(100);
        }
        error = L"未能建立所选游戏的独立通信通道：" + winutil::Utf8ToWide(detail);
        return false;
    });
    if (!connected) return 1;
    if (!panelGuard.Active()) return 0;

    // 字体：优先雅黑，退化到系统 UI 字体
    {
        const wchar_t* uiFace = L"Microsoft YaHei UI";
        g_fontUI   = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, uiFace);
        g_fontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, uiFace);
        g_fontBig  = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, uiFace);
        // Consolas has no Chinese glyphs on some Windows installations; GDI
        // then draws tofu for dynamic status, log, and coordinate text.
        g_fontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, uiFace);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // ★ 不要 CS_HREDRAW | CS_VREDRAW：
    //   这两个风格会在窗口尺寸发生任何变化时强制把**整个客户区**标脏重画。
    //   主窗口里所有内容都是自绘（面板/日志/页签/状态卡），本来就已经做到
    //   双缓冲；再叠一层"尺寸一变就全窗失效"，就变成了每帧整窗 BitBlt，
    //   视觉上表现为面板和地图一起闪。去掉之后由我们自己精确控制失效区域。
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;   // 全自绘
    wc.lpszClassName = L"Poe2NavGui";
    wc.hIcon = ui::AppIcon(); wc.hIconSm = ui::AppIcon(true);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"窗口类注册失败", L"错误", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW bc{};
    bc.cbSize = sizeof(bc);
    bc.style = 0;   // 同上：自绘按钮不需要类级别的整块失效
    bc.lpfnWndProc = BtnProc;
    bc.hInstance = hInst;
    bc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_HAND);
    bc.hbrBackground = nullptr;
    bc.lpszClassName = L"Poe2Button";
    if (!RegisterClassExW(&bc)) {
        MessageBoxW(nullptr, L"按钮类注册失败", L"错误", MB_ICONERROR);
        return 1;
    }

    // Match the supplied full-panel screenshot: about 1460 x 1050 visible pixels
    // at the existing 125% Windows scaling, including the title bar and borders.
    // Keep Win32 logical units so Windows applies the same scaling as the UI.
    constexpr int kDefaultPanelWidth = 1182, kDefaultPanelHeight = 848;
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(cfg.TargetWindow(), MONITOR_DEFAULTTONEAREST), &monitor))
        work = monitor.rcWork;
    const int workWidth = int(work.right - work.left), workHeight = int(work.bottom - work.top);
    const int initW = (std::min)(kDefaultPanelWidth, workWidth);
    const int initH = (std::min)(kDefaultPanelHeight, workHeight);
    const int initX = work.left + (workWidth - initW) / 2;
    const int initY = work.top + (workHeight - initH) / 2;

    // ★★ WS_CLIPCHILDREN 是这里最关键的一个风格，之前一直漏了。
    //
    //   主窗口 WM_PAINT 是"先画进内存位图，再整幅 BitBlt 到屏幕"。没有
    //   WS_CLIPCHILDREN 时，这次 BitBlt 会把**子窗口占的区域**也一起涂掉：
    //   地图控件、两个 EDIT 输入框、所有自绘按钮，全在主窗口客户区里。
    //   涂掉之后 Windows 再让子窗口各自 WM_PAINT 画回来 —— 于是同一块
    //   像素在"父窗口画的"和"子窗口画的"之间来回切换，看起来就是一直闪。
    //
    //   走路 call 标定区最明显：那里挤着 2 个 EDIT + 2 个按钮 + 底纹卡片，
    //   是重叠绘制最密集的一块，所以"新做的走路 call 区域一直闪"。
    //   地图区同理（之前那个"地图一直闪"其实也是这个根因的另一个表现）。
    //
    //   加上 WS_CLIPCHILDREN 之后，父窗口绘制时系统会裁剪掉所有子窗口区域，
    //   每个像素只有唯一的主人，闪烁从原理上消失。
    const std::wstring panelTitle = gametarget::PanelTitle(cfg.targetPid, cfg.targetStart);
    HWND hwnd = CreateWindowExW(0, L"Poe2NavGui", panelTitle.c_str(),
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                initX, initY,
                                initW, initH,
                                nullptr, nullptr, hInst, &panelGuard);
    if (!hwnd) {
        MessageBoxW(nullptr, L"创建窗口失败", L"错误", MB_ICONERROR);
        return 1;
    }

    // 主界面和设置窗口共享标题栏主题与程序图标。
    {
        ui::ApplyWindow(hwnd, false);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    if (IsWindow(hwnd) && panelGuard.Active()) CreateControls(hwnd);
    if (IsWindow(hwnd) && panelGuard.Active()) {
        g_pipeStartupThread=std::thread([] {
            const auto& cfg=app::Config();
            std::string pipeName=cfg.pipeName;
            while(g_running.load()) {
                if(usecase::StartPlayerTracker(pipeName)) break;
                for(int i=0;i<10&&g_running.load();++i) Sleep(100);
            }
        });
    }

    MSG m{};
    while (panelGuard.WaitForMessage()) {
        if (!PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) continue;
        if (m.message == WM_QUIT) break;
        if (ui::ProcessDialogMessage(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (IsWindow(hwnd)) {
        LOGI("[Panel] Closing session: %s", panelGuard.ExitReason());
        DestroyWindow(hwnd);
    }

    if (g_fontUI)   DeleteObject(g_fontUI);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontMono) DeleteObject(g_fontMono);
    if (g_fontBig)  DeleteObject(g_fontBig);

    g_running = false;
    return 0;
}

