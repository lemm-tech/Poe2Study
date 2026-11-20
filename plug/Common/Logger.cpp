#include "Logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace poelog {
namespace {

Target  g_target = Target::Console;
FILE*   g_file = nullptr;
std::mutex g_mutex;

// ★ 控制台编码：源码是 UTF-8（工程加了 /utf-8），字符串字面量就是 UTF-8 字节。
//   而 cmd/Windows Terminal 默认代码页是 936(GBK)，直接 fputs 就是一堆乱码。
//   这里的做法：
//     · 输出到控制台时不改代码页，而是把 UTF-8 转成当前控制台的宽字符再写
//       到 File 里 —— 这样既不影响别的程序，也不会在管道重定向时丢掉中文。
//     · 输出到日志文件时按 UTF-8 原样写（带 BOM，方便记事本打开）。
bool g_consoleWide = false;
bool g_pipeUtf8 = false;
bool g_pipeUtf8BomWritten = false;
HANDLE g_out = nullptr;
HANDLE g_err = nullptr;

const char* LevelTag(Level level)
{
    switch (level) {
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    default:           return "INFO ";
    }
}

// 把 DLL/EXE 所在目录作为日志落点
std::string ModuleDirectory()
{
    char mod[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, mod, MAX_PATH);
    std::string full(mod);
    size_t pos = full.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : full.substr(0, pos + 1);
}

// UTF-8 -> 宽字符。失败返回空串（调用方退回 ANSI 写）
std::wstring Utf8ToWide(const char* s, int len)
{
    if (!s || len <= 0) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, &w[0], need);
    return w;
}

// UTF-8 -> 系统 ANSI 代码页（简中 = GBK/936）
//
// ★ 为什么需要：
//   在 VS 里按 F5 时 stdout 被重定向到调试器，GetConsoleMode 检测不到控制台，
//   只能走字节流。此时若直接写 UTF-8 字节，VS 输出窗口按系统 ANSI(936) 解码，
//   中文就变成"宸插姞杞?"。所以重定向分支必须先转 GBK 再写。
std::string Utf8ToAnsi(const char* s, int len)
{
    if (!s || len <= 0) return std::string();
    std::wstring w = Utf8ToWide(s, len);
    if (w.empty()) return std::string();

    const int need = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                                        nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string a((size_t)need, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                        &a[0], need, nullptr, nullptr);
    return a;
}

void WriteConsoleLine(bool toErr, const char* body)
{
    if (!body) return;
    const int len = (int)strlen(body);
    FILE* fp = toErr ? stderr : stdout;

    // 首选：转宽字符 + WriteConsoleW，绕开代码页，任何真实控制台都能显示中文
    if (g_consoleWide) {
        std::wstring w = Utf8ToWide(body, len);
        if (!w.empty()) {
            w.push_back(L'\n');
            HANDLE h = toErr ? g_err : g_out;
            DWORD written = 0;
            WriteConsoleW(h, w.data(), (DWORD)w.size(), &written, nullptr);
            return;
        }
    }

    // NavGUI 启动 Plug.exe 时通过匿名管道读取输出。管道使用带 BOM 的
    // UTF-8，避免 GUI 和子进程各自猜 ACP/UTF-8，造成中文字节被错解码。
    if (g_pipeUtf8) {
        if (!g_pipeUtf8BomWritten) {
            fwrite("\xEF\xBB\xBF", 1, 3, fp);
            g_pipeUtf8BomWritten = true;
        }
        fwrite(body, 1, (size_t)len, fp);
        fputc('\n', fp);
        fflush(fp);
        return;
    }

    // 退路（被重定向到 VS 调试窗口 / 管道 / 文件）：转 ANSI 再写，否则乱码
    std::string ansi = Utf8ToAnsi(body, len);
    if (!ansi.empty()) {
        fwrite(ansi.data(), 1, ansi.size(), fp);
        fputc('\n', fp);
    }
    else {
        fprintf(fp, "%s\n", body);
    }
    fflush(fp);
}

} // namespace

