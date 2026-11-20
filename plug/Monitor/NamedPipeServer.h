#pragma once
#include "../Common/ManagedProtocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// Every reply carries an immutable route, never a reusable slot index.
class NamedPipeServer
{
  public:
    using DispatchCallback = std::function<void(MessageType, const void *, size_t, const managed::Route &)>;
    NamedPipeServer(const std::wstring &, DispatchCallback);
    ~NamedPipeServer();
    bool Initialize();
    bool Uninitialize();
    bool SendPackageTo(const managed::Route &, const void *);
    bool Reply(const managed::Route &, MessageType, const void *, size_t, bool final = false);
    void Publish(MessageType, const void *, size_t);
    void PublishPackage(const void *);
    bool IsConnected() const;
    bool IsRouteCurrent(const managed::Route &) const;
    uint64_t SessionId() const
    {
        return m_session;
    }

  private:
    struct Task
    {
        managed::Route route;
        MessageType type;
        std::vector<uint8_t> body;
    };
    struct Out
    {
        managed::Route route;
        std::vector<uint8_t> bytes;
        uint64_t generation = 0;
    };
    struct Slot
    {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        std::atomic_bool connected{false}, fault{false};
        managed::Welcome identity{};
        uint32_t peerPid = 0, subscriptions = 0;
        std::atomic<uint64_t> generation{0};
        bool walkBusy = false;
        uint64_t rxSequence = 0, rxRequest = 0, txSequence = 0, lastHeartbeat = 0;
        std::mutex queueMutex, writeMutex;
        std::condition_variable cv;
        std::deque<Out> out;
        std::deque<Task> tasks;
        std::thread reader, writer, worker;
    };
    static constexpr size_t kPoolSize = 12, kQueueLimit = 2048;
    std::wstring m_name;
    DispatchCallback m_callback;
    std::vector<std::unique_ptr<Slot>> m_slots;
    mutable std::mutex m_registry;
    std::atomic_bool m_running{false};
    HANDLE m_stop = nullptr;
    uint64_t m_session = 0, m_nextClient = 0;
    Slot *FindLocked(const managed::Route &) const;
    void Drop(Slot &);
    void ReadLoop(Slot &);
    void WriteLoop(Slot &);
    void WorkLoop(Slot &);
    bool Handle(Slot &, MessageType, const void *, size_t);
    bool Enqueue(Slot &, Out);
};
