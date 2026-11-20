#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <windows.h>
namespace pipeio
{
inline uint64_t NewId()
{
    FILETIME f{};
    GetSystemTimeAsFileTime(&f);
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return ((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) ^ uint64_t(q.QuadPart) ^
           uint64_t(GetCurrentProcessId());
}
inline std::wstring Normalize(std::wstring n)
{
    if (n.rfind(L"\\\\.\\pipe\\", 0) != 0 && n.rfind(L"\\\\?\\pipe\\", 0) != 0)
        n = L"\\\\.\\pipe\\" + n;
    return n;
}
enum class ReadResult { Data, Idle, Failed };

// Keep one overlapped read pending across heartbeat ticks. The buffer and event
// must outlive the kernel operation, including cancellation during shutdown.
class Reader
{
    HANDLE m_pipe, m_stop;
    OVERLAPPED m_io{};
    bool m_pending = false;
    std::array<uint8_t, 8192> m_buffer{};

  public:
    Reader(HANDLE pipe, HANDLE stop) : m_pipe(pipe), m_stop(stop)
    {
        m_io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    ~Reader()
    {
        Cancel();
        if (m_io.hEvent)
            CloseHandle(m_io.hEvent);
    }
    void Cancel()
    {
        if (!m_pending)
            return;
        CancelIoEx(m_pipe, &m_io);
        DWORD ignored = 0;
        GetOverlappedResult(m_pipe, &m_io, &ignored, TRUE);
        m_pending = false;
    }
    const uint8_t *Data() const { return m_buffer.data(); }
    ReadResult Read(DWORD timeout, DWORD &received)
    {
        received = 0;
        if (!m_io.hEvent || WaitForSingleObject(m_stop, 0) != WAIT_TIMEOUT)
            return ReadResult::Failed;
        if (!m_pending)
        {
            ResetEvent(m_io.hEvent);
            if (ReadFile(m_pipe, m_buffer.data(), DWORD(m_buffer.size()), nullptr, &m_io))
            {
                const BOOL ok = GetOverlappedResult(m_pipe, &m_io, &received, FALSE);
                return ok && received ? ReadResult::Data : ReadResult::Failed;
            }
            if (GetLastError() != ERROR_IO_PENDING)
                return ReadResult::Failed;
            m_pending = true;
        }
        HANDLE events[] = {m_stop, m_io.hEvent};
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, timeout);
        if (result == WAIT_TIMEOUT)
            return ReadResult::Idle;
        if (result != WAIT_OBJECT_0 + 1)
        {
            Cancel();
            return ReadResult::Failed;
        }
        const BOOL ok = GetOverlappedResult(m_pipe, &m_io, &received, FALSE);
        m_pending = false;
        return ok && received ? ReadResult::Data : ReadResult::Failed;
    }
};
inline bool Transfer(HANDLE pipe, void *data, DWORD n, bool write, HANDLE stop, DWORD timeout = 3000)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;
    auto p = static_cast<uint8_t *>(data);
    bool ok = true;
    const ULONGLONG started = GetTickCount64();
    while (n)
    {
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT || (timeout != INFINITE && elapsed >= timeout))
        {
            ok = false;
            break;
        }
        ResetEvent(ov.hEvent);
        DWORD done = 0;
        BOOL r = write ? WriteFile(pipe, p, n, &done, &ov) : ReadFile(pipe, p, n, &done, &ov);
        if (!r && GetLastError() == ERROR_IO_PENDING)
        {
            HANDLE wait[2] = {stop, ov.hEvent};
            const DWORD remaining = timeout == INFINITE ? INFINITE : timeout - DWORD(elapsed);
            DWORD w = WaitForMultipleObjects(2, wait, FALSE, remaining);
            if (w != WAIT_OBJECT_0 + 1)
            {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
                ok = false;
                break;
            }
            r = GetOverlappedResult(pipe, &ov, &done, FALSE);
        }
        if (!r || !done)
        {
            ok = false;
            break;
        }
        p += done;
        n -= done;
    }
    CloseHandle(ov.hEvent);
    return ok;
}
} // namespace pipeio
