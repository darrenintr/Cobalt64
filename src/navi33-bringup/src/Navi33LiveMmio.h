// SPDX-License-Identifier: MIT
// Navi33 DCN 3.2.1 live STATUS read-only probe.
// This file is C++17 host-testable and contains NO IOKit, register writes,
// PCI enablement, GPU command submission, or framebuffer takeover.
//
// Offsets: Linux amdgpu dcn_3_2_1_offset.h (AMD MIT), DMU segment BASE_IDX=2.
// Reading these counters does NOT establish DP/HDMI output or GPU acceleration.
#pragma once
#include "Navi33HardwarePlan.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace live_mmio {
constexpr unsigned kPipeCount = 4u;
constexpr unsigned kRegCount = kPipeCount * 3u;
constexpr uint8_t kDcnSegment = 2u;

struct RegisterTriplet {
    uint32_t control;
    uint32_t clock;
    uint32_t frame;
};
constexpr RegisterTriplet kOtgs[kPipeCount] = {
    {0x1b41u, 0x1b85u, 0x1b4cu},
    {0x1bc1u, 0x1c05u, 0x1bccu},
    {0x1c41u, 0x1c85u, 0x1c4cu},
    {0x1cc1u, 0x1d05u, 0x1cccu}
};
static_assert(kOtgs[0].control == 0x1b41u &&
              kOtgs[3].frame == 0x1cccu,
              "DCN321, NOT DCN41 offsets");

enum class Code : uint8_t {
    Disabled=0, InvalidInventory, InvalidAddress, NoReader,
    MmioReadFailed, UnmappedAllOnes, Pass
};
struct Plan {
    Code code = Code::InvalidInventory;
    AddressCode addressStatus = AddressCode::InventoryRejected;
    uint64_t byteOffsets[kPipeCount][3]{};
};
// A caller-provided, already mapped, DEVICE MMIO read function. The only
// addresses passed to it are the validated BAR5 byte offsets in Plan.
using ReadDword = bool (*)(void *ctx, uint64_t bar5ByteOffset, uint32_t *out);
struct Sample {
    Code code = Code::Disabled;
    uint32_t frame[kPipeCount]{};
    uint8_t activeMask = 0u;
    uint8_t clockMask = 0u;
    uint8_t reads = 0u;
};
inline Plan buildPlan(const IPDiscovery &disc, uint64_t assignedBar5Bytes) {
    Plan plan{};
    if (inventory(disc).code != InventoryCode::Pass)
        return plan;
    for (unsigned pipe=0; pipe<kPipeCount; ++pipe) {
        const uint32_t regs[3] = {kOtgs[pipe].control,
                                   kOtgs[pipe].clock,
                                   kOtgs[pipe].frame};
        for (unsigned reg=0; reg<3u; ++reg) {
            const RegisterAddress loc =
                resolveDword(disc, Engine::DCN321, kDcnSegment,
                             regs[reg], assignedBar5Bytes);
            if (loc.code != AddressCode::Pass) {
                plan.code = Code::InvalidAddress;
                plan.addressStatus = loc.code;
                return plan;
            }
            plan.byteOffsets[pipe][reg] = loc.bar5ByteOffset;
        }
    }
    plan.addressStatus = AddressCode::Pass;
    plan.code = Code::Pass;
    return plan;
}
inline Sample readSnapshot(const Plan &plan, bool explicitlyEnabled,
                           ReadDword read, void *ctx) {
    Sample out{};
    if (!explicitlyEnabled) return out; // absolutely no I/O
    if (plan.code != Code::Pass) {
        out.code = plan.code;
        return out;
    }
    if (!read) {
        out.code = Code::NoReader;
        return out;
    }
    uint32_t frames[kPipeCount]{};
    uint8_t active = 0u, clock = 0u;
    // All offsets MUST be checked before ANY callback. A malformed partial
    // plan is rejected even if the caller forged Plan.code=Pass.
    for (unsigned p=0; p<kPipeCount; ++p) {
        for (unsigned i=0; i<3u; ++i) {
            const uint64_t off=plan.byteOffsets[p][i];
            if ((off & 3ull) != 0ull || off == 0ull ||
                off > UINT64_MAX - 4ull) {
                out.code=Code::InvalidAddress;
                return out;
            }
            for (unsigned pp=0; pp<p; ++pp)
                for (unsigned ii=0; ii<3u; ++ii)
                    if (plan.byteOffsets[pp][ii] == off) {
                        out.code=Code::InvalidAddress;
                        return out;
                    }
            for (unsigned ii=0; ii<i; ++ii)
                if (plan.byteOffsets[p][ii] == off) {
                    out.code=Code::InvalidAddress;
                    return out;
                }
        }
    }
    for (unsigned p=0; p<kPipeCount; ++p) {
        uint32_t regs[3]{};
        for (unsigned i=0; i<3u; ++i) {
            if (!read(ctx, plan.byteOffsets[p][i], &regs[i])) {
                out.code=Code::MmioReadFailed;
                return out;
            }
            ++out.reads;
            if (regs[i] == UINT32_MAX) {
                out.code=Code::UnmappedAllOnes;
                return out;
            }
        }
        if ((regs[0] & 1u) != 0u) active |= static_cast<uint8_t>(1u << p);
        if ((regs[1] & 0x100u) != 0u) clock |= static_cast<uint8_t>(1u << p);
        frames[p] = regs[2] & 0x00ffffffu;
    }
    for (unsigned p=0; p<kPipeCount; ++p) out.frame[p] = frames[p];
    out.activeMask=active;
    out.clockMask=clock;
    out.code=Code::Pass;
    return out;
}
constexpr bool kHasGpuWritePath=false;
constexpr bool kHasGfx11Submission=false;
static_assert(!kHasGpuWritePath && !kHasGfx11Submission,
              "MMIO observer must never initialize or submit GPU workloads");
} } // namespace navi33::live_mmio
