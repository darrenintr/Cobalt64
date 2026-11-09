// SPDX-License-Identifier: MIT
// RX 7600 Linux boot parity: an OFFLINE stage-order contract, not a GPU driver.
// Reference: torvalds/linux 7475ea8b58616b33c04afb976cf0fbaad9dd63fb.
// This file has no MMIO/PCI/BAR/DMA/firmware/doorbell operations and is
// deliberately NOT linked to Navi33Probe.kext. A matching trace is NOT hardware
// execution evidence; every stage requires its own independent silicon proof.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace linux_boot_parity {

enum class Stage : uint8_t {
    IpDiscovery, CommonHardware, GmcHardware, IhHardware,
    PspFirmwareLoad, SmuHardware,
    GfxRlcAutoloadWait, GfxhubEnable, GfxGoldenRegisters, GfxRlcResume,
    CpDoorbellRanges, MesKiqEnable, MesKiqQueueInit,
    MesSchedulerQueueMap, MesSetHwResources, MesSetHwResources1,
    MesQuerySchedulerStatus, MesKiqOwnershipTransfer,
    KcqMqdInit, MesMapComputeQueues, KgqMqdInit, MesMapGfxQueues,
    GfxStart, GfxRingTest, ComputeRingTest
};
constexpr size_t kMaxStages = 25u;
constexpr uint32_t kLegacyMesVersion = 0x47u;
constexpr uint32_t kResources1MesVersion = 0x52u;
constexpr uint32_t kMesVersionMask = 0xfffu;

enum class Status : uint8_t {
    Ready, WrongSilicon, WrongLinuxPath, MissingOwnership,
    FirmwareUnsupported, DirectQueueConflict, NoPlan, TraceMismatch
};

struct Config {
    bool exactGc1102 = false;
    bool pspFirmwarePath = false;
    bool mesKiq = false;
    bool asyncGfx = false;
    bool exclusiveCardOwner = false;
    bool retainedGpuMemory = false;
    bool g34G42Enabled = false;
    uint32_t mesSchedulerVersion = 0;
};
struct Plan {
    Status status = Status::NoPlan;
    Stage stages[kMaxStages]{};
    size_t count = 0;
    bool ready() const { return status == Status::Ready; }
};

// A scheduling checklist only. Every stage is labelled with the corresponding
// Linux function in LINUX_BOOT_PARITY.md. This does not execute a stage or
// authorize its GPU register writes.
inline Plan makePlan(const Config& c) {
    Plan p{};
    if (!c.exactGc1102) { p.status = Status::WrongSilicon; return p; }
    if (!c.pspFirmwarePath || !c.mesKiq || !c.asyncGfx) {
        p.status = Status::WrongLinuxPath; return p;
    }
    if (!c.exclusiveCardOwner || !c.retainedGpuMemory) {
        p.status = Status::MissingOwnership; return p;
    }
    if (c.g34G42Enabled) { p.status = Status::DirectQueueConflict; return p; }
    if ((c.mesSchedulerVersion & kMesVersionMask) < kLegacyMesVersion) {
        p.status = Status::FirmwareUnsupported; return p;
    }
    const Stage beforeResources[] = {
        Stage::IpDiscovery, Stage::CommonHardware, Stage::GmcHardware,
        Stage::IhHardware, Stage::PspFirmwareLoad, Stage::SmuHardware,
        Stage::GfxRlcAutoloadWait, Stage::GfxhubEnable,
        Stage::GfxGoldenRegisters, Stage::GfxRlcResume,
        Stage::CpDoorbellRanges, Stage::MesKiqEnable,
        Stage::MesKiqQueueInit, Stage::MesSchedulerQueueMap,
        Stage::MesSetHwResources
    };
    for (const Stage s : beforeResources) p.stages[p.count++] = s;
    if ((c.mesSchedulerVersion & kMesVersionMask) >= kResources1MesVersion)
        p.stages[p.count++] = Stage::MesSetHwResources1;
    const Stage afterResources[] = {
        Stage::MesQuerySchedulerStatus, Stage::MesKiqOwnershipTransfer,
        Stage::KcqMqdInit, Stage::MesMapComputeQueues,
        Stage::KgqMqdInit, Stage::MesMapGfxQueues,
        Stage::GfxStart, Stage::GfxRingTest, Stage::ComputeRingTest
    };
    for (const Stage s : afterResources) p.stages[p.count++] = s;
    p.status = Status::Ready;
    return p;
}
struct OrderCheck {
    Status status = Status::NoPlan;
    size_t firstMismatch = 0;
};
inline OrderCheck checkRecordedOrder(const Plan& plan,
                                    const Stage* observed, size_t count) {
    if (!plan.ready()) return {plan.status, 0u};
    if (!observed || count != plan.count)
        return {Status::TraceMismatch, count < plan.count ? count : plan.count};
    for (size_t i = 0; i < count; ++i)
        if (observed[i] != plan.stages[i])
            return {Status::TraceMismatch, i};
    return {Status::Ready, count};
}

} } // namespace navi33::linux_boot_parity
