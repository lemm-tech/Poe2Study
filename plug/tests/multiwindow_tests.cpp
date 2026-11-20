#include "../Common/AppConfig.h"
#include "../Common/InstanceMutex.h"
#include "../Plug/NamedPipeClie.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int passed = 0, failed = 0;
#define CHECK(x) do { const bool ok = !!(x); printf("[%s] %s\n", ok ? "PASS" : "FAIL", #x); fflush(stdout); ok ? ++passed : ++failed; } while (0)

std::wstring StopName(DWORD pid) { return L"Local\\Poe2MultiTestStop_" + std::to_wstring(pid); }
int Host() {
    HANDLE locks[]{CreateMutexW(nullptr, FALSE, L"Local\\PathOfExileSingleInstance"),
                   CreateMutexW(nullptr, FALSE, L"Local\\PoERunMutexA"),
                   CreateMutexW(nullptr, FALSE, L"Local\\PoERunMutexB")};
    HANDLE unrelated = CreateMutexW(nullptr, FALSE, (L"Local\\Poe2MultiTestUnrelated_" + std::to_wstring(GetCurrentProcessId())).c_str());
    HANDLE unnamed = CreateMutexW(nullptr, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, StopName(GetCurrentProcessId()).c_str());
    HMODULE dll = LoadLibraryW(L".\\Monitor.dll");
    if (!dll || !unrelated || !unnamed || !stop || !locks[0] || !locks[1] || !locks[2]) return 30;
    if (WaitForSingleObject(stop, 45000) != WAIT_OBJECT_0) return 31;
    // Closing the three target handles must leave other synchronization intact.
    if (WaitForSingleObject(unrelated, 0) != WAIT_OBJECT_0 || WaitForSingleObject(unnamed, 0) != WAIT_OBJECT_0) return 32;
    ReleaseMutex(unrelated); ReleaseMutex(unnamed);
    auto shutdown = reinterpret_cast<DWORD(WINAPI*)(LPVOID)>(GetProcAddress(dll, "MonitorShutdown"));
    if (!shutdown || shutdown(nullptr) != 1 || !FreeLibrary(dll)) return 33;
    CloseHandle(stop); CloseHandle(unrelated); CloseHandle(unnamed);
    return 0;
}
struct Child {
    PROCESS_INFORMATION info{};
    ~Child() { Stop(); if (info.hThread) CloseHandle(info.hThread); if (info.hProcess) CloseHandle(info.hProcess); }
    bool Start() {
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, DWORD(_countof(exe)));
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --host";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        return CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &info) != FALSE;
    }
    bool Stop() {
        if (!info.hProcess) return true;
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, StopName(info.dwProcessId).c_str());
        if (event) { SetEvent(event); CloseHandle(event); }
        if (WaitForSingleObject(info.hProcess, 10000) != WAIT_OBJECT_0) return false;
        DWORD code = 1; GetExitCodeProcess(info.hProcess, &code); return code == 0;
    }
};
bool Ready(DWORD pid) {
    const auto pipe = L"\\\\.\\pipe\\Poe2MultiTest_" + std::to_wstring(pid);
    const auto end = GetTickCount64() + 10000;
    while (GetTickCount64() < end) {
        if (WaitNamedPipeW(pipe.c_str(), 50)) return true;
        Sleep(20);
    }
    return false;
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--host") return Host();
    CHECK(instance::IsInstanceLock(L"\\BaseNamedObjects\\PoERunMutexA"));
    CHECK(!instance::IsInstanceLock(L"\\BaseNamedObjects\\PoERunMutexA_extra"));
    CHECK(!instance::IsInstanceLock(L"\\BaseNamedObjects\\DBWinMutex"));
    Child a, b;
    CHECK(a.Start()); CHECK(b.Start());
    if (!a.info.hProcess || !b.info.hProcess) return 1;
    CHECK(Ready(a.info.dwProcessId)); CHECK(Ready(b.info.dwProcessId));
    const auto ap = a.info.dwProcessId, bp = b.info.dwProcessId;
    const auto at = gametarget::CreationTime(a.info.hProcess), bt = gametarget::CreationTime(b.info.hProcess);
    CHECK(at && bt);
    CHECK(instance::Inspect(ap, at).matches.size() == 3);
    CHECK(instance::Inspect(bp, bt).matches.size() == 3);
    CHECK(!instance::Release(ap, at + 1).ok);
    CHECK(instance::Inspect(ap, at).matches.size() == 3);
    const auto released = instance::Release(ap, at);
    CHECK(released.ok && released.closed == 3);
    CHECK(instance::Inspect(ap, at).matches.empty());
    CHECK(instance::Inspect(bp, bt).matches.size() == 3);
    CHECK(instance::Release(ap, at).closed == 0);
    std::string error;
    app::AppConfig ca, cb; ca.LoadFromFile("config.ini"); cb.LoadFromFile("config.ini");
    CHECK(ca.BindTarget(ap, at, nullptr, error)); CHECK(cb.BindTarget(bp, bt, nullptr, error));
    CHECK(ca.pipeName != cb.pipeName && ca.pipeName == "Poe2MultiTest_" + std::to_string(ap));
    CHECK(ca.RuntimeFileW(L"config.ini") != cb.RuntimeFileW(L"config.ini"));
    CHECK(ca.RuntimeFileW(L"combo.ini") != cb.RuntimeFileW(L"combo.ini"));
    CHECK(ca.RuntimeFileW(L"follow.ini") != cb.RuntimeFileW(L"follow.ini"));
    CHECK(WritePrivateProfileStringW(L"nav", L"stepDistance", L"333", ca.RuntimeFileW(L"config.ini").c_str()));
    CHECK(GetPrivateProfileIntW(L"nav", L"stepDistance", 0, cb.RuntimeFileW(L"config.ini").c_str()) == 126);
    app::AppConfig wrong = ca;
    CHECK(!wrong.BindTarget(ap, at + 1, nullptr, error));
    // A real other process's HWND must also be rejected.
    CHECK(!wrong.BindTarget(ap, at, GetDesktopWindow(), error));
    const std::wstring pipeA = ca.PipeFullName(), pipeB = cb.PipeFullName();
    pipe::SetExpectedGameIdentity(ap, at);
    CHECK(pipe::StartManagedConnections(pipeA, 5000));
    CHECK(pipe::GetConnectionSnapshot().gamePid == ap);
    pipe::StopManagedConnections();
    pipe::SetExpectedGameIdentity(bp, bt);
    CHECK(!pipe::StartManagedConnections(pipeA, 700));
    CHECK(!pipe::GetConnectionSnapshot().ready);
    pipe::StopManagedConnections();
    pipe::SetExpectedGameIdentity(ap, at + 1);
    CHECK(!pipe::StartManagedConnections(pipeA, 700));
    pipe::StopManagedConnections();
    pipe::SetExpectedGameIdentity(bp, bt);
    CHECK(pipe::StartManagedConnections(pipeB, 5000));
    CHECK(pipe::GetConnectionSnapshot().gamePid == bp);
    pipe::StopManagedConnections();
    CHECK(a.Stop()); CHECK(!ca.TargetAlive()); CHECK(cb.TargetAlive());
    pipe::SetExpectedGameIdentity(bp, bt);
    CHECK(pipe::StartManagedConnections(pipeB, 5000));
    CHECK(pipe::GetConnectionSnapshot().gamePid == bp);
    pipe::StopManagedConnections(); CHECK(b.Stop());
    printf("MULTIWINDOW passed=%d failed=%d\n", passed, failed);
    return failed ? 1 : 0;
}
