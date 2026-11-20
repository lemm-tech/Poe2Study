#pragma once
#include "ManagedProtocol.h"
#include "ComboProtocol.h"
#include "PacketProtocol.h"
#include "Recovery.h"
#include "ViewControls.h"
#include <initializer_list>

// Stable, read-only export. Bump the ABI for incompatible field meanings,
// enum values, packing, or session behavior, even if sizeof stays unchanged.
namespace monitorabi {
constexpr char kExportName[] = "MonitorInterfaceInfo";
constexpr uint32_t kMagic = 0x494D3250;
constexpr uint32_t kAbiVersion = 3;
constexpr uint32_t kPidBoundSessions = 1;
constexpr uint32_t kPacketTools = 2;
// Server action mode must send the stop even when Player+0x214 is unchanged.
constexpr uint32_t kComboServerRelease = 4;
constexpr uint32_t kPortalInteraction = 8;
constexpr uint32_t kPartyTeleport = 16;
constexpr uint32_t kRitualInteraction = 32;
constexpr uint32_t kAutomaticRecovery = 64;
constexpr uint32_t kPortalSummon = 128;
constexpr uint32_t kStationaryDodgeRoll = 256;
// DodgeRoll's native seventh argument carries 0xFF; reject the earlier DLL.
constexpr uint32_t kDodgeRollNativeFlags = 512;
constexpr uint32_t kViewControls = 1024;
constexpr uint32_t kWheelZoomLimit = 2048;

constexpr uint64_t WireLayout()
{
    uint64_t hash = 14695981039346656037ull;
    for (uint64_t value : std::initializer_list<uint64_t>{
        sizeof(PacketHeader), MSG_LEN, sizeof(managed::Hello),
        sizeof(managed::Welcome), sizeof(managed::Envelope),
        sizeof(managed::ErrorReply), sizeof(managed::GameStatus),
        sizeof(MapMeta), sizeof(MapRowHeader), sizeof(NavStatus), sizeof(NavGo),
        sizeof(NavBatchHeader), sizeof(NavBatchChunk), sizeof(NavCfg),
        sizeof(ScanBmpReq), sizeof(WalkScanReq), sizeof(WalkScanAck),
        sizeof(SkillCastReq), sizeof(SkillAck), sizeof(SkillStopReq),
        sizeof(SkillStopAck), sizeof(WalkCallReq), sizeof(WalkCallAck),
        sizeof(combo::CastRequest), sizeof(combo::CastReply),
        sizeof(recovery::Config), sizeof(recovery::Snapshot),
        sizeof(viewcontrols::Config), sizeof(viewcontrols::Snapshot),
        packet::kVersion, sizeof(packet::Request), sizeof(packet::Reply), sizeof(packet::CaptureEvent), sizeof(packet::PortalScene),
        MT_ManagedHello, MT_ManagedWelcome, MT_ManagedPacket, MT_ComboCastAck, MT_PacketCapture}) {
        hash = (hash ^ value) * 1099511628211ull;
    }
    return hash;
}

#pragma pack(push, 1)
struct Info {
    uint32_t magic = kMagic;
    uint32_t bytes = 32;
    uint32_t abiVersion = kAbiVersion;
    uint32_t managedVersion = managed::kVersion;
    uint32_t comboVersion = combo::kWireVersion;
    uint32_t capabilities = kPidBoundSessions | kPacketTools | kComboServerRelease | kPortalInteraction | kPartyTeleport |
                            kRitualInteraction | kAutomaticRecovery | kPortalSummon | kStationaryDodgeRoll |
                            kDodgeRollNativeFlags | kViewControls | kWheelZoomLimit;
    uint64_t wireLayout = WireLayout();
};
#pragma pack(pop)
static_assert(sizeof(Info) == 32, "stable Monitor interface descriptor");

inline bool Compatible(const Info& info)
{
    const Info expected{};
    return info.magic == expected.magic && info.bytes == expected.bytes &&
           info.abiVersion == expected.abiVersion &&
           info.managedVersion == expected.managedVersion &&
           info.comboVersion == expected.comboVersion &&
           (info.capabilities & expected.capabilities) == expected.capabilities &&
           info.wireLayout == expected.wireLayout;
}
} // namespace monitorabi
