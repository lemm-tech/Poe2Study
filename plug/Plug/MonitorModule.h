#pragma once
#include <windows.h>
#include <string>
#include "../Common/MonitorInterface.h"

namespace monitorimage {
struct Module {
    uint64_t base = 0;
    std::wstring path;
    DWORD error = ERROR_SUCCESS;
    uint32_t matches = 0;
};

struct Identity {
    std::wstring path;
    uint64_t base = 0;
    uint32_t timestamp = 0, imageSize = 0;
    bool headers = false, exports = false, interfaceFound = false;
    bool interfaceRead = false, codeView = false;
    GUID pdbGuid{};
    uint32_t pdbAge = 0, shutdownRva = 0;
    monitorabi::Info interfaceInfo{0, 0, 0, 0, 0, 0, 0};
    std::string stage;
    DWORD error = ERROR_SUCCESS;
};

enum class Compatibility { Compatible, Incompatible, Unknown };
struct Check {
    Compatibility compatibility = Compatibility::Unknown;
    bool sameBuild = false;
    std::string reason, diagnostic;
};

Module Find(DWORD pid, const wchar_t* fileName);
Identity ReadLocal(const std::wstring& path);
Identity ReadRemote(HANDLE process, const Module& module);
Check Compare(const Identity& local, const Identity& remote);
std::string Describe(const Identity& identity);
} // namespace monitorimage
