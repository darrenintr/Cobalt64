// SPDX-License-Identifier: MIT
// Cobalt64 Navi 33 MES Route B: pure scheduler ring submission planner.
// Reference: Linux amdgpu mes_v11_0_submit_pkt_and_poll_completion(),
// mes_v11_0_ring_funcs (align_mask=1, 64-bit wptr) and mes_v11_api_def.h.
// No BAR, MMIO, PCI, firmware or doorbell operations occur in this file.
// Intentionally NOT linked into the production kext submission path.
#pragma once

#include "Navi33MesGfxRouteB.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace navi33 { namespace mes_routeb_transport {

namespace mes = mes_gfx_routeb;
constexpr uint32_t kSchedulerDoorbellDword = 0x16u; // NAVI10 MES_RING0 (0x0b << 1)
constexpr uint32_t kSchedulerDoorbellByte = 0x58u;
constexpr uint32_t kKiqDoorbellDword = 0x18u; // MES_RING1 (0x0c << 1)
constexpr uint32_t kKiqDoorbellByte = 0x60u;
constexpr uint32_t kSubmitDwords = 2u * mes::kFrameDwords;
constexpr uint32_t kDefaultRingDwords = 1024u; // mes_v11_0_ring_init: 1024 dwords
static_assert(kSubmitDwords == 128u && kSchedulerDoorbellByte != kKiqDoorbellByte,
              "Separate 2-frame MES scheduler ring and KIQ doorbells");

enum class Step : uint8_t { SetResources, SetResources1, MapGfx };
enum class Code : uint8_t {
    Ready, FirmwareRefused, OwnershipRefused, SchedulerRefused,
    ResourceOrderRefused, PacketRefused, FenceRefused,
    RingRefused, PointerRefused, InsufficientSpace
};

struct Ring {
    uint64_t gpuBase = 0u;
    uint64_t rptr = 0u; // Monotonic 64-bit RPTR from scheduler writeback
    uint64_t wptr = 0u; // Monotonic 64-bit producer WPTR
    uint32_t dwords = kDefaultRingDwords;
    uint32_t doorbellByte = kSchedulerDoorbellByte;
    bool exclusive = false;
};

// Each scatter word is relative to the caller-owned ring buffer. Building
// this plan NEVER modifies memory, making it safe to inspect on a Linux host.
struct Word { uint32_t index = 0u, value = 0u; };
struct Plan {
    Code code = Code::SchedulerRefused;
    Step step = Step::SetResources;
    Word writes[kSubmitDwords]{};
    uint32_t count = 0u;
    uint64_t previousWptr = 0u, nextWptr = 0u, observedRptr = 0u;
    uint32_t doorbellByte = 0u;
    uint64_t expectedApiFence = 0u, apiFenceAddress = 0u;
    uint64_t expectedSchedulerFence = 0u, schedulerFenceAddress = 0u;
    bool ready() const { return code == Code::Ready; }
};
struct Input {
    Step step = Step::SetResources;
    mes::Owner owner{};
    Ring ring{};
    mes::Build command{};
    mes::Packet query{};
    uint64_t seq = 0u; // Must be the caller-owned scheduler fence sequence
};

inline bool isPow2(uint32_t v) { return v && !(v & (v - 1u)); }
inline uint32_t expectedOpcode(Step step) {
    return step == Step::SetResources ? mes::kSetResources :
        step == Step::SetResources1 ? mes::kSetResources1 : mes::kAddQueue;
}
inline Code gate(const Input &in) {
    const mes::Owner &o = in.owner;
    if (!o.exactNavi33 || !o.mesFirmwareLive || !o.freshEpoch)
        return Code::FirmwareRefused;
    if (!o.kiqAndGfxUnowned || !o.g42Disabled)
        return Code::OwnershipRefused;
    if ((o.mesVersion & mes::kVersionMask) < mes::kMinLegacyMesVersion ||
        !o.schedulerQueueLive)
        return Code::SchedulerRefused;
    switch (in.step) {
    case Step::SetResources:
        if (o.schedulerResourcesAck || o.schedulerResources1Ack)
            return Code::ResourceOrderRefused;
        break;
    case Step::SetResources1:
        if ((o.mesVersion & mes::kVersionMask) < mes::kResources1MesVersion ||
            !o.schedulerResourcesAck || o.schedulerResources1Ack)
            return Code::ResourceOrderRefused;
        break;
    case Step::MapGfx:
        if (!mes::readiness(o).ok())
            return Code::ResourceOrderRefused;
        break;
    }
    return Code::Ready;
}

