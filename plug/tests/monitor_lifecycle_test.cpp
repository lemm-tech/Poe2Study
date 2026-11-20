#include "../Plug/NamedPipeClie.h"
#include "../Common/MonitorInterface.h"
#include "../Common/GameOffsets.h"
#include <cstdio>
#include <string>
#include <windows.h>
int main()
{
    std::wstring baseName = L"Poe2Lifecycle_" + std::to_wstring(GetCurrentProcessId());
    std::wstring name = baseName + L"_" + std::to_wstring(GetCurrentProcessId());
    WritePrivateProfileStringW(L"pipe", L"name", baseName.c_str(), L".\\config.ini");
    uint64_t previous = 0;
    for (int round = 1; round <= 10; ++round)
    {
        HMODULE dll = LoadLibraryW(L".\\Monitor.dll");
        if (!dll)
        {
            printf("load failed=%lu\n", GetLastError());
            return 1;
        }
        auto shutdown = (DWORD(WINAPI *)(LPVOID))GetProcAddress(dll, "MonitorShutdown");
        if (!shutdown)
            return 2;
        const auto descriptor = reinterpret_cast<const monitorabi::Info*>(GetProcAddress(dll, monitorabi::kExportName));
        if (!descriptor || !monitorabi::Compatible(*descriptor) || !(descriptor->capabilities & monitorabi::kPortalSummon) ||
            !(descriptor->capabilities & monitorabi::kStationaryDodgeRoll) || !(descriptor->capabilities & monitorabi::kViewControls) ||
            !(descriptor->capabilities & monitorabi::kWheelZoomLimit))
            return 11;
        if (!pipe::StartManagedConnections(name, 10000))
            return 3;
        auto current = pipe::GetConnectionSnapshot();
        if (current.gamePid != GetCurrentProcessId() || current.sessionId == previous)
            return 4;
        if (current.controllerId > 3 || current.trackerId > 3 || current.dataId > 3)
        {
            printf("unexpected registration churn C=%llu T=%llu D=%llu\n", current.controllerId, current.trackerId,
                   current.dataId);
            return 10;
        }
        previous = current.sessionId;
        pipe::NamedPipeClie client(name, nullptr);
        if (!client.Initialize())
            return 5;
        PacketHeader req{};
        req.m_MsgType = MT_PositionReq;
        std::vector<uint8_t> body;
        if (client.Request(&req, MT_PositionReply, body))
            return 6; // No game in this isolated process.
        auto packetReply = [&](const packet::Request& request, packet::Result expected) {
            packet::Message message(request); packet::Reply reply;
            if (!client.Request(&message, MT_PacketReply, body, 3000) || body.size() != sizeof(reply)) return false;
            std::memcpy(&reply, body.data(), sizeof(reply));
            return reply.version == packet::kVersion && reply.result == expected && !reply.invoked && !reply.exceptionCode;
        };
        packet::Request summon; summon.operation = packet::Operation::SummonPortal;
        if (!packetReply(summon, packet::Result::BadRequest)) return 12;
        const packet::PortalScene scene{0x20000000, 0x20000000, 0x20001000, 0x20002000};
        summon.length = sizeof(scene); std::memcpy(summon.bytes, &scene, sizeof(scene));
        // The fixture host is not the game. Both preview and active commands
        // must fail its build guard without a native invocation.
        summon.options = packet::PreviewOnly;
        if (!packetReply(summon, packet::Result::UnsupportedBuild)) return 13;
        summon.options = 0;
        if (!packetReply(summon, packet::Result::UnsupportedBuild)) return 14;
        summon.options = packet::AppendEnd;
        if (!packetReply(summon, packet::Result::BadRequest)) return 15;
        summon.options = 0; summon.operation = packet::Operation::EnterPortal;
        summon.itemKey = 0xABCD3318; summon.expectedEntity = 0x20003000;
        if (!packetReply(summon, packet::Result::UnsupportedBuild)) return 16;
        summon.length = 1;
        if (!packetReply(summon, packet::Result::BadRequest)) return 17;
        auto rollReply = [&](const combo::CastRequest& request, combo::Result expected) {
            combo::CastMessage message(request); combo::CastReply reply;
            if (!client.Request(&message, MT_ComboCastAck, body, 3000) || body.size() != sizeof(reply)) return false;
            std::memcpy(&reply, body.data(), sizeof(reply));
            return reply.version == combo::kWireVersion && reply.token == request.token && reply.result == expected && !reply.invoked;
        };
        combo::CastRequest roll; roll.operation = combo::Operation::DodgeRoll; roll.token = uint64_t(round);
        roll.skillId = kDodgeRollSkillId + 1;
        if (!rollReply(roll, combo::Result::BadRequest)) return 18;
        roll.skillId = kDodgeRollSkillId; roll.chargeMs = 1;
        if (!rollReply(roll, combo::Result::BadRequest)) return 19;
        roll.chargeMs = 0;
        if (!rollReply(roll, combo::Result::UnsupportedBuild)) return 20;
        roll.operation = combo::Operation::Release;
        if (!rollReply(roll, combo::Result::Ok)) return 21;
        packet::Request view; view.operation = packet::Operation::ConfigureView;
        viewcontrols::Config zoom{1, 3.0f}; view.length = sizeof(zoom);
        std::memcpy(view.bytes, &zoom, sizeof(zoom));
        if (!packetReply(view, packet::Result::UnsupportedBuild)) return 22;
        zoom.maximum = 99.0f; std::memcpy(view.bytes, &zoom, sizeof(zoom));
        if (!packetReply(view, packet::Result::BadRequest)) return 23;
        zoom = {}; std::memcpy(view.bytes, &zoom, sizeof(zoom));
        if (!packetReply(view, packet::Result::Ok)) return 24;
        view = {}; view.operation = packet::Operation::RevealMinimap;
        if (!packetReply(view, packet::Result::UnsupportedBuild)) return 25;
        view.options = packet::PreviewOnly;
        if (!packetReply(view, packet::Result::BadRequest)) return 26;
        view = {}; view.operation = packet::Operation::ViewStatus;
        if (!packetReply(view, packet::Result::Ok)) return 27;
        packet::Request recovery; recovery.operation = packet::Operation::ConfigureRecovery;
        recovery::Config settings; settings.enabled = 1;
        recovery.length = sizeof(settings); std::memcpy(recovery.bytes, &settings, sizeof(settings));
        if (!packetReply(recovery, packet::Result::UnsupportedBuild)) return 28;
        settings.intervalMs[2] = 499; std::memcpy(recovery.bytes, &settings, sizeof(settings));
        if (!packetReply(recovery, packet::Result::BadRequest)) return 29;
        settings = {}; std::memcpy(recovery.bytes, &settings, sizeof(settings));
        if (!packetReply(recovery, packet::Result::Ok)) return 30;
        client.Uninitialize();
        pipe::StopManagedConnections();
        DWORD code = shutdown(nullptr);
        if (code != 1)
            return 7;
        if (!FreeLibrary(dll))
            return 8;
        if (GetModuleHandleW(L"Monitor.dll"))
            return 9;
        printf("ROUND %d: LoadLibrary -> portal/roll/view/recovery capabilities + 19 guarded commands -> 3 registered roles -> unavailable position -> MonitorShutdown=1 -> "
               "FreeLibrary -> unloaded\n",
               round);
    }
    printf("LIFECYCLE passed=10 failed=0\n");
    return 0;
}
