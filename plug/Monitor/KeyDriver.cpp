#include "KeyDriver.h"
#include "Log.h"

#include <windows.h>
#include <cmath>
#include <vector>

namespace keydrive {

namespace {

constexpr WORD kVkW = 'W';
constexpr WORD kVkA = 'A';
constexpr WORD kVkS = 'S';
constexpr WORD kVkD = 'D';

// 当前按下的键（用于确保 ReleaseAll 能全部松开）
bool g_wDown = false;
bool g_aDown = false;
bool g_sDown = false;
bool g_dDown = false;

bool ForceForeground(HWND h)
{
    if (!h) {
        return false;
    }
    if (IsIconic(h)) {
        ShowWindow(h, SW_RESTORE);
    }
    if (GetForegroundWindow() == h) {
        return true;
    }

    // 单纯 SetForegroundWindow 常被 Windows 的前台锁拦掉，
    // 必须配合 AttachThreadInput 才可靠。
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD me = GetCurrentThreadId();
    bool attached = false;
    if (fg && fg != me) {
        attached = (AttachThreadInput(me, fg, TRUE) != 0);
    }

    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);

    if (attached) {
        AttachThreadInput(me, fg, FALSE);
    }
    Sleep(40);
    return GetForegroundWindow() == h;
}

void SendKey(WORD vk, bool down)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// 按住时定期补发 KEYDOWN：有些游戏需要重复事件才算持续输入。
void RepeatKey(WORD vk)
{
    SendKey(vk, true);
}

} // namespace

bool KeyDriver::Initialize(void* hwnd, const char* windowTitleUtf8,
                           const KeyDriveConfig& cfg, std::string& err)
{
    if (!hwnd) {
        // 没给句柄就按配置里的标题找一次（UTF-8 -> 宽字符）。
        // 这样"窗口标题"只有一个来源，不会在这里再写死一份。
        if (windowTitleUtf8 && *windowTitleUtf8) {
            wchar_t wide[256] = { 0 };
            const int n = MultiByteToWideChar(CP_UTF8, 0, windowTitleUtf8, -1,
                                              wide, (int)(sizeof(wide) / sizeof(wide[0])));
            if (n > 0) {
                hwnd = (void*)FindWindowW(nullptr, wide);
            }
        }
        if (!hwnd) {
            err = "窗口句柄为空，且按标题找不到游戏窗口";
            return false;
        }
    }
    m_hwnd = hwnd;
    m_cfg = cfg;

    if (!ForceForeground((HWND)m_hwnd)) {
        LOGI("[Key] 无法把游戏切到前台，按键可能不生效");
    }
    return true;
}

void KeyDriver::ReleaseAll()
{
    if (g_wDown) { SendKey(kVkW, false); g_wDown = false; }
    if (g_aDown) { SendKey(kVkA, false); g_aDown = false; }
    if (g_sDown) { SendKey(kVkS, false); g_sDown = false; }
    if (g_dDown) { SendKey(kVkD, false); g_dDown = false; }
}

// ---- 方向 -> 按键位掩码 ----
// bit0=W bit1=A bit2=S bit3=D
//
// 和 PressToward 用同一套点积规则（cos 72度 以上算命中）。
// 在四个正交对角基下，任意方向最多命中两个键，所以掩码最多两位。
uint32_t KeyDriver::MaskForDir(double dx, double dy) const
{
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        return 0;
    }
    const double ux = dx / len;
    const double uy = dy / len;

    struct Cand { uint32_t bit; double x; double y; };
    const Cand cands[4] = {
        { 1u, kDirW.x, kDirW.y },
        { 2u, kDirA.x, kDirA.y },
        { 4u, kDirS.x, kDirS.y },
        { 8u, kDirD.x, kDirD.y },
    };

    uint32_t mask = 0;
    for (const Cand& c : cands) {
        if (ux * c.x + uy * c.y > 0.30) {
            mask |= c.bit;
        }
    }
    return mask;
}

void KeyDriver::ApplyMask(uint32_t mask)
{
    // 逐键比对，只发状态真正变化的那个。
    // 这条是"连贯"的关键：方向不变时这里什么都不做，
    // 角色就会一直朝着那个方向走，而不是每 200ms 停一下。
    const struct { uint32_t bit; WORD vk; bool* flag; } keys[4] = {
        { 1u, kVkW, &g_wDown },
        { 2u, kVkA, &g_aDown },
        { 4u, kVkS, &g_sDown },
        { 8u, kVkD, &g_dDown },
    };

    for (const auto& k : keys) {
        const bool want = (mask & k.bit) != 0;
        if (want && !*k.flag) {
            SendKey(k.vk, true);
            *k.flag = true;
        }
        else if (!want && *k.flag) {
            SendKey(k.vk, false);
            *k.flag = false;
        }
    }
}

