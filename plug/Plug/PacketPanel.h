#pragma once
#include <windows.h>
#include <functional>
#include <string>
namespace packetui {
void CreatePanel(HWND parent, std::function<bool(std::string&)> canSendAction = {});
void ShowPanel(bool visible, const RECT& bounds);
bool Busy();
void Shutdown();
} // namespace packetui
