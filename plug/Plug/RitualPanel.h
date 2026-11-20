#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include "WorldArea.h"

namespace ritual { struct Options; }

namespace ritualui {
struct Target {
    int32_t x = 0, y = 0;
    // Empty for manually entered coordinates; selected rows retain their scan instance.
    poe::WorldAreaSnapshot area;
};
struct NavigationState {
    bool canStart = false, running = false;
    std::string text, detail;
};
void CreatePanel(HWND parent, std::function<bool(const Target&, std::string&)> navigate,
                 std::function<void()> stop, std::function<NavigationState()> state,
                 std::function<bool(const ritual::Options&, std::string&)> automate);
void ShowPanel(bool visible, const RECT& bounds);
void Shutdown();
} // namespace ritualui
