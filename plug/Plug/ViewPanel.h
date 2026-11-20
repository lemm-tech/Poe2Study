#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace viewui {
void CreatePanel(HWND parent, std::function<bool(std::string&)> canChange,
                 std::function<void(const std::string&)> log);
void MovePanel(int x, int y, int width, int height);
void ShowPanel(bool show);
void Shutdown();
}
