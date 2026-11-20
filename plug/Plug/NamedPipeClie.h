#pragma once
#include "../Common/ManagedProtocol.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
namespace pipe
{
using DispatchCallback = std::function<void(MessageType, const void *, size_t)>;
struct ConnectionSnapshot
{
    bool ready = false;
    // Controller 注册失败时保留服务端返回的原因，供 GUI 区分“未注入”和“被其他 GUI 占用”。
    managed::Error controllerError = managed::Error::Unavailable;
    uint32_t gamePid = 0;
    uint64_t sessionId = 0, controllerId = 0, trackerId = 0, dataId = 0;
    bool Same(const ConnectionSnapshot &b) const
    {
        return ready && b.ready && gamePid == b.gamePid && sessionId == b.sessionId && controllerId == b.controllerId &&
               trackerId == b.trackerId && dataId == b.dataId;
    }
};
class Transport;
struct ClientState;
// A logical request scope on a process-wide fixed role connection. Destroying a
// scope cancels its callbacks, but never closes the shared physical connection.
class NamedPipeClie
{
  public:
    NamedPipeClie(const std::wstring &, DispatchCallback, managed::Role role = managed::Role::Controller);
    ~NamedPipeClie();
    NamedPipeClie(const NamedPipeClie &) = delete;
    NamedPipeClie &operator=(const NamedPipeClie &) = delete;
    bool Initialize();
    bool Uninitialize();
    bool SendPackage(void *);
    bool SendText(const std::string &);
    bool SendDataChunk(const void *, size_t);
    bool Request(const void *msg, MessageType expected, std::vector<uint8_t> &body, uint32_t timeoutMs = 3000);
    bool IsConnected() const;
    uint64_t ClientId() const
    {
        return m_clientId;
    }
    uint64_t SessionId() const
    {
        return m_sessionId;
    }

  private:
    std::wstring m_name;
    managed::Role m_role;
    std::shared_ptr<Transport> m_transport;
    std::shared_ptr<ClientState> m_state;
    uint64_t m_clientId = 0, m_sessionId = 0;
};
bool StartManagedConnections(const std::wstring &, uint32_t timeoutMs = 5000);
// Set before starting transports. Reconnects also enforce PID + process birth time.
void SetExpectedGameIdentity(uint32_t pid, uint64_t creationTime);
void StopManagedConnections();
ConnectionSnapshot GetConnectionSnapshot();
} // namespace pipe