void KeyDriver::RepeatHeld()
{
    if (g_wDown) RepeatKey(kVkW);
    if (g_aDown) RepeatKey(kVkA);
    if (g_sDown) RepeatKey(kVkS);
    if (g_dDown) RepeatKey(kVkD);
}

void KeyDriver::PressToward(double dx, double dy)
{
    // 先全部松开，避免上一次的键还按着
    ReleaseAll();

    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        return;
    }
    const double ux = dx / len;
    const double uy = dy / len;

    // 四个候选方向的世界单位向量（与实测标定一致）
    struct Cand { WORD vk; double x; double y; };
    const Cand cands[4] = {
        { kVkW,  kDirW.x, kDirW.y },
        { kVkA,  kDirA.x, kDirA.y },
        { kVkS,  kDirS.x, kDirS.y },
        { kVkD,  kDirD.x, kDirD.y },
    };

    // 选点积最大的两个（正交基下，两个就足够合成任意方向）
    // 这里用简单的排序：取点积 > 0 的全部键按下，
    // 因为只有朝向目标的那 1~2 个键点积为正，
    // 背向的键点积为负自然被排除。
    for (const Cand& c : cands) {
        const double dot = ux * c.x + uy * c.y;
        if (dot > 0.30) {           // 0.30 ≈ cos(72°)，足够宽松又不会误按
            SendKey(c.vk, true);
            switch (c.vk) {
            case kVkW: g_wDown = true; break;
            case kVkA: g_aDown = true; break;
            case kVkS: g_sDown = true; break;
            case kVkD: g_dDown = true; break;
            default: break;
            }
        }
    }
}

bool KeyDriver::DriveTo(int32_t tx, int32_t ty,
                        void (*readPos)(int32_t& x, int32_t& y),
                        std::string& reason)
{
    if (!m_hwnd || !readPos) {
        reason = "未初始化";
        return false;
    }

    const uint64_t startTick = GetTickCount64();
    int32_t px = 0, py = 0;
    uint32_t noProgress = 0;

    // 当前按下的键组合（写成位掩码，方便判断"方向有没有变"）
    // bit0=W bit1=A bit2=S bit3=D
    uint32_t curMask = 0;

    LOGI("[Key] 开始驱动到 (%d,%d)", tx, ty);

    // 先算出初始方向并按下
    readPos(px, py);

    while (true) {
        // ---- 超时保护 ----
        if ((GetTickCount64() - startTick) > m_cfg.totalTimeoutMs) {
            ReleaseAll();
            reason = "总时长超限";
            return false;
        }

        readPos(px, py);
        const double dx = (double)tx - (double)px;
        const double dy = (double)ty - (double)py;
        const double dist = std::sqrt(dx * dx + dy * dy);

        // ---- 到达 ----
        if (dist <= m_cfg.arriveRadius) {
            ReleaseAll();
            LOGI("[Key] 抵达 (%d,%d)  当前 (%d,%d)  剩余距离 %.1f",
                 tx, ty, px, py, dist);
            reason = "抵达";
            return true;
        }

        // ---- 确保游戏在前台 ----
        // 焦点一旦被抢走，按住的方向键就收不到输入了。
        // 只在真的切走时才做重活（ForceForeground 内部有短路）。
        ForceForeground((HWND)m_hwnd);

        // ---- 算出目标方向对应的按键组合 ----
        const uint32_t wantMask = MaskForDir(dx, dy);

        // ---- 只在组合变化时动按键 ----
        //
        // 这是"连贯"的关键：方向没变就一直按着，
        // 不要每一步都松手重按（那会让角色每 200ms 停一次）。
        if (wantMask != curMask) {
            ApplyMask(wantMask);
            curMask = wantMask;
        }

        // ---- 每 50ms 检查一次 ----
        Sleep(50);

        // 按住时补发 KEYDOWN：某些游戏需要重复事件才算持续输入。
        RepeatHeld();

        // ---- 卡住检测 ----
        // 只在方向没变的情况下才算"没进展"，换方向时位移小是正常的。
        if (wantMask == curMask) {
            int32_t nx = 0, ny = 0;
            readPos(nx, ny);
            const double mx = (double)nx - (double)px;
            const double my = (double)ny - (double)py;
            const double moved = std::sqrt(mx * mx + my * my);

            if (moved < m_cfg.stuckMoveThreshold) {
                ++noProgress;
                if (noProgress >= (m_cfg.stuckSteps * 10)) {
                    // 每 50ms 一次，所以阈值按 10 倍折算成原来按步计的语义
                    ReleaseAll();
                    LOGI("[Key] 连续无位移，判定卡住（当前 %d,%d，目标 %d,%d）",
                         nx, ny, tx, ty);
                    reason = "卡住";
                    return false;
                }
            }
            else {
                noProgress = 0;
            }
        }
        else {
            noProgress = 0;
        }
    }
}

} // namespace keydrive
