#pragma once
#include "GameTarget.h"
#include <vector>

namespace instance {
struct MutexInfo { uintptr_t handle = 0; std::wstring name; };
struct Result {
    bool ok = false;
    unsigned closed = 0;
    std::vector<MutexInfo> matches;
    std::wstring error;
};
// Only exact, observed instance-lock names qualify. No substring matching.
bool IsInstanceLock(const std::wstring& objectName);
Result Inspect(DWORD pid, uint64_t start);
Result Release(DWORD pid, uint64_t start);
} // namespace instance
