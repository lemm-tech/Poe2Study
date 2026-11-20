#pragma once
#include "Protocal.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
namespace managed
{
constexpr uint32_t kMagic = 0x324E504D;
constexpr uint16_t kVersion = 1;
constexpr uint32_t kHeartbeatMs = 2000, kHeartbeatTimeoutMs = 10000;
enum class Role : uint32_t
{
    None = 0,
    Controller = 1,
    Tracker = 2,
    Data = 3,
    Observer = 4
};
enum class Error : uint32_t
{
    Ok = 0,
    BadProtocol = 1,
    ControllerOwned = 2,
    RoleOwned = 3,
    Forbidden = 4,
    StaleSession = 5,
    BadSequence = 6,
    Busy = 7,
    Unavailable = 8,
    BadRequest = 9
};
enum : uint16_t
{
    Request = 1,
    Response = 2,
    Event = 4,
    Final = 8
};
enum : uint32_t
{
    NavEvents = 1,
    GameEvents = 2,
    LogEvents = 4,
    PacketEvents = 8
};
#pragma pack(push, 1)
struct Hello
{
    uint32_t magic = kMagic;
    uint16_t version = kVersion, reserved = 0;
    Role role = Role::None;
    uint32_t processId = 0;
    uint64_t owner = 0;
    uint32_t subscriptions = 0;
};
struct Welcome
{
    uint32_t magic = kMagic;
    uint16_t version = kVersion, reserved = 0;
    Error error = Error::Ok;
    Role role = Role::None;
    uint64_t clientId = 0, sessionId = 0, owner = 0;
    uint32_t gamePid = 0, heartbeatMs = kHeartbeatMs, timeoutMs = kHeartbeatTimeoutMs;
};
struct Envelope
{
    uint32_t magic = kMagic;
    uint16_t version = kVersion, flags = 0;
    uint64_t clientId = 0, sessionId = 0, requestId = 0, sequence = 0;
    MessageType type = MT_Ack;
    uint32_t payloadLength = 0;
};
struct ErrorReply
{
    Error code = Error::Ok;
    MessageType rejectedType = MT_Ack;
};
struct GameStatus
{
    uint32_t gamePid = 0;
    uint64_t sessionId = 0;
    uint32_t connected = 1;
};
#pragma pack(pop)
static_assert(sizeof(Envelope) == 48, "wire envelope");
constexpr size_t kMaxWirePayload = sizeof(Envelope) + MSG_LEN;
struct Route
{
    uint64_t clientId = 0, sessionId = 0, requestId = 0;
};
inline bool Same(const Route &a, const Route &b)
{
    return a.clientId == b.clientId && a.sessionId == b.sessionId;
}
inline const char *RoleName(Role r)
{
    switch (r)
    {
    case Role::Controller:
        return "Controller";
    case Role::Tracker:
        return "Tracker";
    case Role::Data:
        return "Data";
    case Role::Observer:
        return "Observer";
    default:
        return "None";
    }
}
inline uint32_t DefaultSubscriptions(Role r)
{
    return r == Role::Tracker ? NavEvents | GameEvents :
           r == Role::Observer ? NavEvents | GameEvents | LogEvents | PacketEvents : 0;
}
inline bool CanSubscribe(Role r, uint32_t mask)
{
    return (mask & ~(NavEvents | GameEvents | LogEvents | PacketEvents)) == 0 && (r != Role::Data || mask == 0) &&
           (r != Role::Tracker || (mask & (LogEvents | PacketEvents)) == 0);
}
inline uint32_t EventMask(MessageType t)
{
    return t == MT_NavStatus || t == MT_NavAbort ? NavEvents
           : t == MT_GameStatus                  ? GameEvents
           : t == MT_LogEvent                    ? LogEvents
           : t == MT_PacketCapture               ? PacketEvents
                                                 : 0;
}
inline bool Allowed(Role r, MessageType t)
{
    if (t == MT_Heartbeat || t == MT_Subscribe || t == MT_PositionReq)
        return r != Role::None;
    if (r == Role::Data)
        return t == MT_MapReq || t == MT_ScanBmp;
    if (r != Role::Controller)
        return false;
    switch (t)
    {
    case MT_Text:
    case MT_Exit:
    case MT_DataChunk:
    case MT_NavGo:
    case MT_NavKeyGo:
    case MT_NavKeyStop:
    case MT_NavStop:
    case MT_NavBatch:
    case MT_NavCfg:
    case MT_WalkScan:
    case MT_SkillCast:
    case MT_SkillStop:
    case MT_WalkCall:
    case MT_ComboCast:
    case MT_PacketCommand:
        return true;
    default:
        return false;
    }
}
inline std::vector<uint8_t> Frame(MessageType t, const void *p, size_t n)
{
    if (n > kMaxWirePayload || (n && !p))
        return {};
    std::vector<uint8_t> b(HEADER_LEN + n);
    PacketHeader h{};
    h.m_MsgType = t;
    h.m_len = uint32_t(n);
    memcpy(b.data(), &h, HEADER_LEN);
    if (n)
        memcpy(b.data() + HEADER_LEN, p, n);
    return b;
}
inline std::vector<uint8_t> Frame(Envelope e, const void *p, size_t n)
{
    if (n > MSG_LEN || (n && !p))
        return {};
    e.payloadLength = uint32_t(n);
    std::vector<uint8_t> b(sizeof(e) + n);
    memcpy(b.data(), &e, sizeof(e));
    if (n)
        memcpy(b.data() + sizeof(e), p, n);
    return Frame(MT_ManagedPacket, b.data(), b.size());
}
inline bool Decode(const void *p, size_t n, Envelope &e, const uint8_t *&body)
{
    if (!p || n < sizeof(e))
        return false;
    memcpy(&e, p, sizeof(e));
    if (e.magic != kMagic || e.version != kVersion || e.payloadLength > MSG_LEN || n != sizeof(e) + e.payloadLength)
        return false;
    body = static_cast<const uint8_t *>(p) + sizeof(e);
    return true;
}

// Both pipe endpoints share the same bounded stream framing. Complete packets
// are dispatched directly; only a split header/payload needs to be copied.
class StreamDecoder
{
    std::array<uint8_t, HEADER_LEN + kMaxWirePayload> m_partial{};
    size_t m_used = 0;

