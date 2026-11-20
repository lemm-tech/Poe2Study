#include "AppConfig.h"
#include "Logger.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <cstring>

namespace app {
namespace {

// 去掉首尾空白
std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return std::string();
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 取当前 EXE/DLL 所在目录（带尾部分隔符）
std::string ModuleDirectory()
{
    char mod[MAX_PATH] = { 0 };
    const DWORD n = GetModuleFileNameA(nullptr, mod, MAX_PATH);
    if (n == 0) {
        return std::string();
    }
    std::string full(mod, n);
    const size_t pos = full.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : full.substr(0, pos + 1);
}

// 读环境变量，存在则覆盖 out（用 _dupenv_s 避免 C4996）
void OverrideFromEnv(const char* name, std::string& out)
{
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
        if (*buffer != '\0') {
            out = buffer;
        }
        free(buffer);
    }
}

// 判断是否绝对路径（盘符 "C:\..." 或 UNC "\\server\..."）
bool IsAbsolutePath(const std::string& p)
{
    if (p.size() > 1 && p[1] == ':') return true;
    if (p.rfind("\\\\", 0) == 0) return true;
    return false;
}

// 递归创建目录（父目录不存在也一并建）
// 为什么需要：bmpPath 默认是 "output\\map_cost.bmp"，
// 首次运行时 output\ 可能还不存在，ofstream 会直接失败。
void EnsureParentDirectory(const std::string& filePath)
{
    const size_t pos = filePath.find_last_of("\\/");
    if (pos == std::string::npos || pos == 0) {
        return;   // 没有目录部分
    }
    std::string dir = filePath.substr(0, pos);

    // 逐级创建：C:\a\b\c -> 先建 C:\a，再 C:\a\b ...
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '\\' || dir[i] == '/') {
            if (!cur.empty() && cur.back() != ':') {
                CreateDirectoryA(cur.c_str(), nullptr);   // 已存在会返回失败，忽略即可
            }
        }
        if (i < dir.size()) cur.push_back(dir[i]);
    }
}

// 把相对路径解析成"exe 目录 + 相对路径"，并确保父目录存在
std::string ResolveOutputPath(const std::string& configured)
{
    if (configured.empty()) return std::string();

    std::string full = configured;
    if (!IsAbsolutePath(configured)) {
        const std::string exeDir = ModuleDirectory();
        if (!exeDir.empty()) {
            full = exeDir + configured;
        }
    }

    EnsureParentDirectory(full);
    return full;
}

} // namespace

bool AppConfig::LoadFromFile(const std::string& iniPath)
{
    // 先按"exe 所在目录 + fileName"找，找不到再按相对路径找。
    // 为什么：进程的当前工作目录不一定是 exe 目录（从快捷方式、
    // 资源管理器双击、VS 调试器启动时都可能不同），
    // 用相对路径会莫名其妙读不到配置文件。
    std::string resolved = iniPath;
    bool isAbsolute = (iniPath.size() > 1 && iniPath[1] == ':') || iniPath.rfind("\\\\", 0) == 0;
    if (!isAbsolute) {
        const std::string exeDir = ModuleDirectory();
        if (!exeDir.empty()) {
            resolved = exeDir + iniPath;
        }
    }

    std::ifstream ifs(resolved);
    if (!ifs.is_open()) {
        // 回退：万一 exeDir 拼错了，再试一次裸文件名
        if (resolved != iniPath) {
            ifs.clear();
            ifs.open(iniPath);
        }
        if (!ifs.is_open()) {
            // 只有真的读不到配置时才把排查线索全打出来，避免正常运行时刷屏
            char cwd[MAX_PATH] = { 0 };
            GetCurrentDirectoryA(MAX_PATH, cwd);
            LOGE("[Config] 找不到配置文件，全部使用默认值（凭据为空 -> OLA 注册必然失败）");
            LOGE("[Config]   尝试过: %s", resolved.c_str());
            LOGE("[Config]   当前工作目录: %s", cwd);
            LOGE("[Config]   exe 目录: %s", ModuleDirectory().c_str());
            return false;   // 没有配置文件不是错误，用默认值继续
        }
    }
    else {
        LOGI("[Config] 已加载 %s", resolved.c_str());
    }

    std::string section;
    std::string line;
    while (std::getline(ifs, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = ToLower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (section == "game") {
            if (key == "windowtitle") {
                gameWindowTitle = value;
            }
            else if (key == "modulename") {
                gameModuleName = value;
            }
            else if (key == "uselocalrpm") {
                useLocalRpm = (value == "1" || _stricmp(value.c_str(), "true") == 0);
            }
        }
        else if (section == "pipe") {
            if (key == "name") {
                pipeName = value;
            }
        }
        else if (section == "output") {
            if (key == "bmppath") {
                bmpPath = value;
            }
            else if (key == "logpath") {
                logPath = value;
            }
        }
        else if (section == "ola") {
            if (key == "account") {
                ola.account = value;
            }
            else if (key == "password") {
                ola.password = value;
            }
            else if (key == "apptag") {
                ola.appTag = value;
            }
            else if (key == "dllpath") {
                ola.dllPath = value;
            }
        }
    }
    return true;
}

void AppConfig::ApplyEnvironmentOverrides()
{
    OverrideFromEnv("POE2_PIPE_NAME", pipeName);
    OverrideFromEnv("POE2_BMP_PATH", bmpPath);
    OverrideFromEnv("POE2_LOG_PATH", logPath);
    OverrideFromEnv("POE2_GAME_WINDOW", gameWindowTitle);
    OverrideFromEnv("POE2_GAME_MODULE", gameModuleName);
    OverrideFromEnv("POE2_OLA_ACCOUNT", ola.account);
    OverrideFromEnv("POE2_OLA_PASSWORD", ola.password);
    OverrideFromEnv("POE2_OLA_APPTAG", ola.appTag);
}

std::wstring AppConfig::PipeFullName() const
{
    // 管道短名按 UTF-8 存（纯 ASCII 时与 ANSI 一致），转成宽字符
    const int need = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), (int)pipeName.size(), nullptr, 0);
    std::wstring wide;
    if (need > 0) {
        wide.resize((size_t)need);
        MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), (int)pipeName.size(), &wide[0], need);
    }
    else {
        wide.assign(pipeName.begin(), pipeName.end());
    }
    return L"\\\\.\\pipe\\" + wide;
}

