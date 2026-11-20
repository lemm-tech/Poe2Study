#pragma once
// ============================================================
// Logger.h —— Plug.exe / NavGUI.exe 的日志
//
// 两种输出目标：
//   Console  —— 打控制台（等价于原来的 cout / printf）
//   File     —— 写文件（NavGUI 用它落 navgui_log.txt）
//
// 为什么收拢：原来是 cout / printf 各写各的，且早期每个文件各有一套
// 逐行 fopen/fclose 的实现（文档 11 节第 8 条）。现在句柄常开 +
// 临界区串行化，UTF-8 到控制台的编码分支也只剩这一处。
//
// ★ 边界（以前这里写"Plug 与 Monitor 共用一个接口"，是错的）：
//   Monitor.dll 走的是 Monitor\Log.h 的 DllLog，不链接本文件。
//   DLL 的宿主是游戏进程，没有 stdout，不需要控制台 / GBK 重定向这两套
//   分支；两份实现共用的只有行格式，改格式时两边一起改。
//
// 本文件是纯 ASCII（用 /utf-8 编译，纯 ASCII 不受影响）。
// ============================================================
#include <string>
#include <cstdarg>

namespace poelog {

enum class Target {
    Console,   // stdout + stderr
    File       // 写文件
};

enum class Level {
    Info,
    Warn,
    Error
};

// 初始化。File 模式下 fileName 是文件名（会落在 exe 所在目录，
// 若给了 baseDir 则用 baseDir）。覆盖旧文件。
//
// ★ 文件用 _SH_DENYWR 打开（禁止写、允许读）：日志文件在被本进程写入的
//   同时，记事本 / VSCode / Get-Content 都能读。早期用 fopen_s("w") 是独占锁，
//   GUI 一跑起来日志就打不开了。
void Init(Target target, const std::string& fileName = "monitor_log.txt",
          const std::string& baseDir = std::string());

// 卸载（File 模式关句柄）。Console 模式无副作用。
void Shutdown();

// 写一行（自动加时间戳与换行）
void Write(Level level, const char* fmt, ...);

// 把一段文本写到控制台（UTF-8 入，自动转宽字符输出）。
// ★ 用来替代裸 printf：工程用 /utf-8 编译，字面量是 UTF-8 字节，
//   而 cmd 默认代码页 936，直接 printf 会全是乱码。
//   支持 printf 风格格式化；末尾的 \n 会自动补齐（有就不重复加）。
void PrintUtf8(const char* fmt, ...);
void PrintUtf8Warn(const char* fmt, ...);
void PrintUtf8Error(const char* fmt, ...);

} // namespace poelog

// 便捷宏：调用处不用记 Level 枚举的全名
#define LOGI(...)  ::poelog::Write(::poelog::Level::Info,  __VA_ARGS__)
#define LOGW(...)  ::poelog::Write(::poelog::Level::Warn,  __VA_ARGS__)
#define LOGE(...)  ::poelog::Write(::poelog::Level::Error, __VA_ARGS__)

// 打印（替代 printf，自动处理控制台编码）
#define POUT(...)  ::poelog::PrintUtf8(__VA_ARGS__)
