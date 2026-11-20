// ============================================================
// OlaDriver.cpp —— 纯 ReadProcessMemory 实现
//
// 历史：这个文件原来包的是 OLA（欧拉）插件 SDK，需要联网注册。
// 授权过期后 sdk.Reg() 会静默卡死，整个 Plug.exe 无法退出
// （实测挂 90 秒以上，既不返回也不报错）。
//
// 现在改成纯 RPM：不联网、不授权、不加载 44MB 的 OLAPlug DLL。
// 类名保留 ola::OlaDriver 只是为了不动上层调用点。
// ============================================================
#include "OlaDriver.h"
#include "Logger.h"

#include <cstring>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

namespace ola {

namespace {

constexpr size_t kMaxTextLen = 1024;

// 读原始字节
inline bool RpmRead(HANDLE h, uint64_t addr, void* out, size_t size)
{
    if (!h || h == INVALID_HANDLE_VALUE || !out || size == 0) return false;
    SIZE_T n = 0;
    return ReadProcessMemory(h, (LPCVOID)addr, out, size, &n) && n == size;
}

// 读以 \0 结尾的文本（ANSI 或 UTF-16），统一返回 UTF-8
inline std::string RpmReadText(HANDLE h, uint64_t addr, bool utf16)
{
    std::string out;
    if (!h || addr == 0) return out;

    if (utf16) {
        wchar_t wbuf[kMaxTextLen] = { 0 };
        size_t got = 0;
        for (size_t i = 0; i + 1 < kMaxTextLen; ++i) {
            wchar_t c = 0;
            if (!RpmRead(h, addr + i * 2, &c, sizeof(c))) break;
            wbuf[i] = c;
            got = i + 1;
            if (c == 0) break;
        }
        if (got == 0) return out;

        const int need = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
        if (need > 1) {
            out.resize((size_t)need - 1);
            WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, &out[0], need, nullptr, nullptr);
        }
    }
    else {
        char buf[kMaxTextLen] = { 0 };
        for (size_t i = 0; i + 1 < kMaxTextLen; ++i) {
            char c = 0;
            if (!RpmRead(h, addr + i, &c, 1)) break;
            buf[i] = c;
            if (c == 0) break;
        }
        out = buf;
    }
    return out;
}

// 取模块基址
inline uint64_t RpmModuleBase(uint32_t pid, const std::string& moduleName)
{
    if (pid == 0 || moduleName.empty()) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    wchar_t want[MAX_PATH] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, moduleName.c_str(), -1, want, MAX_PATH);

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    uint64_t base = 0;

    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, want) == 0) {
                base = (uint64_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

} // namespace

OlaDriver::OlaDriver() = default;

OlaDriver::~OlaDriver()
{
    Shutdown();
}

bool OlaDriver::InitializeLocal(uint32_t pid)
{
    if (pid == 0) {
        LOGE("[Mem] PID=0，忽略");
        return false;
    }
    if (m_ready && m_hproc && m_pid == pid) {
        return true;
    }

    if (m_hproc) {
        CloseHandle((HANDLE)m_hproc);
        m_hproc = nullptr;
        m_ready = false;
    }

    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    }
    if (!h) {
        LOGE("[Mem] OpenProcess 失败 (err=%lu) —— 需要管理员权限",
             (unsigned long)GetLastError());
        return false;
    }

    m_hproc = (void*)h;
    m_pid = pid;
    m_mode = MemoryMode::Rpm;
    m_ready = true;

    LOGI("[Mem] 已连接 pid=%lu（ReadProcessMemory 后端）", (unsigned long)pid);
    return true;
}

bool OlaDriver::Initialize(const app::OlaCredentials& creds)
{
    // creds 已无用，保留签名兼容旧调用点。
    (void)creds;
    return true;
}

bool OlaDriver::Attach(uint32_t pid, MemoryMode preferred, MemoryMode* actual)
{
    (void)preferred;
    const bool ok = InitializeLocal(pid);
    if (actual) *actual = m_mode;
    return ok;
}

bool OlaDriver::BindWindowByHandle(uint64_t hwnd)
{
    (void)hwnd;
    return false;
}

void OlaDriver::Detach()
{
    if (m_hproc) {
        CloseHandle((HANDLE)m_hproc);
        m_hproc = nullptr;
    }
    m_pid = 0;
    m_ready = false;
    m_mode = MemoryMode::Rpm;
}

void OlaDriver::Shutdown()
{
    Detach();
}

bool OlaDriver::ReadRaw(uint64_t addr, void* out, size_t size) const
{
    return RpmRead((HANDLE)m_hproc, addr, out, size);
}

bool OlaDriver::ReadU8(uint64_t addr, uint8_t& out) const
{
    return RpmRead((HANDLE)m_hproc, addr, &out, 1);
}

bool OlaDriver::ReadU32(uint64_t addr, uint32_t& out) const
{
    return RpmRead((HANDLE)m_hproc, addr, &out, 4);
}

bool OlaDriver::ReadU64(uint64_t addr, uint64_t& out) const
{
    return RpmRead((HANDLE)m_hproc, addr, &out, 8);
}

std::string OlaDriver::ReadAnsi(uint64_t addr) const
{
    return RpmReadText((HANDLE)m_hproc, addr, false);
}

std::string OlaDriver::ReadUtf16(uint64_t addr) const
{
    return RpmReadText((HANDLE)m_hproc, addr, true);
}

bool OlaDriver::ReadBlob(uint64_t addr, size_t size, std::vector<uint8_t>& out) const
{
    out.clear();
    if (addr == 0 || size == 0) return false;

    out.resize(size);
    if (!RpmRead((HANDLE)m_hproc, addr, out.data(), size)) {
        out.clear();
        return false;
    }
    return true;
}

uint64_t OlaDriver::ModuleBase(const std::string& moduleName) const
{
    return RpmModuleBase(m_pid, moduleName);
}

} // namespace ola