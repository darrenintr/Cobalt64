// Navi33 DCN3.2.1 read-only register OFFSET table, no GPU I/O.
// Values and BASE_IDX verified against Linux AMD MIT licensed header:
// drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_2_1_offset.h
// Copyright (C) 2022 Advanced Micro Devices, Inc.
// Does NOT reset firmware GOP, train DP, set mode, read MMIO or write pixels.
#pragma once
#include "Navi33HardwarePlan.h"

namespace navi33 {
namespace dcn321 {

// Whitelist: conservative status / scanout metadata; no write API exists.
// Base segment 2 is taken from IP Discovery DMU(271) on this device.
// The four display pipes must be individually inspected; pipe0 may be idle.
enum class Register : uint8_t {
    VmFbBase = 0, VmFbTop,
    Pipe0Pitch, Pipe0SurfaceLo, Pipe0SurfaceHi, Pipe0FrameCount,
    Pipe1Pitch, Pipe1SurfaceLo, Pipe1SurfaceHi, Pipe1FrameCount,
    Pipe2Pitch, Pipe2SurfaceLo, Pipe2SurfaceHi, Pipe2FrameCount,
    Pipe3Pitch, Pipe3SurfaceLo, Pipe3SurfaceHi, Pipe3FrameCount,
    Count
};

struct RegisterSpec {
    Register key;
    const char *linuxName;
    uint8_t baseSegment;
    uint32_t regDwordOffset;
};

constexpr RegisterSpec kReadOnlyRegisters[] = {
    {Register::VmFbBase, "regDCN_VM_FB_LOCATION_BASE", 2u, 0x0475u},
    {Register::VmFbTop, "regDCN_VM_FB_LOCATION_TOP", 2u, 0x0476u},
    {Register::Pipe0Pitch, "regHUBPREQ0_DCSURF_SURFACE_PITCH", 2u, 0x0607u},
    {Register::Pipe0SurfaceLo, "regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS", 2u, 0x060au},
    {Register::Pipe0SurfaceHi, "regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2u, 0x060bu},
    {Register::Pipe0FrameCount, "regOTG0_OTG_STATUS_FRAME_COUNT", 2u, 0x1b4cu},
    {Register::Pipe1Pitch, "regHUBPREQ1_DCSURF_SURFACE_PITCH", 2u, 0x06e3u},
    {Register::Pipe1SurfaceLo, "regHUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 2u, 0x06e6u},
    {Register::Pipe1SurfaceHi, "regHUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2u, 0x06e7u},
    {Register::Pipe1FrameCount, "regOTG1_OTG_STATUS_FRAME_COUNT", 2u, 0x1bccu},
    {Register::Pipe2Pitch, "regHUBPREQ2_DCSURF_SURFACE_PITCH", 2u, 0x07bfu},
    {Register::Pipe2SurfaceLo, "regHUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 2u, 0x07c2u},
    {Register::Pipe2SurfaceHi, "regHUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2u, 0x07c3u},
    {Register::Pipe2FrameCount, "regOTG2_OTG_STATUS_FRAME_COUNT", 2u, 0x1c4cu},
    {Register::Pipe3Pitch, "regHUBPREQ3_DCSURF_SURFACE_PITCH", 2u, 0x089bu},
    {Register::Pipe3SurfaceLo, "regHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS", 2u, 0x089eu},
    {Register::Pipe3SurfaceHi, "regHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2u, 0x089fu},
    {Register::Pipe3FrameCount, "regOTG3_OTG_STATUS_FRAME_COUNT", 2u, 0x1cccu}
};

constexpr size_t kReadOnlyCount = sizeof(kReadOnlyRegisters)/sizeof(kReadOnlyRegisters[0]);
static_assert(kReadOnlyCount == static_cast<size_t>(Register::Count), "DCN register coverage mismatch");
static_assert(kReadOnlyRegisters[5].regDwordOffset == 0x1b4cu &&
              kReadOnlyRegisters[5].baseSegment == 2u,
              "DCN3.2.1 must not use DCN4 register layouts");

inline RegisterAddress resolveReadOnly(const IPDiscovery &discovery,
                                      Register key, uint64_t bar5Bytes) {
    const size_t index = static_cast<size_t>(key);
    if (index >= kReadOnlyCount)
        return {AddressCode::MissingSegment, 0u};
    const RegisterSpec &spec = kReadOnlyRegisters[index];
    return resolveDword(discovery, Engine::DCN321, spec.baseSegment,
                        spec.regDwordOffset, bar5Bytes);
}

struct ReadPlan {
    AddressCode code = AddressCode::InventoryRejected;
    uint8_t resolved = 0;
    uint64_t offsets[kReadOnlyCount]{};
};

inline ReadPlan buildReadPlan(const IPDiscovery &discovery,
                             uint64_t assignedBar5Bytes) {
    ReadPlan out{};
    for (size_t i = 0; i < kReadOnlyCount; ++i) {
        const RegisterAddress loc = resolveReadOnly(discovery,
            kReadOnlyRegisters[i].key, assignedBar5Bytes);
        if (loc.code != AddressCode::Pass) {
            out.code = loc.code;
            return out;
        }
        out.offsets[i] = loc.bar5ByteOffset;
        ++out.resolved;
    }
    out.code = AddressCode::Pass;
    return out;
}

// These are GPU-MC addresses, not necessarily PCI physical addresses.
// The frame-count values cannot be sampled until explicit read-only MMIO
// has a safe, separately reviewed implementation.
constexpr uint64_t combineSurfaceAddress(uint32_t low, uint32_t high) {
    return (static_cast<uint64_t>(high) << 32u) | low;
}

} // namespace dcn321
} // namespace navi33
