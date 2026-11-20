#pragma once
#include "../Common/ManagedProtocol.h"
#include "../Common/PacketProtocol.h"
#include <functional>

namespace viewservice {
packet::Reply Handle(const packet::Request&, const managed::Route&);
void Tick(const std::function<bool(const managed::Route&)>& current);
bool Shutdown();
}
