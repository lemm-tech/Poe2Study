#include "GameLauncher.h"
#include <filesystem>
#include <vector>

namespace sessions::game {
std::wstring NormalizePath(const std::wstring& path) {
    const auto first = path.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    auto text = path.substr(first, path.find_last_not_of(L" \t\r\n") - first + 1);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
        text = text.substr(1, text.size() - 2);
    return text;
}
std::wstring LoadPath(const std::wstring& settingsFile) {
    std::vector<wchar_t> text(32768);
    const auto length = GetPrivateProfileStringW(L"launcher", L"gamePath", L"", text.data(),
                                                 DWORD(text.size()), settingsFile.c_str());
    return std::wstring(text.data(), length);
}
bool SavePath(const std::wstring& settingsFile, const std::wstring& path, DWORD& error) {
    error = ERROR_SUCCESS;
    // This separate file is UTF-16 so paths survive outside the active code page.
    // Build-time config.ini copying must not replace the saved launcher path.
    HANDLE file = CreateFileW(settingsFile.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE;
    if (!ok) error = GetLastError();
    if (ok && size.QuadPart == 0) {
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) != FALSE;
        if (!ok) error = GetLastError();
        else if (written != sizeof(bom)) { ok = false; error = ERROR_WRITE_FAULT; }
    }
    CloseHandle(file);
    if (!ok) return false;
    const auto normalized = NormalizePath(path);
    if (!WritePrivateProfileStringW(L"launcher", L"gamePath", normalized.c_str(), settingsFile.c_str())) {
        error = GetLastError(); return false;
    }
    return true;
}
std::wstring ErrorText(DWORD error) {
    wchar_t text[1024]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   error, 0, text, DWORD(_countof(text)), nullptr);
    std::wstring detail = text;
    const auto end = detail.find_last_not_of(L" \t\r\n");
    if (end != std::wstring::npos) detail.resize(end + 1);
    return L"错误 " + std::to_wstring(error) + (detail.empty() ? L"" : L"：" + detail);
}
LaunchResult Start(const std::wstring& path, const std::wstring& panelDirectory) {
    LaunchResult result;
    const auto normalized = NormalizePath(path);
    if (normalized.empty()) { result.error = L"请先填写或浏览选择游戏可执行文件（.exe）。"; return result; }
    if (normalized.find_first_of(L"\"\r\n") != std::wstring::npos) {
        result.error = L"游戏路径格式无效，请只填写 .exe 文件路径。"; return result;
    }
    std::filesystem::path executable(normalized);
    if (executable.is_relative()) executable = std::filesystem::path(panelDirectory) / executable;
    std::error_code ec;
    executable = std::filesystem::absolute(executable, ec).lexically_normal();
    if (ec) { result.error = L"解析游戏路径失败（" + ErrorText(DWORD(ec.value())) + L"）"; return result; }
    result.path = executable.wstring();
    const auto attributes = GetFileAttributesW(result.path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.error = L"无法访问游戏文件（" + ErrorText(GetLastError()) + L"）"; return result;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || _wcsicmp(executable.extension().c_str(), L".exe")) {
        result.error = L"请选择游戏的 .exe 可执行文件，不能填写文件夹或快捷方式。"; return result;
    }
    std::wstring command = L"\"" + result.path + L"\"";
    const auto directory = executable.parent_path().wstring();
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(result.path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         directory.c_str(), &startup, &child)) {
        result.error = L"启动游戏失败（" + ErrorText(GetLastError()) + L"）"; return result;
    }
    result.pid = child.dwProcessId;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return result;
}
} // namespace sessions::game