void Init(Target target, const std::string& fileName, const std::string& baseDir)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    // ★ 切换目标前先把上一个目标的资源收干净。
    //   原实现只在 target==File 时才碰 g_file，于是
    //   Init(File) -> Init(Console) 会让日志文件句柄永久泄漏，
    //   外部想删除/重命名这个文件会被占用。
    if (target != Target::File && g_file) {
        fclose(g_file);
        g_file = nullptr;
    }

    g_target = target;
    g_pipeUtf8 = false;
    g_pipeUtf8BomWritten = false;

    // 探测控制台是否支持宽字符输出（被重定向到文件时不支持，退回字节流）
    if (target == Target::Console) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        g_consoleWide = (out != nullptr && out != INVALID_HANDLE_VALUE &&
                         GetConsoleMode(out, &mode) != 0);
        g_out = g_consoleWide ? out : nullptr;
        g_pipeUtf8 = !g_consoleWide && out != nullptr && out != INVALID_HANDLE_VALUE &&
                     GetFileType(out) == FILE_TYPE_PIPE;

        g_err = GetStdHandle(STD_ERROR_HANDLE);
        if (g_err == INVALID_HANDLE_VALUE) {
            g_err = nullptr;
        }
    }

    if (target == Target::File) {
        if (g_file) {
            fclose(g_file);
            g_file = nullptr;
        }

        std::string dir = baseDir.empty() ? ModuleDirectory() : baseDir;
        // 约定：dir 以 '\' 结尾，fileName 不带前导 '\'。
        // 调用方传进来的 baseDir 不保证这一点，这里补齐，否则会拼出
        // "xxxxxmonitor_log.txt" 这种路径。
        if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
            dir.push_back('\\');
        }

        const std::string full = dir + fileName;

        // ★ 用 _fsopen 而不是 fopen_s。
        //
        //   fopen_s 的 "w" 是独占锁（_SH_DENYRW）：GUI 一跑起来，
        //   navgui_log.txt 就被锁死，用户拿记事本 / VSCode 都打不开，
        //   想边跑边看日志（这正是测试技能按钮时最需要的事）做不到。
        //
        //   _SH_DENYWR = 禁止其他进程写，但**允许读**。
        //   Monitor 侧的 Log.h 早就是这么做的，这里对齐。
        g_file = _fsopen(full.c_str(), "w", _SH_DENYWR);
        const errno_t rc = g_file ? 0 : errno;
        if (rc != 0 || !g_file) {
            // ★ 日志系统自己的失败必须能被看见。
            //   原实现这里只有 if 没有 else：路径不可写 / 磁盘满 /
            //   文件名非法时，日志一行没有、程序照跑、没人知道为什么。
            g_file = nullptr;
            char dbg[512] = { 0 };
            _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
                        "[poelog] 打开日志文件失败 rc=%d path=%s\n",
                        (int)rc, full.c_str());
            OutputDebugStringA(dbg);

            // 降级到控制台：至少还能看到输出，而不是全静默
            HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            g_consoleWide = (out != nullptr && out != INVALID_HANDLE_VALUE &&
                             GetConsoleMode(out, &mode) != 0);
            g_out = g_consoleWide ? out : nullptr;
            g_pipeUtf8 = !g_consoleWide && out != nullptr && out != INVALID_HANDLE_VALUE &&
                         GetFileType(out) == FILE_TYPE_PIPE;
            g_pipeUtf8BomWritten = false;
            g_err = GetStdHandle(STD_ERROR_HANDLE);
            if (g_err == INVALID_HANDLE_VALUE) {
                g_err = nullptr;
            }
            g_target = Target::Console;
            return;
        }

        // 写 UTF-8 BOM，记事本 / VSCode 打开都不会猜错编码
        fwrite("\xEF\xBB\xBF", 1, 3, g_file);
        fprintf(g_file, "===== log start =====\n");
        fflush(g_file);
    }
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void Write(Level level, const char* fmt, ...)
{
    // 先格式化到本地缓冲，避免在锁内调用两次 vfprintf
    char body[2048] = { 0 };
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_target == Target::Console) {
        WriteConsoleLine(level == Level::Error, body);
        return;
    }

    if (!g_file) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] %s %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        LevelTag(level), body);
    fflush(g_file);
}

void PrintUtf8(const char* fmt, ...)
{
    if (!fmt) return;
    char body[2048] = { 0 };
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
    va_end(args);

    // 去掉尾部换行（WriteConsoleLine 会自己补一个），避免出现空行
    size_t n = strlen(body);
    while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r')) {
        body[--n] = '\0';
    }
    // 空行也要能打出来
    if (n == 0) {
        body[0] = ' ';
        body[1] = '\0';
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    WriteConsoleLine(false, body);
}

void PrintUtf8Warn(const char* fmt, ...)
{
    if (!fmt) return;
    char body[2048] = { 0 };
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
    va_end(args);
    size_t n = strlen(body);
    while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r')) body[--n] = '\0';

    std::lock_guard<std::mutex> lock(g_mutex);
    WriteConsoleLine(false, body);
}

void PrintUtf8Error(const char* fmt, ...)
{
    if (!fmt) return;
    char body[2048] = { 0 };
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
    va_end(args);
    size_t n = strlen(body);
    while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r')) body[--n] = '\0';

    std::lock_guard<std::mutex> lock(g_mutex);
    WriteConsoleLine(true, body);
}

} // namespace poelog