std::string AppConfig::BmpPath() const
{
    if (targetPid && !bmpPath.empty()) return RuntimeFile("map_cost.bmp");
    return ResolveOutputPath(bmpPath);
}

std::string AppConfig::LogPath() const
{
    if (targetPid) return RuntimeFile("monitor_log.txt");
    return ResolveOutputPath(logPath);
}

AppConfig& Config()
{
    // function-local static：首次调用才构造，规避静态初始化顺序问题
    static AppConfig instance;
    return instance;
}

bool AppConfig::BindTarget(DWORD pid, uint64_t start, HWND window, std::string& error)
{
    error.clear();
    if (!gametarget::Alive(pid, start)) { error = "Target process exited or identity changed"; return false; }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t image[32768]{}; DWORD imageLength = DWORD(_countof(image));
    const bool imageOk = process && QueryFullProcessImageNameW(process, 0, image, &imageLength);
    if (process) CloseHandle(process);
    const auto leaf = std::filesystem::path(image).filename().wstring();
    const auto wanted = std::filesystem::u8path(gameModuleName).wstring();
    if (!imageOk || _wcsicmp(leaf.c_str(), wanted.c_str()) != 0) {
        error = "Selected process image does not match configured game module"; return false;
    }
    DWORD windowPid = 0;
    if (window && (!IsWindow(window) || !GetWindowThreadProcessId(window, &windowPid) || windowPid != pid)) {
        error = "Selected window no longer belongs to target process"; return false;
    }
    const auto root = gametarget::ModuleDirectory();
    const auto directory = gametarget::SessionDirectory(root, pid, start);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = "Cannot create session directory: " + ec.message(); return false; }
    // First use takes a private copy; future panel restarts retain this game's settings.
    for (const wchar_t* name : {L"config.ini", L"combo.ini", L"follow.ini", L"recovery.ini"}) {
        const auto source = root + name, destination = directory + name;
        if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !CopyFileW(source.c_str(), destination.c_str(), TRUE) && GetLastError() != ERROR_FILE_EXISTS) {
            error = "Cannot copy session config"; return false;
        }
    }
    LoadFromFile(gametarget::Ansi(directory + L"config.ini"));
    ApplyEnvironmentOverrides();
    // Persist the effective base name so the DLL (which does not inherit this
    // panel's environment) reads exactly the same name before adding its PID.
    if (!WritePrivateProfileStringA("pipe", "name", pipeName.c_str(),
                                   gametarget::Ansi(directory + L"config.ini").c_str())) {
        error = "Cannot save session pipe configuration"; return false;
    }
    targetPid = pid;
    targetStart = start;
    targetWindow = window;
    sessionDirectory = directory;
    pipeName = gametarget::PipeName(pipeName, pid);
    return true;
}
bool AppConfig::TargetAlive() const { return gametarget::Alive(targetPid, targetStart); }
HWND AppConfig::TargetWindow() const {
    return TargetAlive() ? gametarget::Window(targetPid, targetWindow) : nullptr;
}
std::wstring AppConfig::RuntimeFileW(const wchar_t* file) const {
    return (sessionDirectory.empty() ? gametarget::ModuleDirectory() : sessionDirectory) + file;
}
std::string AppConfig::RuntimeFile(const char* file) const {
    return gametarget::Ansi(RuntimeFileW(std::wstring(file, file + strlen(file)).c_str()));
}

} // namespace app
