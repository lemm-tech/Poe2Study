#pragma once
// ============================================================
// PollSource.h —— 定时采样 + 双缓冲发布（纯调度，时基外部注入）
//
// 为什么需要双缓冲：
//   周围实体遍历一次要沿红黑树走几百个节点，几毫秒到几十毫秒。
//   而消费侧（导航 tick / UI 绘制）随时要读这份"当前周围有什么"。
//   如果两边直接共享同一个 vector：
//     写侧 clear() + push_back() 的中间态会被读侧看到 ——
//     读到"只剩前 3 个"或者"新旧混在一起"的快照，
//     表现为目标忽有忽无、最近邻一会儿是这只一会儿是那只。
//   双缓冲之后：写侧只动 A，读侧永远看 B，最后原子交换索引。
//
// 为什么失败不覆盖快照：
//   采样失败（遍历链断了、正在切图）不代表"周围没有怪"。
//   用空快照覆盖会让上层误判成"安全"，然后白白走进去。
//   所以失败时保留上一份有效快照，只累计失败次数。
//
// ★ 时基由调用方注入，本文件不调任何时间 API。
// ============================================================
#include <atomic>
#include <cstdint>
#include <vector>

namespace poll {

// 一次采样的结果。定长上限 + 实际数量，
// 避免每次 tick 重新分配（连续跑几个小时这个开销不小）。
struct Snapshot {
    static constexpr size_t kMaxPoints = 512;

    int32_t    xs[kMaxPoints] = {};
    int32_t    ys[kMaxPoints] = {};
    uint64_t   keys[kMaxPoints] = {};
    size_t     count      = 0;

    uint64_t   stampMs    = 0;      // 采样时刻
    bool       valid      = false;  // 这份快照是否来自一次成功采样

    void Clear()
    {
        count = 0;
        valid = false;
    }

    // 满了就丢后面的（先到先得）。真实场景 512 个同屏目标已是上限。
    void Add(int32_t x, int32_t y, uint64_t key = 0)
    {
        if (count >= kMaxPoints) {
            return;
        }
        xs[count]   = x;
        ys[count]   = y;
        keys[count] = key;
        ++count;
    }
};

// 采集器。Fetch 由调用方实现（lambda / 函数指针 / 虚函数都行）。
//
//   Fetch(Snapshot& out) -> bool
//     true  = 本次采样成功，out 里是完整内容（调用前已 Clear）
//     false = 本次不可用（链断了/切图中/数据尚未就绪）
template <class Fetcher>
class Source {
public:
    // 连续失败几次后上抛"数据源失效"
    void SetMaxConsecutiveFail(uint32_t n) { m_maxFail = n ? n : 1; }

    // 当前是否处于"连续失败超限"状态。上层可据此降级（停止自动行为等）。
    bool Degraded() const { return m_degraded; }

    uint32_t FailStreak() const { return m_failStreak; }

    // 每 tick 调一次。
    //
    //   nowMs  单调时基
    //   period 采样周期（毫秒），未到周期直接返回 false
    //   fetch  采集回调
    //
    // 返回 true 表示本次真的重采了一次（不论成败）。
    template <class F>
    bool Tick(uint64_t nowMs, uint32_t periodMs, F&& fetch)
    {
        if (periodMs == 0) {
            periodMs = 1;
        }
        // ★ 用 hasTick 而不是"lastTick == 0"判断首次。
        //   时基外部注入，第一个 tick 很可能就是 nowMs = 0。
        if (m_hasTick && (nowMs - m_lastTick) < (uint64_t)periodMs) {
            return false;
        }
        m_hasTick  = true;
        m_lastTick = nowMs;

        // 往"非当前读"的那一块写
        Snapshot& w = m_buf[1 - m_readIdx.load(std::memory_order_relaxed)];

        w.Clear();
        w.stampMs = nowMs;

        const bool ok = fetch(w);

        if (ok) {
            w.valid       = true;
            m_failStreak  = 0;
            m_degraded    = false;
            // 发布：读侧从此看到这一块
            m_readIdx.store(1 - m_readIdx.load(std::memory_order_relaxed),
                            std::memory_order_release);
        }
        else {
            // ★ 失败不覆盖：w 这块丢了，读侧继续看上一份有效快照
            w.valid = false;
            ++m_failStreak;
            if (m_maxFail != 0 && m_failStreak >= m_maxFail) {
                m_degraded = true;
            }
        }
        return true;
    }

    // 读当前快照（不加锁，返回的引用在下一次 Tick 成功前保持稳定）
    const Snapshot& Peek() const
    {
        return m_buf[m_readIdx.load(std::memory_order_acquire)];
    }

    // 从没成功采过样
    bool NeverSampled() const
    {
        return !m_buf[m_readIdx.load(std::memory_order_acquire)].valid;
    }

    void Reset()
    {
        m_buf[0].Clear();
        m_buf[1].Clear();
        m_readIdx.store(0, std::memory_order_relaxed);
        m_lastTick   = 0;
        m_hasTick    = false;
        m_failStreak = 0;
        m_degraded   = false;
    }

private:
    Snapshot             m_buf[2];
    std::atomic<int>     m_readIdx{ 0 };
    uint64_t             m_lastTick    = 0;
    bool                 m_hasTick     = false;   // ★ 不用 lastTick==0 判首次
    uint32_t             m_failStreak  = 0;
    uint32_t             m_maxFail     = 5;
    bool                 m_degraded    = false;
};

} // namespace poll
