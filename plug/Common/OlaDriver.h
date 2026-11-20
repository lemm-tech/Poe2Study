#pragma once
// ============================================================
// OlaDriver.h —— 内存读取驱动
//
// ★ 名字是历史遗留：这里已经**没有 OLA 代码**了。
//   原因：OLA（欧拉）插件需要联网注册、有授权期，
//   过期后 Reg() 不返回也不报错，直接把进程挂死（实测 >90 秒）。
//   现在改成纯 ReadProcessMemory，不需要任何授权，也不需要驱动。
//
// 保留类名 ola::OlaDriver 与原先的方法签名，只是为了不改动
// 上层（GameMemory / MyOLAFunction / UseCases）的调用点。
// ============================================================
#include <cstdint>
#include <string>
#include <vector>

#include "../Common/AppConfig.h"

namespace ola {

// 读取模式（目前只有 RPM 一种实现）
enum class MemoryMode {
    Rpm,         // ReadProcessMemory
    Remote       // 预留：将来接内核驱动时用
};

// 字符串编码：组件名走 ANSI(GBK)，游戏内文本走 UTF-16
enum class TextKind {
    Ansi,
    Utf16
};

class OlaDriver {
public:
    OlaDriver();
    ~OlaDriver();

    OlaDriver(const OlaDriver&) = delete;
    OlaDriver& operator=(const OlaDriver&) = delete;

    // ---- 初始化 ----

    // 打开目标进程。失败通常是权限不足（需要管理员身份运行）
    bool InitializeLocal(uint32_t pid);

    // 兼容旧调用点：忽略 creds，内部转成 InitializeLocal
    bool Initialize(const app::OlaCredentials& creds);

    // 附加到进程（等同 InitializeLocal）
    bool Attach(uint32_t pid, MemoryMode preferred, MemoryMode* actual = nullptr);

    // 历史接口，现在不做事
    bool BindWindowByHandle(uint64_t hwnd);

    void Detach();
    void Shutdown();

    // ---- 状态 ----
    bool IsRegistered() const { return m_ready; }
    bool IsAttached() const { return m_pid != 0; }
    uint32_t Pid() const { return m_pid; }
    MemoryMode ActiveMode() const { return m_mode; }
    bool IsDriverActive() const { return false; }   // 无内核驱动
    bool IsLocalMode() const { return m_ready; }

    // ---- 读内存 ----
    bool ReadRaw(uint64_t addr, void* out, size_t size) const;
    bool ReadU8(uint64_t addr, uint8_t& out) const;
    bool ReadU32(uint64_t addr, uint32_t& out) const;
    bool ReadU64(uint64_t addr, uint64_t& out) const;

    std::string ReadAnsi(uint64_t addr) const;
    std::string ReadUtf16(uint64_t addr) const;

    bool ReadBlob(uint64_t addr, size_t size, std::vector<uint8_t>& out) const;

    // 取模块基址
    uint64_t ModuleBase(const std::string& moduleName) const;

private:
    uint32_t   m_pid = 0;
    MemoryMode m_mode = MemoryMode::Rpm;
    bool       m_ready = false;
    void*      m_hproc = nullptr;   // HANDLE
};

} // namespace ola
