#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ComboConfig.h"
#include "../Common/AppConfig.h"
#include <cwchar>
#include <sstream>
#include <stdexcept>

namespace combo {
namespace {
std::wstring Wide(const std::string& s) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!size && !s.empty()) throw std::runtime_error("Invalid UTF-8 config text");
    std::wstring out(size, 0);
    if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), size);
    return out;
}
std::string Utf8(const std::wstring& s) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    if (!size && !s.empty()) throw std::runtime_error("Invalid UTF-16 config text");
    std::string out(size, 0);
    if (size) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}
void Validate(const std::vector<Row>& rows) {
    if (rows.size() > kMaxRows) throw std::runtime_error("Too many combo rows (maximum 256)");
    std::set<uint64_t> keys;
    for (const auto& r : rows) {
        if (!r.key || !keys.insert(r.key).second || r.internalId.empty() || r.internalId.size() > 200 ||
            !std::all_of(r.internalId.begin(), r.internalId.end(), [](char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_';
            }) || r.name.empty() || r.name.size() > 512 || r.name.find_first_of("\r\n=[]") != std::string::npos ||
            r.slotHint >= 65 || unsigned(r.usage) > 1 || unsigned(r.environment) > 1 ||
            r.intervalMs > kMaxTimeMs || r.chargeMs > kMaxTimeMs || r.distance > 0x1000 * 23)
            throw std::runtime_error("Invalid combo row data");
    }
}
std::wstring ReadText(const std::wstring& path, const std::wstring& section, const wchar_t* key) {
    wchar_t value[1024]{};
    const auto n = GetPrivateProfileStringW(section.c_str(), key, L"", value, 1024, path.c_str());
    if (!n || n >= 1023) throw std::runtime_error("Missing/oversized combo config value");
    return value;
}
uint64_t Number(const std::wstring& path, const std::wstring& section, const wchar_t* key, uint64_t max) {
    const auto s = ReadText(path, section, key);
    if (s.find_first_not_of(L"0123456789") != std::wstring::npos) throw std::runtime_error("Invalid config number");
    size_t used = 0; const auto n = std::stoull(s, &used);
    if (used != s.size() || n > max) throw std::runtime_error("Config number out of range");
    return n;
}
} // namespace

std::wstring DefaultConfigPath() {
    return app::Config().RuntimeFileW(L"combo.ini");
}
bool LoadConfig(const std::wstring& path, std::vector<Row>& rows, std::string& error) {
    error.clear();
    try {
        if (path.empty()) throw std::runtime_error("Config path unavailable");
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("Cannot access combo config");
            rows.clear(); return true;
        }
        if (Number(path, L"combo", L"version", 1) != 1) throw std::runtime_error("Unsupported combo config version");
        const auto count = Number(path, L"combo", L"count", kMaxRows);
        std::vector<Row> loaded;
        for (uint64_t i = 0; i < count; ++i) {
            const auto section = L"skill" + std::to_wstring(i);
            Row r;
            r.key = Number(path, section, L"key", UINT64_MAX);
            r.internalId = Utf8(ReadText(path, section, L"internalId"));
            r.name = Utf8(ReadText(path, section, L"name"));
            r.slotHint = uint32_t(Number(path, section, L"slotHint", 64));
            r.usage = Usage(Number(path, section, L"usage", 1));
            r.environment = Environment(Number(path, section, L"environment", 1));
            r.intervalMs = uint32_t(Number(path, section, L"intervalMs", kMaxTimeMs));
            r.chargeMs = uint32_t(Number(path, section, L"chargeMs", kMaxTimeMs));
            r.distance = uint32_t(Number(path, section, L"distance", 0x1000 * 23));
            loaded.push_back(std::move(r));
        }
        Validate(loaded); rows = std::move(loaded); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool SaveConfig(const std::wstring& path, const std::vector<Row>& rows, std::string& error) {
    error.clear();
    std::wstring temp;
    try {
        if (path.empty()) throw std::runtime_error("Config path unavailable");
        Validate(rows);
        std::wostringstream s;
        s << L"; POE2 combo settings. No game-session counters are persisted.\r\n[combo]\r\nversion=1\r\ncount=" << rows.size() << L"\r\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            s << L"\r\n[skill" << i << L"]\r\nkey=" << r.key << L"\r\ninternalId=" << Wide(r.internalId)
              << L"\r\nname=" << Wide(r.name) << L"\r\nslotHint=" << r.slotHint
              << L"\r\nusage=" << unsigned(r.usage) << L"\r\nenvironment=" << unsigned(r.environment)
              << L"\r\nintervalMs=" << r.intervalMs << L"\r\nchargeMs=" << r.chargeMs
              << L"\r\ndistance=" << r.distance << L"\r\n";
        }
        const auto body = std::wstring(1, wchar_t(0xFEFF)) + s.str();
        temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create combo config temporary file");
        DWORD written = 0; const auto bytes = DWORD(body.size() * sizeof(wchar_t));
        const bool ok = WriteFile(file, body.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot commit combo config");
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
        return true;
    } catch (const std::exception& e) {
        if (!temp.empty()) DeleteFileW(temp.c_str());
        error = e.what(); return false;
    }
}
} // namespace combo
