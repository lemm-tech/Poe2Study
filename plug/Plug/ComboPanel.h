#pragma once
#include <windows.h>
namespace combo {
void ShowSettings(HWND owner);
// Locks configuration from combined-test startup through skill release/cleanup.
void SetNavigationTestActive(bool active);
void ShutdownCombo();
} // namespace combo
