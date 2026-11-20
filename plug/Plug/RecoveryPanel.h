#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace recoveryui {
constexpr int kHeight = 194;
void CreatePanel(HWND parent, std::function<bool(std::string&)> canEnable, std::function<void(const std::wstring&)> log);
void ShowPanel(bool visible);
void MovePanel(int x, int y, int width);
void Shutdown();
} // namespace recoveryui
