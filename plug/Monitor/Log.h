#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <share.h>   // _fsopen / _SH_DENYWR

// ============================================================
// Log.h：DLL 内文件日志（替代 printf，不弹控制台）
//
// ★ 为什么 Monitor 不用 Common\Logger（这点以前注释写反了）：
//   Common\Logger.cpp 虽然也支持 Target::File，但它的落点、文件名、
//   生命周期都由 poelog::Init 决定，而且它的设计基准是"Plug.exe 的控制台
//   输出"（含 UTF-8 -> 控制台宽字符、UTF-8 -> GBK 重定向两套分支）。
//   Monitor 是 SubSystem=Windows 的 DLL，宿主是游戏进程，一个 stdout 都
//   没有，不需要那两套分支，也不想为了日志去链接 Common\Logger.cpp。
//
//   所以这里是**有意保留的第二份实现**，不是漏重构：
//     Common\Logger —— Plug.exe / NavGUI.exe（控制台或普通文件）
//     Monitor\Log.h —— Monitor.dll（注入进程内，只要一个常开的文件句柄）
//   两份共用的只有"行格式长什么样"（[hh:mm:ss.mmm] 正文），改格式时两边
//   都要改；除此之外没有任何共享代码，也不用共享。
//
// 职责：日志写入 DLL 同目录下的 monitor_log.txt，
// 这样不 AllocConsole、不弹窗口，也就不会因关闭控制台而误杀游戏。
//
// 实现要点：日志句柄只在 Init 里打开一次，之后 Write 直接写同一个
// FILE*。旧版每写一行都 fopen/fclose，扫图这种高频日志会把时间
// 全耗在打开关闭文件上，还把日志线程卡死。
// 写入用临界区串行化，多线程（扫图线程 / 管道线程）同时写不会串行错乱。
// ============================================================

namespace DllLog {

// 日志文件完整路径（进程启动时初始化一次）
inline std::string& LogPath()
{
	static std::string path;
	return path;
}

// 常开的日志句柄（Init 里打开，Close 里关闭）
inline FILE*& LogFile()
{
	static FILE* fp = nullptr;
	return fp;
}

// 写日志的临界区（首次使用时初始化）
inline CRITICAL_SECTION& LogLock()
{
	static CRITICAL_SECTION cs;
	static bool inited = false;
	if (!inited) {
		InitializeCriticalSection(&cs);
		inited = true;
	}
	return cs;
}

// 初始化：定位 DLL 所在目录 + 文件名，并以覆盖方式打开一次
// fileName 由调用方从配置传入（MonitorConfig.logFileName），此处不给默认值，
// 避免"文件名"这个资源名散落在多处（架构守卫 A5/A9）。
inline void Init(HMODULE hModule, const char* fileName)
{
	if (fileName == nullptr || *fileName == '\0') {
		fileName = "monitor_log.txt";   // 最后兜底：配置读不到时才用
	}
	char mod[MAX_PATH] = { 0 };
	GetModuleFileNameA(hModule, mod, MAX_PATH);
	std::string full(mod);
	size_t pos = full.find_last_of("\\/");
	std::string dir = (pos == std::string::npos) ? "" : full.substr(0, pos + 1);
	LogPath() = dir + fileName;

	// 清空旧日志，句柄保持打开。
	// 用 _fsopen 而不是 fopen_s：fopen_s 的 "w" 是独占锁（_SH_DENYRW），
	// 游戏运行时连记事本都打不开日志，排查时只能卸载 DLL 才能看。
	// _SH_DENYWR = 禁止其他进程写，但允许读。
	FILE* fp = _fsopen(LogPath().c_str(), "w", _SH_DENYWR);
	if (fp == nullptr) {
		// 共享模式失败时退回独占，保证至少能记日志
		fopen_s(&fp, LogPath().c_str(), "w");
	}
	if (fp != nullptr) {
		fprintf(fp, "===== Monitor DLL 日志 =====\n");
		fprintf(fp, "[路径] %s\n", LogPath().c_str());
		fprintf(fp, "[进程] PID=%lu\n", (unsigned long)GetCurrentProcessId());
		fflush(fp);
	}
	LogFile() = fp;
}

// 写一行日志（带时间戳）
inline void Write(const char* fmt, ...)
{
	FILE* fp = LogFile();
	if (!fp) {
		return;
	}

	SYSTEMTIME st;
	GetLocalTime(&st);

	EnterCriticalSection(&LogLock());
	fprintf(fp, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	va_list args;
	va_start(args, fmt);
	vfprintf(fp, fmt, args);
	va_end(args);

	fprintf(fp, "\n");
	fflush(fp);   // 进程被游戏带走时也不丢最后几行
	LeaveCriticalSection(&LogLock());
}

// 卸载时关闭句柄（DllMain 的 PROCESS_DETACH 里调用）
inline void Close()
{
	FILE* fp = LogFile();
	if (!fp) {
		return;
	}
	EnterCriticalSection(&LogLock());
	fclose(fp);
	LogFile() = nullptr;
	LeaveCriticalSection(&LogLock());
}

} // namespace DllLog

// 便捷宏
#define LOGI(...) DllLog::Write(__VA_ARGS__)
