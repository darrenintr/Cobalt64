// SPDX-License-Identifier: MIT
// Navi33 PSP13 MP0 mailbox observer. Read-only and host-testable.
// Registers from Linux amdgpu mp_13_0_2_offset.h, MP0 BASE_IDX 0:
//   SMN_C2PMSG_81 = 0x0091 (sOS sign of life),
//   SMN_C2PMSG_58 = 0x007a (sOS firmware version),
//   SMN_C2PMSG_35 = 0x0063 (ROM bootloader command/status).
// The sOS-alive register is used by Linux psp_v13_0_is_sos_alive().
// psp_v13_0_wait_for_bootloader() polls C2PMSG_35 for exactly 0x80000000
// (ready for a load command); bits[7:0] report a command error.
// These two reads CANNOT prove GFX11/SDMA/VM/Metal are initialized.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace psp13 {
constexpr uint8_t kSegment = 0u;
constexpr uint32_t kAliveDword = 0x0091u;
constexpr uint32_t kVersionDword = 0x007au;
constexpr uint32_t kBootloaderDword = 0x0063u;
constexpr uint32_t kBootloaderReady = 0x80000000u;
constexpr bool kCanWriteMailbox = false;
constexpr bool kCanLoadFirmware = false;
static_assert(!kCanWriteMailbox && !kCanLoadFirmware,
              "A read-only PSP status probe must not boot firmware");

struct Plan {
    n33_psp_state state = N33_PSP_NO_INVENTORY;
    uint64_t aliveOffset = 0u;
    uint64_t versionOffset = 0u;
    uint64_t bootloaderOffset = 0u;
};
inline Plan buildPlan(const IPDiscovery &disc, uint64_t bar5Length) {
    Plan plan{};
    if (inventory(disc).code != InventoryCode::Pass) return plan;
    const RegisterAddress alive = resolveDword(
        disc, Engine::PSP13, kSegment, kAliveDword, bar5Length);
    const RegisterAddress ver = resolveDword(
        disc, Engine::PSP13, kSegment, kVersionDword, bar5Length);
    const RegisterAddress boot = resolveDword(
        disc, Engine::PSP13, kSegment, kBootloaderDword, bar5Length);
    if (alive.code != AddressCode::Pass || ver.code != AddressCode::Pass ||
        boot.code != AddressCode::Pass)
        { plan.state = N33_PSP_BAD_MMIO_PLAN; return plan; }
    if (!alive.bar5ByteOffset || !ver.bar5ByteOffset ||
        alive.bar5ByteOffset == ver.bar5ByteOffset ||
        (alive.bar5ByteOffset & 3u) || (ver.bar5ByteOffset & 3u) ||
        alive.bar5ByteOffset > bar5Length ||
        ver.bar5ByteOffset > bar5Length ||
        bar5Length - alive.bar5ByteOffset < 4u ||
        bar5Length - ver.bar5ByteOffset < 4u) {
        plan.state = N33_PSP_BAD_MMIO_PLAN;
        return plan;
    }
    if (!boot.bar5ByteOffset || (boot.bar5ByteOffset & 3u) ||
        boot.bar5ByteOffset == alive.bar5ByteOffset ||
        boot.bar5ByteOffset == ver.bar5ByteOffset ||
        boot.bar5ByteOffset > bar5Length ||
        bar5Length - boot.bar5ByteOffset < 4u) {
        plan.state = N33_PSP_BAD_MMIO_PLAN;
        return plan;
    }
    plan.aliveOffset = alive.bar5ByteOffset;
    plan.versionOffset = ver.bar5ByteOffset;
    plan.bootloaderOffset = boot.bar5ByteOffset;
    plan.state = N33_PSP_STOPPED; // planning passed; not a hardware verdict
    return plan;
}
using ReadDword = bool (*)(void *ctx, uint64_t byteOffset, uint32_t *value);
struct Sample {
    n33_psp_state state = N33_PSP_DISABLED;
    uint32_t signOfLife = 0u;
    uint32_t sosVersion = 0u;
    uint32_t readCount = 0u;
};
inline Sample readOnce(const Plan &plan, bool optIn,
                       ReadDword reader, void *ctx) {
    Sample result{};
    if (!optIn) return result;
    if (plan.state != N33_PSP_STOPPED) {
        result.state = plan.state;
        return result;
    }
    if (!reader) { result.state = N33_PSP_READ_ERROR; return result; }
    if (!plan.aliveOffset || !plan.versionOffset ||
        plan.aliveOffset == plan.versionOffset ||
        (plan.aliveOffset & 3u) || (plan.versionOffset & 3u)) {
        result.state = N33_PSP_BAD_MMIO_PLAN;
        return result;
    }
    uint32_t first=0u, version=0u, second=0u;
    if (!reader(ctx, plan.aliveOffset, &first)) {
        result.state=N33_PSP_READ_ERROR; return result;
    }
    result.readCount=1u;
    if (first == UINT32_MAX) {
        result.state=N33_PSP_INVALID_READ; return result;
    }
    // Only read the version when the existing firmware says it is alive.
    if (!first) { result.state=N33_PSP_STOPPED; return result; }
    if (!reader(ctx, plan.versionOffset, &version)) {
        result.state=N33_PSP_READ_ERROR; return result;
    }
    result.readCount=2u;
    if (version == UINT32_MAX) {
        result.state=N33_PSP_INVALID_READ; return result;
    }
    if (!reader(ctx, plan.aliveOffset, &second)) {
        result.state=N33_PSP_READ_ERROR; return result;
    }
    result.readCount=3u;
    if (second == UINT32_MAX) {
        result.state=N33_PSP_INVALID_READ; return result;
    }
    if (!second || second != first) {
        result.state=N33_PSP_UNSTABLE; return result;
    }
    result.state=N33_PSP_RUNNING;
    result.signOfLife=first;
    result.sosVersion=version;
    return result;
}
// One read of the bootloader mailbox, whatever the sOS state: a stopped
// sOS above an idle bootloader is what an un-driven dGPU should show.
// Reading never issues a bootloader command; kCanWriteMailbox stays false.
inline bool readBootloader(const Plan &plan, ReadDword reader, void *ctx,
                           uint32_t *value) {
    if (!value || !reader || plan.state != N33_PSP_STOPPED ||
        !plan.bootloaderOffset || (plan.bootloaderOffset & 3u))
        return false;
    return reader(ctx, plan.bootloaderOffset, value);
}
inline n33_psp_status report(const Sample &sample) {
    n33_psp_status out{};
    out.abi_version=N33_ABI_VERSION;
    out.size=sizeof(out);
    out.state=sample.state;
    out.sign_of_life=sample.signOfLife;
    out.sos_version=sample.sosVersion;
    out.read_count=sample.readCount;
    return out;
}
} } // namespace navi33::psp13