inline Plan prepare(const Input &in) {
    Plan p{};p.step = in.step;
    p.code = gate(in);
    if (!p.ready()) return p;
    if (!in.command.validation.ok() ||
        in.command.packet.dw[0] != mes::header(expectedOpcode(in.step)) ||
        in.command.statusOffset + sizeof(mes::Status) > mes::kFrameBytes ||
        (in.command.statusOffset & 3u))
        { p.code = Code::PacketRefused; return p; }

    mes::Status commandStatus{};
    memcpy(&commandStatus,
           reinterpret_cast<const unsigned char *>(in.command.packet.dw) +
               in.command.statusOffset,
           sizeof(commandStatus));
    mes::QueryFields query{};
    memcpy(&query, in.query.dw, sizeof(query));
    if (query.header.bits != mes::header(mes::kQueryStatus) || query.subopcode != 0u)
        { p.code = Code::PacketRefused; return p; }
    if (!mes::validMc(commandStatus.addr, 8u) ||
        !mes::validMc(query.status.addr, 8u) ||
        commandStatus.addr == query.status.addr ||
        !commandStatus.value || !in.seq ||
        query.status.value != in.seq ||
        in.seq == UINT64_MAX)
        { p.code = Code::FenceRefused; return p; }

    const Ring &r = in.ring;
    if (!mes::validMc(r.gpuBase, 256u) ||
        !isPow2(r.dwords) || r.dwords < 256u || r.dwords > (1u << 16u) ||
        r.doorbellByte != kSchedulerDoorbellByte || !r.exclusive)
        { p.code = Code::RingRefused; return p; }
    if (r.wptr < r.rptr || r.wptr > UINT64_MAX - kSubmitDwords ||
        r.wptr - r.rptr >= r.dwords)
        { p.code = Code::PointerRefused; return p; }
    if (r.wptr - r.rptr + kSubmitDwords >= r.dwords)
        { p.code = Code::InsufficientSpace; return p; }

    p.previousWptr = r.wptr;
    p.nextWptr = r.wptr + kSubmitDwords;
    p.observedRptr = r.rptr;
    p.doorbellByte = r.doorbellByte;
    p.apiFenceAddress = commandStatus.addr;
    p.expectedApiFence = commandStatus.value;
    p.schedulerFenceAddress = query.status.addr;
    p.expectedSchedulerFence = in.seq;
    const uint32_t mask = r.dwords - 1u;
    for (uint32_t i=0; i<mes::kFrameDwords; ++i) {
        p.writes[i] = {static_cast<uint32_t>((r.wptr + i) & mask),
                       in.command.packet.dw[i]};
        p.writes[i + mes::kFrameDwords] = {
            static_cast<uint32_t>((r.wptr + mes::kFrameDwords + i) & mask),
            in.query.dw[i]};
    }
    p.count = kSubmitDwords;
    p.code = Code::Ready;
    return p;
}

struct ObservedFences { uint64_t api = 0u, scheduler = 0u; };
// A completed scheduler QUERY frame does not substitute for the API status.
inline bool complete(const Plan &p, ObservedFences observed) {
    return p.ready() && mes::completed(
        {observed.scheduler, observed.api},
        p.expectedSchedulerFence, p.expectedApiFence);
}

// CPU-owned host-test ring ONLY: do not feed a BAR/VRAM pointer to this.
inline bool applyHostOnly(const Plan &p, uint32_t *buffer, size_t count) {
    if (!p.ready() || !buffer || p.count != kSubmitDwords)
        return false;
    for (unsigned i=0; i<p.count; ++i)
        if (p.writes[i].index >= count)
            return false;
    for (unsigned i=0; i<p.count; ++i)
        buffer[p.writes[i].index] = p.writes[i].value;
    return true;
}

} } // namespace navi33::mes_routeb_transport
