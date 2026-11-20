#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ui {
namespace color {
inline constexpr COLORREF kBg = RGB(18, 20, 26), kPanel = RGB(26, 29, 38);
inline constexpr COLORREF kPanelHi = RGB(34, 38, 50), kBorder = RGB(48, 54, 70);
inline constexpr COLORREF kBorderGlow = RGB(72, 84, 110), kText = RGB(214, 220, 232);
inline constexpr COLORREF kTextDim = RGB(130, 140, 160), kAccent = RGB(96, 168, 255);
inline constexpr COLORREF kAccentDim = RGB(52, 92, 148), kGood = RGB(72, 214, 138);
inline constexpr COLORREF kWarn = RGB(240, 176, 64), kBad = RGB(240, 96, 96);
}
enum class ButtonStyle { Secondary, Primary, Danger, Tab };
HFONT Font(int height = 14, int weight = FW_NORMAL);
HBRUSH Background();
HICON AppIcon(bool smallIcon = false);
void ApplyWindow(HWND window, bool settingsDialog = true);
void CenterWindow(HWND window, HWND owner);
void StyleControl(HWND control, HFONT font, ButtonStyle style = ButtonStyle::Secondary);
void SetButtonSelected(HWND control, bool selected);
void StyleLabel(HWND control, HFONT font, bool muted = false, bool onPanel = false);
void Card(HDC dc, RECT rect);
void Text(HDC dc, const wchar_t* text, RECT rect, HFONT font,
          COLORREF color = color::kText, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE);
// Handles owner-draw controls and dark list rows; leaves unrelated messages alone.
bool HandleMessage(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT& result);
bool ProcessDialogMessage(MSG& message);
} // namespace ui
