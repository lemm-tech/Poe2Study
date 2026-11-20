#pragma once
#include "../Common/PacketProtocol.h"
#include "../Common/ManagedProtocol.h"
#include <functional>
namespace packetservice {
packet::Reply Handle(const packet::Request&, const managed::Route&);
void Tick(const std::function<bool(const managed::Route&)>& current,
          const std::function<void(const packet::CaptureEvent&)>& publish);
// Caller holds the same game-action mutex as navigation and skill calls.
void RecoveryTick(const std::function<bool(const managed::Route&)>& current, bool actionBusy);
bool Shutdown();
} // namespace packetservice
