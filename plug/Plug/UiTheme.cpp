#define WIN32_LEAN_AND_MEAN
#include "UiTheme.h"
#include "../NavGUI/resource.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace ui {
namespace {
constexpr UINT_PTR kControlSubclass = 0x504F45, kWindowSubclass = 0x504F46;
enum class Kind { Button, Edit, Combo, ComboEdit, Label, List, Header };
struct Control {
    Kind kind; ButtonStyle style = ButtonStyle::Secondary;
    bool hot = false, muted = false, panel = false;
    bool selected = false, checkable = false, automatic = false;
    UINT checked = BST_UNCHECKED;
};
std::vector<HWND>& Dialogs() { static std::vector<HWND> windows; return windows; }
HBRUSH FieldBrush() { static HBRUSH brush = CreateSolidBrush(color::kBg); return brush; }
void Fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}
void Round(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius = 8) {
    HPEN pen = CreatePen(PS_SOLID, 1, border); HBRUSH brush = CreateSolidBrush(fill);
    auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, brush);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}
std::wstring Caption(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(size_t(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1); text.resize(length); return text;
}
HFONT ControlFont(HWND window) { return reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)); }
void PaintCombo(HWND hwnd, HDC dc, const Control& state) {
    RECT rect{}; GetClientRect(hwnd, &rect);
    COMBOBOXINFO info{sizeof(info)};
    const bool editable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & CBS_DROPDOWNLIST) == CBS_DROPDOWN;
    const bool haveInfo = GetComboBoxInfo(hwnd, &info) != FALSE;
    const int saved = SaveDC(dc);
    if (editable && haveInfo && info.hwndItem) {
        RECT edit{}; GetWindowRect(info.hwndItem, &edit);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&edit), 2);
        ExcludeClipRect(dc, edit.left, edit.top, edit.right, edit.bottom);
    }
    Fill(dc, rect, color::kPanel);
    const HWND focus = GetFocus();
    const bool enabled = IsWindowEnabled(hwnd) != FALSE, focused = focus == hwnd || (focus && IsChild(hwnd, focus));
    Round(dc, rect, enabled ? color::kBg : color::kPanel,
          focused ? color::kAccent : state.hot ? color::kBorderGlow : color::kBorder, 5);
    const int index = int(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
    std::wstring text;
    if (!editable && index >= 0) {
        const auto n = SendMessageW(hwnd, CB_GETLBTEXTLEN, index, 0);
        if (n >= 0 && n < 4096) {
            text.resize(size_t(n) + 1); SendMessageW(hwnd, CB_GETLBTEXT, index, LPARAM(text.data()));
            text.resize(size_t(n));
        }
    }
    RECT label{rect.left + 10, rect.top, rect.right - 28, rect.bottom};
    Text(dc, text.c_str(), label, ControlFont(hwnd), enabled ? color::kText : color::kTextDim,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    const RECT button = haveInfo ? info.rcButton : RECT{rect.right - 28, rect.top, rect.right, rect.bottom};
    const int x = (button.left + button.right) / 2, y = (button.top + button.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, enabled ? color::kText : color::kTextDim);
    auto previous = SelectObject(dc, pen);
    MoveToEx(dc, x - 4, y - 2, nullptr); LineTo(dc, x, y + 2); LineTo(dc, x + 4, y - 2);
    SelectObject(dc, previous); DeleteObject(pen);
    if (saved) RestoreDC(dc, saved);
}
void PaintHeader(HWND hwnd, HDC dc) {
    RECT client{}; GetClientRect(hwnd, &client); Fill(dc, client, color::kPanelHi);
    const int count = Header_GetItemCount(hwnd);
    for (int i = 0; i < count; ++i) {
        RECT rect{}; Header_GetItemRect(hwnd, i, &rect);
        wchar_t text[256]{}; HDITEMW item{}; item.mask = HDI_TEXT; item.pszText = text; item.cchTextMax = 256;
        Header_GetItem(hwnd, i, &item); rect.left += 10; rect.right -= 8;
        Text(dc, text, rect, ControlFont(hwnd), color::kTextDim,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    Fill(dc, {client.left, client.bottom - 1, client.right, client.bottom}, color::kBorder);
}
template <typename Paint>
void PaintBuffered(HWND hwnd, HDC target, Paint&& paint) {
    RECT rect{}; GetClientRect(hwnd, &rect);
    if (rect.right <= 0 || rect.bottom <= 0) { paint(target); return; }
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, rect.right, rect.bottom) : nullptr;
    if (!buffer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        paint(target); return;
    }
    const HGDIOBJ previous = SelectObject(buffer, bitmap);
    paint(buffer);
    BitBlt(target, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, previous); DeleteObject(bitmap); DeleteDC(buffer);
}
LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<Control*>(data);
    switch (message) {
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX:
        if (state.kind == Kind::Combo) {
            const bool enabled = IsWindowEnabled(hwnd) != FALSE;
            SetTextColor(HDC(wp), enabled ? color::kText : color::kTextDim);
            SetBkColor(HDC(wp), enabled ? color::kBg : color::kPanel);
            static HBRUSH disabled = CreateSolidBrush(color::kPanel);
            return LRESULT(enabled ? FieldBrush() : disabled);
        }
        break;
    case BM_GETCHECK:
        if (state.checkable) return state.checked;
        break;
    case BM_SETCHECK:
        if (state.checkable) {
            state.checked = wp == BST_CHECKED ? BST_CHECKED : BST_UNCHECKED;
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (!state.hot) {
            state.hot = true; TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE: state.hot = false; InvalidateRect(hwnd, nullptr, FALSE); break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE:
        if (state.kind == Kind::ComboEdit) InvalidateRect(GetParent(hwnd), nullptr, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE); RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE); break;
    case WM_NCPAINT:
        if (state.kind == Kind::Edit) {
            HDC dc = GetWindowDC(hwnd); RECT rect{}; GetWindowRect(hwnd, &rect); OffsetRect(&rect, -rect.left, -rect.top);
            HBRUSH border = CreateSolidBrush(GetFocus() == hwnd ? color::kAccent : color::kBorder);
            FrameRect(dc, &rect, border); DeleteObject(border); ReleaseDC(hwnd, dc); return 0;
        }
        break;
    case WM_PAINT:
        if (state.kind == Kind::Combo || state.kind == Kind::Label || state.kind == Kind::Header) {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
            PaintBuffered(hwnd, dc, [&](HDC buffered) {
                if (state.kind == Kind::Combo) PaintCombo(hwnd, buffered, state);
                else if (state.kind == Kind::Header) PaintHeader(hwnd, buffered);
                else {
                    RECT rect{}; GetClientRect(hwnd, &rect); Fill(buffered, rect, state.panel ? color::kPanel : color::kBg);
                    Text(buffered, Caption(hwnd).c_str(), rect, ControlFont(hwnd),
                         state.muted ? color::kTextDim : color::kText, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                }
            });
            EndPaint(hwnd, &paint); return 0;
        }
        break;
    case WM_PRINTCLIENT:
        if (state.kind == Kind::Combo) { PaintCombo(hwnd, HDC(wp), state); return 0; }
        if (state.kind == Kind::Header) { PaintHeader(hwnd, HDC(wp)); return 0; }
        break;
    case WM_ERASEBKGND:
        if (state.kind == Kind::Combo || state.kind == Kind::Label || state.kind == Kind::Header || state.kind == Kind::Button) return 1;
        break;
    case WM_NCDESTROY: {
        const auto result = DefSubclassProc(hwnd, message, wp, lp);
        RemoveWindowSubclass(hwnd, ControlProc, kControlSubclass);
        delete &state; return result;
    }
    }
    const auto result = DefSubclassProc(hwnd, message, wp, lp);
    if ((message == CB_SETCURSEL || message == CB_RESETCONTENT || message == WM_SETTEXT) &&
        (state.kind == Kind::Combo || state.kind == Kind::Label)) InvalidateRect(hwnd, nullptr, FALSE);
    if (state.kind == Kind::Combo && (message == WM_SIZE || message == CB_SHOWDROPDOWN ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_CAPTURECHANGED)) InvalidateRect(hwnd, nullptr, FALSE);
    return result;
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        auto& windows = Dialogs(); windows.erase(std::remove(windows.begin(), windows.end(), hwnd), windows.end());
        RemoveWindowSubclass(hwnd, WindowProc, kWindowSubclass);
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}
void Attach(HWND hwnd, Control* state) {
    if (!SetWindowSubclass(hwnd, ControlProc, kControlSubclass, DWORD_PTR(state))) delete state;
}
} // namespace

HFONT Font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
}
HBRUSH Background() { static HBRUSH brush = CreateSolidBrush(color::kBg); return brush; }
HICON AppIcon(bool smallIcon) {
    return HICON(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                           GetSystemMetrics(smallIcon ? SM_CXSMICON : SM_CXICON),
                           GetSystemMetrics(smallIcon ? SM_CYSMICON : SM_CYICON), LR_SHARED));
}
void ApplyWindow(HWND window, bool settingsDialog) {
    BOOL dark = TRUE; DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const COLORREF caption = color::kBg, text = color::kText, border = color::kBorder;
    DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 36, &text, sizeof(text));
    DwmSetWindowAttribute(window, 34, &border, sizeof(border));
    SendMessageW(window, WM_SETICON, ICON_BIG, LPARAM(AppIcon()));
    SendMessageW(window, WM_SETICON, ICON_SMALL, LPARAM(AppIcon(true)));
    if (settingsDialog) {
        Dialogs().push_back(window); SetWindowSubclass(window, WindowProc, kWindowSubclass, 0);
    }
}
void CenterWindow(HWND window, HWND owner) {
    RECT parent{}, rect{}; GetWindowRect(owner, &parent); GetWindowRect(window, &rect);
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &info);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const int x = std::clamp<int>((parent.left + parent.right - width) / 2, int(info.rcWork.left),
                           (std::max)(int(info.rcWork.left), int(info.rcWork.right) - width));
    const int y = std::clamp<int>((parent.top + parent.bottom - height) / 2, int(info.rcWork.top),
                           (std::max)(int(info.rcWork.top), int(info.rcWork.bottom) - height));
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}
void StyleLabel(HWND hwnd, HFONT font, bool muted, bool onPanel) {
    SendMessageW(hwnd, WM_SETFONT, WPARAM(font), FALSE);
    DWORD_PTR data = 0;
    if (GetWindowSubclass(hwnd, ControlProc, kControlSubclass, &data)) {
        auto& state = *reinterpret_cast<Control*>(data); state.muted = muted; state.panel = onPanel;
        InvalidateRect(hwnd, nullptr, FALSE); return;
    }
    auto* state = new Control{Kind::Label}; state->muted = muted; state->panel = onPanel; Attach(hwnd, state);
}
void StyleControl(HWND hwnd, HFONT font, ButtonStyle style) {
    SendMessageW(hwnd, WM_SETFONT, WPARAM(font), FALSE);
    wchar_t cls[64]{}; GetClassNameW(hwnd, cls, 64);
    const auto windowStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!wcscmp(cls, L"Button")) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, (windowStyle & ~BS_TYPEMASK) | BS_OWNERDRAW);
        auto* state = new Control{Kind::Button}; state->style = style;
        const auto type = windowStyle & BS_TYPEMASK;
        state->checkable = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX;
        state->automatic = type == BS_AUTOCHECKBOX;
        Attach(hwnd, state);
    } else if (!wcscmp(cls, L"Edit")) {
        SetWindowTheme(hwnd, L"", L"");
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, windowStyle | WS_BORDER);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        Attach(hwnd, new Control{Kind::Edit});
    } else if (!wcscmp(cls, L"ComboBox")) {
        SetWindowTheme(hwnd, L"", L"");
        if ((windowStyle & CBS_DROPDOWNLIST) == CBS_DROPDOWN)
            SetWindowLongPtrW(hwnd, GWL_STYLE, windowStyle | WS_CLIPCHILDREN);
        SendMessageW(hwnd, CB_SETITEMHEIGHT, WPARAM(-1), 26);
        SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, 30);
        Attach(hwnd, new Control{Kind::Combo});
        COMBOBOXINFO combo{sizeof(combo)};
        if (GetComboBoxInfo(hwnd, &combo)) {
            if ((windowStyle & CBS_DROPDOWNLIST) == CBS_DROPDOWN && combo.hwndItem) {
                // Preserve native input/caret/IME; only the outer frame and arrow are painted above.
                SetWindowTheme(combo.hwndItem, L"", L"");
                SetWindowLongPtrW(combo.hwndItem, GWL_STYLE, GetWindowLongPtrW(combo.hwndItem, GWL_STYLE) & ~WS_BORDER);
                SetWindowLongPtrW(combo.hwndItem, GWL_EXSTYLE, GetWindowLongPtrW(combo.hwndItem, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
                SendMessageW(combo.hwndItem, WM_SETFONT, WPARAM(font), FALSE);
                SendMessageW(combo.hwndItem, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 2));
                SetWindowPos(combo.hwndItem, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                Attach(combo.hwndItem, new Control{Kind::ComboEdit});
            }
            if (combo.hwndList) SetWindowTheme(combo.hwndList, L"DarkMode_Explorer", nullptr);
        }
    } else if (!wcscmp(cls, WC_LISTVIEWW)) {
        SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(hwnd, color::kPanel); ListView_SetTextBkColor(hwnd, color::kPanel);
        ListView_SetTextColor(hwnd, color::kText);
        auto* state = new Control{Kind::List};
        // The list view owns its assigned image list and destroys it with the control.
        HIMAGELIST rowHeight = ImageList_Create(1, 30, ILC_COLOR32, 1, 1);
        ListView_SetImageList(hwnd, rowHeight, LVSIL_SMALL); Attach(hwnd, state);
        HWND header = ListView_GetHeader(hwnd); SendMessageW(header, WM_SETFONT, WPARAM(font), FALSE);
        Attach(header, new Control{Kind::Header});
    }
}
void SetButtonSelected(HWND hwnd, bool selected) {
    DWORD_PTR data = 0;
    if (!GetWindowSubclass(hwnd, ControlProc, kControlSubclass, &data)) return;
    auto& state = *reinterpret_cast<Control*>(data);
    if (state.kind != Kind::Button || state.selected == selected) return;
    state.selected = selected; InvalidateRect(hwnd, nullptr, FALSE);
}
void Card(HDC dc, RECT rect) { Round(dc, rect, color::kPanel, color::kBorder, 10); }
void Text(HDC dc, const wchar_t* text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    auto previous = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, flags); SelectObject(dc, previous);
}
bool HandleMessage(HWND, UINT message, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;
    if (message == WM_COMMAND && HIWORD(wp) == BN_CLICKED && lp) {
        DWORD_PTR data = 0;
        if (GetWindowSubclass(HWND(lp), ControlProc, kControlSubclass, &data)) {
            const auto& state = *reinterpret_cast<Control*>(data);
            // Owner drawing replaces the native checkbox type. Restore its
            // automatic state change before the parent reads BM_GETCHECK.
            if (state.checkable && state.automatic)
                SendMessageW(HWND(lp), BM_SETCHECK, state.checked == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
        }
    }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORSTATIC) {
        HDC dc = HDC(wp); SetTextColor(dc, IsWindowEnabled(HWND(lp)) ? color::kText : color::kTextDim);
        SetBkColor(dc, color::kBg); result = LRESULT(FieldBrush()); return true;
    }
    if (message == WM_MEASUREITEM) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_COMBOBOX) { item.itemHeight = 30; result = TRUE; return true; }
    }
    if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_BUTTON) {
            DWORD_PTR data = 0;
            if (!GetWindowSubclass(item.hwndItem, ControlProc, kControlSubclass, &data)) return false;
            const auto& state = *reinterpret_cast<Control*>(data);
            const bool disabled = (item.itemState & ODS_DISABLED) != 0, down = (item.itemState & ODS_SELECTED) != 0;
            COLORREF fill = color::kPanelHi, edge = color::kBorder, ink = color::kText;
            if (state.style == ButtonStyle::Tab) {
                fill = state.selected ? color::kPanelHi : color::kPanel;
                edge = state.selected ? color::kAccent : color::kBorder;
                ink = state.selected ? color::kText : color::kTextDim;
            }
            if (state.style == ButtonStyle::Primary) { fill = RGB(48, 104, 176); edge = color::kAccent; }
            if (state.style == ButtonStyle::Danger) { fill = RGB(53, 30, 36); edge = RGB(104, 53, 62); ink = color::kBad; }
            if (state.hot && !disabled) {
                edge = state.style == ButtonStyle::Danger ? color::kBad : color::kAccent;
                if (state.style == ButtonStyle::Secondary) fill = RGB(40, 47, 62);
                if (state.style == ButtonStyle::Primary) fill = RGB(58, 123, 202);
                if (state.style == ButtonStyle::Tab) { fill = color::kPanelHi; ink = color::kText; }
            }
            if (down && !disabled) fill = color::kAccentDim;
            if (disabled) { fill = color::kPanel; edge = color::kBorder; ink = color::kTextDim; }
            Fill(item.hDC, item.rcItem, state.style == ButtonStyle::Tab ? color::kBg : color::kPanel);
            Round(item.hDC, item.rcItem, fill, (item.itemState & ODS_FOCUS) ? color::kAccent : edge, 6);
            RECT text = item.rcItem; if (down) OffsetRect(&text, 0, 1);
            if (state.checkable) {
                const int top = (text.top + text.bottom - 16) / 2;
                RECT box{text.left + 10, top, text.left + 26, top + 16};
                Round(item.hDC, box, color::kBg, state.checked == BST_CHECKED && !disabled ? color::kAccent : color::kBorderGlow, 3);
                if (state.checked == BST_CHECKED) {
                    HPEN pen = CreatePen(PS_SOLID, 2, disabled ? color::kTextDim : color::kAccent);
                    auto old = SelectObject(item.hDC, pen);
                    MoveToEx(item.hDC, box.left + 3, box.top + 8, nullptr);
                    LineTo(item.hDC, box.left + 7, box.top + 12); LineTo(item.hDC, box.left + 13, box.top + 4);
                    SelectObject(item.hDC, old); DeleteObject(pen);
                }
                text.left += 34; text.right -= 6;
            }
            Text(item.hDC, Caption(item.hwndItem).c_str(), text, ControlFont(item.hwndItem), ink,
                 (state.checkable ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (state.style == ButtonStyle::Tab && (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
                RECT focus = item.rcItem; InflateRect(&focus, -5, -5); DrawFocusRect(item.hDC, &focus);
            }
            result = TRUE; return true;
        }
        if (item.CtlType == ODT_COMBOBOX) {
            const bool selected = (item.itemState & ODS_SELECTED) != 0;
            Fill(item.hDC, item.rcItem, selected ? color::kAccentDim : color::kPanel);
            std::wstring text;
            if (item.itemID != UINT(-1)) {
                const auto n = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
                if (n >= 0 && n < 4096) {
                    text.resize(size_t(n) + 1); SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, LPARAM(text.data()));
                    text.resize(size_t(n));
                }
            }
            RECT label = item.rcItem; label.left += 10; label.right -= 6;
            Text(item.hDC, text.c_str(), label, ControlFont(item.hwndItem), color::kText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            result = TRUE; return true;
        }
    }
    if (message == WM_NOTIFY) {
        const auto* header = reinterpret_cast<NMHDR*>(lp);
        DWORD_PTR data = 0;
        if (header->code != NM_CUSTOMDRAW || !GetWindowSubclass(header->hwndFrom, ControlProc, kControlSubclass, &data) ||
            reinterpret_cast<Control*>(data)->kind != Kind::List) return false;
        auto& draw = *reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) { result = CDRF_NOTIFYITEMDRAW; return true; }
        if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const auto index = int(draw.nmcd.dwItemSpec);
            const bool selected = (ListView_GetItemState(header->hwndFrom, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            const COLORREF background = selected ? RGB(36, 63, 96) : index % 2 ? RGB(29, 33, 43) : color::kPanel;
            // List-view custom draw does not guarantee nmcd.rc for item stages.
            RECT row{}, client{};
            ListView_GetItemRect(header->hwndFrom, index, &row, LVIR_BOUNDS);
            GetClientRect(header->hwndFrom, &client); row.left = client.left; row.right = client.right;
            Fill(draw.nmcd.hdc, row, background);
            const int count = Header_GetItemCount(ListView_GetHeader(header->hwndFrom));
            for (int i = 0; i < count; ++i) {
                RECT rect{}; ListView_GetSubItemRect(header->hwndFrom, index, i, LVIR_LABEL, &rect);
                wchar_t text[512]{}; ListView_GetItemText(header->hwndFrom, index, i, text, 512);
                rect.left += 10; rect.right -= 8;
                Text(draw.nmcd.hdc, text, rect, ControlFont(header->hwndFrom), i == 0 ? color::kTextDim : color::kText,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if (selected) {
                auto edge = row; edge.right = edge.left + 3; Fill(draw.nmcd.hdc, edge, color::kAccent);
            }
            result = CDRF_SKIPDEFAULT; return true;
        }
    }
    return false;
}
bool ProcessDialogMessage(MSG& message) {
    const auto windows = Dialogs();
    for (HWND window : windows)
        if (IsWindow(window) && (message.hwnd == window || IsChild(window, message.hwnd)) && IsDialogMessageW(window, &message))
            return true;
    return false;
}
} // namespace ui
