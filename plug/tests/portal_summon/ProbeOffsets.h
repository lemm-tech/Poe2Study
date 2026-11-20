#pragma once
#include "../../Common/GameOffsets.h"
// The verified entry is now shared with production; independent probe only.
namespace portalprobe {
constexpr uint64_t kSummonRva = kPortalSummonRva;
constexpr uint64_t kSummonGateOff = kPortalSummonGateOff;
constexpr uint16_t kSummonOpcode = 0xD0;
constexpr const auto& kSummonCode = kPortalSummonSignature;
static_assert(sizeof(kSummonCode) == 0x8D, "whole summon function fingerprint");
}
