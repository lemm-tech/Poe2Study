#include "InstanceMutex.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace instance {
namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE v = nullptr) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Entry {
    HANDLE value;
    uintptr_t handleCount, pointerCount;
    ULONG access, type, attributes, reserved;
};
static_assert(sizeof(Entry) == 40, "x64 handle snapshot required");
struct Snapshot { uintptr_t count, reserved; Entry entries[1]; };
struct UnicodeName { USHORT length, maximum; PWSTR buffer; };
using QueryProcess = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
using QueryObject = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
using CompareObjects = LONG(NTAPI*)(HANDLE, HANDLE);
struct Native {
    QueryProcess process = reinterpret_cast<QueryProcess>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    QueryObject object = reinterpret_cast<QueryObject>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    CompareObjects compare = reinterpret_cast<CompareObjects>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCompareObjects"));
};
bool Entries(const Native& api, HANDLE process, std::vector<Entry>& entries) {
    if (!api.process) return false;
    ULONG size = 65536;
    for (int retry = 0; retry < 10 && size <= 64 * 1024 * 1024; ++retry) {
        std::vector<unsigned char> bytes(size);
        ULONG needed = 0;
        const LONG status = api.process(process, 51, bytes.data(), size, &needed);
        if (status >= 0) {
            const auto* snap = reinterpret_cast<const Snapshot*>(bytes.data());
            if (snap->count > (size - offsetof(Snapshot, entries)) / sizeof(Entry)) return false;
            entries.assign(snap->entries, snap->entries + snap->count);
            return true;
        }
        // STATUS_INFO_LENGTH_MISMATCH / STATUS_BUFFER_TOO_SMALL only.
        if (ULONG(status) != 0xC0000004 && ULONG(status) != 0xC0000023) return false;
        size = (std::max)(size * 2, needed + 4096);
    }
    return false;
}
ULONG MutexType(const Native& api) {
    Handle mutex(CreateMutexW(nullptr, FALSE, nullptr));
    if (!mutex.value) return 0;
    std::vector<Entry> entries;
    if (!Entries(api, GetCurrentProcess(), entries)) return 0;
    for (const auto& entry : entries) if (entry.value == mutex.value) return entry.type;
    return 0;
}
std::wstring ObjectName(const Native& api, HANDLE handle) {
    if (!api.object) return {};
    std::vector<unsigned char> bytes(65536);
    ULONG needed = 0;
    if (api.object(handle, 1, bytes.data(), ULONG(bytes.size()), &needed) < 0) return {};
    const auto* name = reinterpret_cast<const UnicodeName*>(bytes.data());
    const auto begin = reinterpret_cast<uintptr_t>(bytes.data());
    const auto str = reinterpret_cast<uintptr_t>(name->buffer);
    if (!name->length || name->length % sizeof(wchar_t) || str < begin ||
        str + name->length > begin + bytes.size()) return {};
    return std::wstring(name->buffer, name->length / sizeof(wchar_t));
}
Result Run(DWORD pid, uint64_t start, bool close) {
    Result result;
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid));
    if (!process.value || !start || gametarget::CreationTime(process.value) != start ||
        WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) {
        result.error = L"游戏已退出、进程身份已变化或无法读取句柄（错误 " + std::to_wstring(GetLastError()) + L"）";
        return result;
    }
    Native api;
    const ULONG type = MutexType(api);
    std::vector<Entry> entries;
    if (!type || !Entries(api, process.value, entries) || (close && !api.compare)) {
        result.error = L"无法读取进程互斥体快照；未关闭句柄";
        return result;
    }
    for (const auto& entry : entries) {
        if (entry.type != type) continue;
        HANDLE local = nullptr;
        if (!DuplicateHandle(process.value, entry.value, GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        Handle copy(local);
        const std::wstring name = ObjectName(api, copy.value);
        if (!IsInstanceLock(name)) continue;
        result.matches.push_back({reinterpret_cast<uintptr_t>(entry.value), name});
        if (!close) continue;
        // Recheck identity of this handle immediately before closing its source.
        HANDLE check = nullptr;
        if (!DuplicateHandle(process.value, entry.value, GetCurrentProcess(), &check, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            result.error = L"复核句柄失败"; break;
        }
        Handle recheck(check);
        if (api.compare(copy.value, recheck.value) < 0) { result.error = L"句柄已复用，操作已停止"; break; }
        HANDLE removed = nullptr;
        if (!DuplicateHandle(process.value, entry.value, GetCurrentProcess(), &removed, 0, FALSE,
                             DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) {
            result.error = L"关闭多开互斥体失败（错误 " + std::to_wstring(GetLastError()) + L"）"; break;
        }
        Handle removedCopy(removed);
        ++result.closed;
    }
    result.ok = result.error.empty();
    return result;
}
} // namespace
bool IsInstanceLock(const std::wstring& name) {
    const auto slash = name.find_last_of(L'\\');
    const std::wstring leaf = name.substr(slash == std::wstring::npos ? 0 : slash + 1);
    return leaf == L"PathOfExileSingleInstance" || leaf == L"PoERunMutexA" || leaf == L"PoERunMutexB";
}
Result Inspect(DWORD pid, uint64_t start) { return Run(pid, start, false); }
Result Release(DWORD pid, uint64_t start) {
    Result result = Run(pid, start, true);
    if (result.ok) {
        const auto remaining = Inspect(pid, start);
        if (!remaining.ok || !remaining.matches.empty()) {
            result.ok = false;
            result.error = L"关闭后复核未通过；游戏可能重新创建了多开互斥体";
        }
    }
    return result;
}
} // namespace instance
