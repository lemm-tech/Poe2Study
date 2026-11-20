#pragma once
#include "Protocal.h"
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace packet {
constexpr uint32_t kVersion = 1, kMaxBytes = 1536;
constexpr uint32_t kActionOpcode = 0x1A5, kEndOpcode = 0x1AC;
constexpr uint32_t kInteractAction = 0x2000C266, kInteractFlags = 0xFF000400;
constexpr uint32_t kPickAction = kInteractAction;
// Append new operations so existing command values remain stable.
enum class Operation : uint32_t { Status = 1, Pick, Action, EndAction, SendPlain, Capture, EnterPortal, TeleportToPartyMember, InteractRitual, ConfigureRecovery, RecoveryStatus, SummonPortal, ConfigureView, ViewStatus, RevealMinimap };
inline bool Sends(Operation operation) {
    return operation == Operation::Pick || operation == Operation::Action || operation == Operation::EndAction ||
           operation == Operation::SendPlain || operation == Operation::EnterPortal || operation == Operation::TeleportToPartyMember ||
           operation == Operation::InteractRitual || operation == Operation::SummonPortal;
}

// Required for summoning; optional for existing manual EnterPortal clients.
// These are identities to compare with current memory, never pointers to invoke.
struct PortalScene {
    uint64_t world = 0, mapOwner = 0, record = 0, player = 0;
    bool operator==(const PortalScene& other) const {
        return world == other.world && mapOwner == other.mapOwner && record == other.record && player == other.player;
    }
};
static_assert(sizeof(PortalScene) == 32, "portal scene identity layout");

constexpr uint16_t kPartyTeleportOpcode = 0xB5;
constexpr uint32_t kPartyNameLimit = 96, kPartyAccountLimit = 128;
// Counted UTF-16 identities in Request.bytes; never transmit a process pointer.
struct PartyTeleportTarget {
    uint32_t nameLength = 0, accountLength = 0;
    char16_t name[kPartyNameLimit + 1]{}, account[kPartyAccountLimit + 1]{};
};
template<std::size_t N> inline bool ValidIdentity(const char16_t (&text)[N], uint32_t length) {
    if (!length || length >= N || text[length]) return false;
    for (uint32_t i = 0; i < length; ++i) {
        const auto ch = text[i];
        if (ch < 0x20 || ch == 0x7F || (ch >= 0xDC00 && ch <= 0xDFFF)) return false;
        if (ch >= 0xD800 && ch <= 0xDBFF &&
            (++i >= length || text[i] < 0xDC00 || text[i] > 0xDFFF)) return false;
    }
    return true;
}
inline bool ValidTarget(const PartyTeleportTarget& target) {
    return ValidIdentity(target.name, target.nameLength) && ValidIdentity(target.account, target.accountLength);
}
static_assert(sizeof(PartyTeleportTarget) == 460, "party teleport identity layout");
enum class Result : uint32_t {
    Ok = 0, BadRequest, UnsupportedBuild, ContextUnavailable, TargetMissing,
    ContextChanged, Exception, Busy, HookFailed
};
enum : uint32_t { Send = 1, Receive = 2, AppendEnd = 1, PreviewOnly = 2, Truncated = 1 };
#pragma pack(push, 1)
struct Request {
    uint32_t version = kVersion;
    Operation operation = Operation::Status;
    uint32_t options = 0, captureMask = 0, itemKey = 0, actionId = 0;
    int32_t x = 0, y = 0;
    uint32_t actionFlags = 0x400, length = 0;
    uint64_t expectedEntity = 0;
    uint8_t bytes[kMaxBytes]{};
};
struct Reply {
    uint32_t version = kVersion;
    Result result = Result::BadRequest;
    uint32_t invoked = 0, exceptionCode = 0, captureMask = 0, objectLength = 0;
    uint64_t captured = 0, dropped = 0;
    uint8_t object[48]{};
    char detail[160]{};
};
struct CaptureEvent {
    uint32_t version = kVersion, direction = 0, flags = 0;
    uint32_t originalLength = 0, length = 0, threadId = 0;
    uint64_t sequence = 0, tick = 0, streamOffset = 0;
    uint8_t bytes[kMaxBytes]{};
};
#pragma pack(pop)
static_assert(sizeof(Request) == 1584, "packet request layout");
static_assert(sizeof(Reply) == 248, "packet reply layout");
static_assert(sizeof(CaptureEvent) == 1584, "packet capture layout");
static_assert(sizeof(Request) <= MSG_LEN && sizeof(CaptureEvent) <= MSG_LEN, "packet wire limit");
class Message : public PacketHeader {
public:
    explicit Message(const Request& request) {
        m_MsgType = MT_PacketCommand; m_len = sizeof(request);
        std::memcpy(body, &request, sizeof(request));
    }
    uint8_t body[sizeof(Request)]{};
};
} // namespace packet
