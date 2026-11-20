#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../Plug/RecoveryPanel.h"
#include "../Plug/ViewPanel.h"
#include "../Plug/UiTheme.h"
#include <algorithm>

// Isolated preview of the real child panels. No target, pipe, or DLL injection.
LRESULT CALLBACK Preview(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch(message) {
    case WM_CREATE:
        recoveryui::CreatePanel(window, [](std::string&) { return false; }, [](const std::wstring&) {});
        viewui::CreatePanel(window, [](std::string&) { return false; }, [](const std::string&) {});
        recoveryui::ShowPanel(true); viewui::ShowPanel(true);
        SetTimer(window, 42, 120000, nullptr); return 0;
    case WM_SIZE: {
        RECT rect{}; GetClientRect(window,&rect);
        const int viewWidth=(std::max)(148,(std::min)(190,int(rect.right-24)/4));
        const int width=rect.right-32-viewWidth;
        recoveryui::MovePanel(12,12,width);
        viewui::MovePanel(20+width,12,viewWidth,recoveryui::kHeight);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint; auto dc=BeginPaint(window,&paint); RECT rect{}; GetClientRect(window,&rect);
        FillRect(dc,&rect,ui::Background()); EndPaint(window,&paint); return 0;
    }
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_TIMER: if(wp==42) { DestroyWindow(window); return 0; } break;
    case WM_DESTROY: recoveryui::Shutdown(); viewui::Shutdown(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    WNDCLASSW cls{}; cls.hInstance=instance; cls.lpfnWndProc=Preview; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.lpszClassName=L"Poe2ViewPanelPreview"; RegisterClassW(&cls);
    auto window=CreateWindowExW(0,cls.lpszClassName,L"Layout preview",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,660,260,nullptr,nullptr,instance,nullptr);
    ShowWindow(window,SW_SHOW); UpdateWindow(window);
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
