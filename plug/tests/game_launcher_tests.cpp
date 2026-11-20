#include "../Plug/GameLauncher.h"
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;
namespace game = sessions::game;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #value "\n"; } } while (0)

fs::path Image() {
    wchar_t image[32768]{};
    GetModuleFileNameW(nullptr, image, DWORD(_countof(image)));
    return fs::path(image);
}
int Child() {
    wchar_t cwd[32768]{};
    GetCurrentDirectoryW(DWORD(_countof(cwd)), cwd);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool valid = argv && argc == 1 && fs::equivalent(fs::path(cwd), Image().parent_path()) &&
                       fs::equivalent(fs::path(argv[0]), Image());
    if (argv) LocalFree(argv);
    std::ofstream(Image().parent_path() / L"child.txt") << GetCurrentProcessId() << " " << valid;
    return valid ? 0 : 1;
}
void CheckChild(const game::LaunchResult& result, const fs::path& directory) {
    CHECK(result.pid != 0 && result.error.empty());
    if (!result.pid) return;
    const auto report = directory / L"child.txt";
    DWORD pid = 0; bool valid = false;
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        std::ifstream input(report);
        if (input >> pid >> valid) break;
        Sleep(10);
    }
    CHECK(pid == result.pid);
    CHECK(valid);
    if (HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, result.pid)) {
        CHECK(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
        CloseHandle(child);
    }
    fs::remove(report);
}
int wmain() {
    if (Image().filename() == L"Game \u03a9.EXE") return Child();
    const auto folder = Image().parent_path() / (L"launcher \u4e2d\u6587 \u03a9 " + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(folder);
    const auto executable = folder / L"Game \u03a9.EXE";
    CHECK(CopyFileW(Image().c_str(), executable.c_str(), TRUE));
    const auto settings = folder / L"launcher.ini";
    DWORD error = 0;
    CHECK(game::LoadPath(settings.wstring()).empty());
    CHECK(game::SavePath(settings.wstring(), executable.wstring(), error));
    CHECK(error == ERROR_SUCCESS);
    CHECK(game::LoadPath(settings.wstring()) == executable.wstring());
    std::ifstream raw(settings, std::ios::binary);
    unsigned char first = 0, second = 0;
    raw.read(reinterpret_cast<char*>(&first), 1); raw.read(reinterpret_cast<char*>(&second), 1);
    CHECK(first == 0xFF && second == 0xFE); raw.close();
    CHECK(game::SavePath(settings.wstring(), L"", error));
    CHECK(game::LoadPath(settings.wstring()).empty());
    const auto quoted = L"  \"" + executable.wstring() + L"\"  ";
    CHECK(game::SavePath(settings.wstring(), quoted, error));
    CHECK(game::LoadPath(settings.wstring()) == executable.wstring());
    SetFileAttributesW(settings.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(!game::SavePath(settings.wstring(), L"changed", error) && error != ERROR_SUCCESS);
    CHECK(game::LoadPath(settings.wstring()) == executable.wstring());
    SetFileAttributesW(settings.c_str(), FILE_ATTRIBUTE_NORMAL);

    CheckChild(game::Start(quoted, L"C:\\"), folder);
    CheckChild(game::Start(executable.filename().wstring(), folder.wstring()), folder);
    auto bad = game::Start(L"", folder.wstring());
    CHECK(!bad.pid && !bad.error.empty());
    bad = game::Start((folder / L"missing.exe").wstring(), folder.wstring());
    CHECK(!bad.pid && !bad.error.empty());
    bad = game::Start(folder.wstring(), folder.wstring());
    CHECK(!bad.pid && !bad.error.empty());
    const auto document = folder / L"not-game.txt";
    std::ofstream(document) << "test";
    bad = game::Start(document.wstring(), folder.wstring());
    CHECK(!bad.pid && !bad.error.empty());
    bad = game::Start(executable.wstring() + L" --argument", folder.wstring());
    CHECK(!bad.pid && !bad.error.empty());
    bad = game::Start(L"\"" + executable.wstring() + L"\" --argument", folder.wstring());
    CHECK(!bad.pid && !bad.error.empty());
    const auto invalid = folder / L"invalid.exe";
    std::ofstream(invalid) << "not a PE executable";
    bad = game::Start(invalid.wstring(), folder.wstring());
    CHECK(!bad.pid && !bad.error.empty());
    // All temporary files are created beneath this one test-owned directory.
    fs::remove_all(folder);
    std::cout << "Game launcher: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
