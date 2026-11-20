#pragma once
#include <windows.h>
#include <functional>
#include <string>
namespace follow {
void CreatePanel(HWND parent, std::function<bool(std::string&)> canStart);
void ShowPanel(bool visible, const RECT& bounds);
void ShutdownPanel();
bool PartyTeleportPending();
} // namespace follow
