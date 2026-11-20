#pragma once
#include "Protocal.h"

namespace combo {
constexpr uint32_t kWireVersion = 1;
enum class Operation : uint32_t { Begin = 1, Release = 2, DodgeRoll = 3 };
enum class Result : uint32_t {
    Ok = 0, BadRequest = 1, ContextChanged = 2, UnknownSkill = 3,
    Busy = 4, UnsupportedBuild = 5, Exception = 6
};
#pragma pack(push, 1)
struct CastRequest {
    uint32_t version = kWireVersion;
    Operation operation = Operation::Begin;
    uint64_t token = 0, expectedEntity = 0, expectedWorld = 0;
    uint32_t skillId = 0, chargeMs = 0;
    int32_t x = 0, y = 0;
};
struct CastReply {
    uint32_t version = kWireVersion;
    Result result = Result::BadRequest;
    uint64_t token = 0;
    uint64_t pressedAt = 0, releasedAt = 0;
    uint32_t invoked = 0, exceptionCode = 0;
    char detail[160]{};
};
#pragma pack(pop)
static_assert(sizeof(CastRequest) == 48, "combo request layout");
static_assert(sizeof(CastReply) == 200, "combo reply layout");
class CastMessage : public PacketHeader {
public:
    explicit CastMessage(const CastRequest& r) {
        m_MsgType = MT_ComboCast; m_len = sizeof(r); std::memcpy(body, &r, sizeof(r));
    }
    char body[sizeof(CastRequest)]{};
};
} // namespace combo
