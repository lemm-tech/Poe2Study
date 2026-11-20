// KeyDriver.h - 用 WASD 方向键驱动角色沿路径行走。
//
// 为什么这条路可行（实测标定，不是推测）：
//   开着 WASD 移动时，四个键在世界坐标里是完全正交的对角方向：
//       W: (+1, +1)/sqrt2
//       S: (-1, -1)/sqrt2
//       A: (-1, +1)/sqrt2
//       D: (+1, -1)/sqrt2
//   任意方向都能用两个键的组合合成出来：
//       (+1, 0) = W + D
//       (0, +1) = W + A
//       (-1, 0) = S + A
//       (0, -1) = S + D
//   所以不需要「世界坐标 -> 屏幕坐标」的标定，也不需要调游戏的 vtable。
//   游戏自己的碰撞和贴墙逻辑会处理一切，我们只管大方向。
//
// 控制律：贪心转向
//   每个控制周期：读坐标 -> 算到当前航点的方向 -> 选键 -> 按一小段 -> 松手 -> 重读
//   每步都松手，是因为持续按住撞墙后会卡死；松开重按能让游戏
//   重新评估移动方向，更容易脱离贴墙状态。

#pragma once

#include <cstdint>
#include <string>

namespace keydrive {

// ------------------------------------------------------------
// 世界方向。由实测标定得出（单位向量）。
// 如果需要重新标定，改这里即可。
// ------------------------------------------------------------
struct DirVec {
    double x;
    double y;
};

// 四个键对应的世界方向（归一化）
constexpr DirVec kDirW = {  0.70710678,  0.70710678 };   // (+x, +y)
constexpr DirVec kDirS = { -0.70710678, -0.70710678 };   // (-x, -y)
constexpr DirVec kDirA = { -0.70710678,  0.70710678 };   // (-x, +y)
constexpr DirVec kDirD = {  0.70710678, -0.70710678 };   // (+x, -y)

// 按键驱动配置
struct KeyDriveConfig {
    // 单步按住时长（毫秒）。
    // 350ms 大约能让角色走出 40~50 像素，和航点间距同量级，
    // 这样起停次数少，走起来连贯。太短会变成一点一点挪。
    uint32_t stepHoldMs = 350;

    // 松手后等多久再重读坐标（毫秒）。
    // 只需要够游戏把新坐标写进内存（一帧 16ms 足够），
    // 原来是 90ms，角色会明显「走一步顿一下」。
    uint32_t stepSettleMs = 25;

    // 判定「已到航点」的距离（世界像素）
    // 到达判定半径（世界像素）。
    // 太小会导致最后一步反复过冲，太大则"没到就停"。
    // 每步按住 200ms 大约走 25 像素，取 6 比较合适。
    double arriveRadius = 6.0;

    // 单步后位移小于这个值视为「没动」（像素），用来检测卡住
    double stuckMoveThreshold = 2.0;

    // 连续多少步没动就认为卡住
    uint32_t stuckSteps = 6;

    // 整个驱动过程的总时长上限（毫秒）
    uint32_t totalTimeoutMs = 60000;
};

class KeyDriver {
public:
    // 绑定游戏窗口（用于切前台）。
    //   hwnd  窗口句柄。传 nullptr 时内部按 windowTitleUtf8 自己找一次，
    //         两者都拿不到才算失败。
    //   titleUtf8  窗口标题（UTF-8，来自 config.ini 的 [game] windowTitle）。
    //         这里**不写死任何标题**：游戏改名 / 改区域，只改配置即可。
    bool Initialize(void* hwnd, const char* windowTitleUtf8,
                    const KeyDriveConfig& cfg, std::string& err);

    // 驱动角色走向 (tx, ty)。
    //   readPos: 回调，返回当前世界坐标 (x, y)
    //   返回 true 表示到达，false 表示超时/卡住/失败
    bool DriveTo(int32_t tx, int32_t ty,
                 void (*readPos)(int32_t& x, int32_t& y),
                 std::string& reason);

    // 立刻松开所有按键（任何时候都可以调，包括异常路径）
    void ReleaseAll();

private:
    // 根据世界方向选按键并按下（一次性，用于旧逻辑）
    void PressToward(double dx, double dy);

    // 方向 -> 按键位掩码（bit0=W bit1=A bit2=S bit3=D）
    uint32_t MaskForDir(double dx, double dy) const;

    // 按掩码更新按键状态（只发需要变化的那几个）
    void ApplyMask(uint32_t mask);

    // 对当前按住的键补发 KEYDOWN
    void RepeatHeld();

    void*                  m_hwnd = nullptr;
    KeyDriveConfig         m_cfg;
};

} // namespace keydrive