  public:
    template <class Dispatch> bool Feed(const uint8_t *data, size_t size, Dispatch &&dispatch)
    {
        if (size && !data)
            return false;
        while (size)
        {
            PacketHeader header{};
            if (!m_used && size >= HEADER_LEN)
            {
                memcpy(&header, data, HEADER_LEN);
                if (header.m_len > kMaxWirePayload)
                    return false;
                const size_t total = HEADER_LEN + size_t(header.m_len);
                if (size >= total)
                {
                    if (!dispatch(header.m_MsgType, data + HEADER_LEN, header.m_len))
                        return false;
                    data += total;
                    size -= total;
                    continue;
                }
            }
            if (m_used < HEADER_LEN)
            {
                const size_t needed = HEADER_LEN - m_used;
                const size_t take = size < needed ? size : needed;
                memcpy(m_partial.data() + m_used, data, take);
                m_used += take;
                data += take;
                size -= take;
                if (m_used < HEADER_LEN)
                    return true;
            }
            memcpy(&header, m_partial.data(), HEADER_LEN);
            if (header.m_len > kMaxWirePayload)
                return false;
            const size_t total = HEADER_LEN + size_t(header.m_len);
            const size_t needed = total - m_used;
            const size_t take = size < needed ? size : needed;
            if (take)
                memcpy(m_partial.data() + m_used, data, take);
            m_used += take;
            data += take;
            size -= take;
            if (m_used < total)
                return true;
            if (!dispatch(header.m_MsgType, m_partial.data() + HEADER_LEN, header.m_len))
                return false;
            m_used = 0;
        }
        return true;
    }
};
} // namespace managed
