#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace poelog {
// One retained UTF-8 JSONL file per run. WriteFile avoids losing a CRT buffer
// on process failure; readers can open an active log. Parts are capped at 8 MiB.
class RunDiagnostics {
public:
    explicit RunDiagnostics(const std::wstring& directory, const wchar_t* prefix = L"run") {
        if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
        SYSTEMTIME t{}; GetLocalTime(&t);
        static std::atomic_uint sequence{0};
        wchar_t name[160]{};
        swprintf_s(name, L"\\%s_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u",
            prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, GetCurrentProcessId(), ++sequence);
        stem_ = directory + name;
        OpenPart();
    }
    ~RunDiagnostics() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    RunDiagnostics(const RunDiagnostics&) = delete;
    RunDiagnostics& operator=(const RunDiagnostics&) = delete;
    const std::wstring& Path() const { return path_; }
    bool Good() const { return file_ != INVALID_HANDLE_VALUE; }
    static std::string Utf8(const std::wstring& value) {
        const auto n = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
        std::string out(n, 0);
        if (n) WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), n, nullptr, nullptr);
        return out;
    }
    static std::string Json(const std::string& value) {
        std::string out = "\"";
        for (const unsigned char ch : value) {
            if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
            else if (ch < 32) { char escaped[8]{}; sprintf_s(escaped, "\\u%04x", unsigned(ch)); out += escaped; }
            else out += char(ch);
        }
        return out + '"';
    }
    void Write(const std::string& event, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        Line(event, message, 0);
    }
    // Repeated values produce a heartbeat every five seconds, with the count
    // of suppressed repeats. Changed values are recorded immediately.
    void Changed(const std::string& key, const std::string& message, uint64_t intervalMs = 5000) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& last = repeated_[key]; const auto now = GetTickCount64();
        if (last.have && last.message == message && now - last.at < intervalMs) { ++last.suppressed; return; }
        Line(key, message, last.suppressed);
        last = {true, now, 0, message};
    }
private:
    struct Repeated { bool have = false; uint64_t at = 0, suppressed = 0; std::string message; };
    std::mutex mutex_;
    std::map<std::string, Repeated> repeated_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring stem_, path_;
    unsigned part_ = 0;
    uint64_t started_ = GetTickCount64(), bytes_ = 0;
    void OpenPart() {
        ++part_;
        const auto next = stem_ + (part_ == 1 ? L"" : L"_part" + std::to_wstring(part_)) + L".jsonl";
        file_ = CreateFileW(next.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ != INVALID_HANDLE_VALUE) { if (path_.empty()) path_ = next; bytes_ = 0; }
    }
    void Line(const std::string& event, const std::string& message, uint64_t suppressed) {
        if (file_ == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME t{}; GetLocalTime(&t); char stamp[40]{};
        sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        const auto line = "{\"time\":" + Json(stamp) + ",\"elapsed_ms\":" + std::to_string(GetTickCount64() - started_) +
            ",\"event\":" + Json(event) + ",\"message\":" + Json(message) +
            ",\"suppressed\":" + std::to_string(suppressed) + "}\r\n";
        if (bytes_ && bytes_ + line.size() > 8 * 1024 * 1024) {
            CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; OpenPart();
            if (file_ == INVALID_HANDLE_VALUE) return;
        }
        DWORD wrote = 0;
        if (!WriteFile(file_, line.data(), DWORD(line.size()), &wrote, nullptr) || wrote != line.size()) {
            CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; return;
        }
        bytes_ += wrote;
    }
};
} // namespace poelog
