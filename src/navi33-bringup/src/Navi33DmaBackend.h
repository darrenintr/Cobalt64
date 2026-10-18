// Navi33DmaBackend - host-testable DMA buffer backend between pinned pages,
// GMC11 GART shadow PTEs and SDMA6 copy packets.
// Pure C++17. No IOKit, MMIO, doorbell, PSP, interrupt or GPU submission.
//
// Two translation modes exist on purpose and are kept apart:
//  - IommuMapped: bus addresses come from an OS IOMMU mapping that the
//    platform attests belongs to this device.
//  - PhysicalUnverified: CPU-physical pages used as bus addresses on a board
//    with no IOMMU. DMA isolation is absent, but that does NOT make the GPU
//    unable to reach those pages. Reachability is simply unproven here, so
//    this mode is refused unless the caller opens its explicit allow gate.
// Mapping into the GART additionally needs gpuGateOpen, which production code
// never sets. A successful result here is not hardware evidence.
#pragma once
#include "Navi33Gmc11Gart.h"
#include "Navi33Sdma6Packet.h"
#include "Navi33Sdma6Ring.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 {
namespace dma_backend {

constexpr size_t kMaxBufferPages = 16u; // 64 KiB, as dma::kMaxPages
constexpr size_t kMaxBuffers = 8u;
constexpr size_t kTableEntries = 256u;  // 1 MiB shadow GART aperture
constexpr uint64_t kGpuVaBase = 0x100000000ull;

enum class Translation : uint8_t { None = 0, IommuMapped, PhysicalUnverified };
enum class State : uint8_t { Free = 0, Host, Mapped, Quarantined };

enum class Code : uint8_t {
    Pass = 0, BadEpoch, BadHandle, BadSize, BadPageList, DuplicatePage,
    BadAddress, PageInUse, NoBuffer, WrongState, Busy, Quarantined,
    Unattested, GateClosed, NoTable, GartRejected, BadRange, RingRejected
};

// Handles carry the owner epoch and the slot generation, so a stale handle
// from an earlier epoch or a released slot is refused rather than aliased.
struct Handle {
    uint32_t slot = 0u;
    uint32_t generation = 0u;
    uint64_t epoch = 0u;
};

struct Config {
    uint64_t epoch = 0u;                  // zero is refused
    bool iommuAttested = false;           // platform attests IOMMU mapping
    bool allowPhysicalUnverified = false; // explicit opt-in, default closed
    bool gpuGateOpen = false;             // may install GART PTEs
};

struct CopyRequest {
    Handle source{};
    uint64_t sourceOffset = 0u;
    Handle destination{};
    uint64_t destinationOffset = 0u;
    uint32_t bytes = 0u;
};

class Backend {
public:
    explicit Backend(const Config &config)
        : config_(config), valid_(config.epoch != 0u) {}

    // Registers already-pinned bus pages. Validates every page before it
    // takes a slot, so a refused attach leaves no state behind.
    Code attach(Translation translation, const uint64_t *busPages,
                size_t pages, Handle &out);
    // Installs GART PTEs for a Host buffer (gated).
    Code map(Handle handle);
    // Removes PTEs of a Mapped buffer whose work has completed.
    Code unmap(Handle handle);
    // Frees a Host buffer whose work has completed. Pages become reusable.
    Code release(Handle handle);
    // Freezes a buffer after an ambiguous fault. Its pages and any PTEs stay
    // reserved; the buffer can never be unmapped, released or reused.
    Code quarantine(Handle handle);
    // Publishes one SDMA copy through the ring and records its fence on both
    // buffers. Nothing is published unless every check passes.
    Code submitCopy(sdma6::Ring &ring, const CopyRequest &request,
                    uint64_t &fence);
    // Advances the completion horizon. Monotonic; never moves backwards.
    void observeCompletion(uint64_t completedFence) {
        if (completedFence > completed_) completed_ = completedFence;
    }

    State state(Handle handle) const;
    gmc11::Aperture aperture() const {
        return {kGpuVaBase, kTableEntries * gmc11::kPageBytes};
    }
    const uint64_t *table() const { return table_; }
    uint64_t completedFence() const { return completed_; }
    size_t liveBuffers() const;

private:
    struct Slot {
        State state = State::Free;
        uint32_t generation = 0u;
        Translation translation = Translation::None;
        size_t pages = 0u;
        uint64_t bytes = 0u;
        uint64_t gpuVa = 0u;
        size_t tableIndex = 0u;
        uint64_t lastFence = 0u;
        uint64_t busPages[kMaxBufferPages]{};
    };

    Code resolve(Handle handle, Slot *&slot);
    bool busy(const Slot &slot) const { return slot.lastFence > completed_; }
    bool pageInUse(uint64_t bus) const;

    Config config_;
    bool valid_;
    Slot slots_[kMaxBuffers]{};
    uint64_t table_[kTableEntries]{};
    uint64_t completed_ = 0u;
};

inline Code Backend::resolve(Handle handle, Slot *&slot) {
    slot = nullptr;
    if (!valid_ || handle.epoch != config_.epoch) return Code::BadEpoch;
    if (handle.slot >= kMaxBuffers) return Code::BadHandle;
    Slot &s = slots_[handle.slot];
    if (s.state == State::Free || s.generation != handle.generation)
        return Code::BadHandle;
    slot = &s;
    return Code::Pass;
}

inline bool Backend::pageInUse(uint64_t bus) const {
    for (const Slot &s : slots_) {
        if (s.state == State::Free) continue;
        for (size_t i = 0; i < s.pages; ++i)
            if (s.busPages[i] == bus) return true;
    }
    return false;
}

inline size_t Backend::liveBuffers() const {
    size_t n = 0u;
    for (const Slot &s : slots_) n += s.state != State::Free ? 1u : 0u;
    return n;
}

inline State Backend::state(Handle handle) const {
    if (!valid_ || handle.epoch != config_.epoch ||
        handle.slot >= kMaxBuffers)
        return State::Free;
    const Slot &s = slots_[handle.slot];
    return s.generation == handle.generation ? s.state : State::Free;
}

inline Code Backend::attach(Translation translation, const uint64_t *busPages,
                            size_t pages, Handle &out) {
    out = Handle{};
    if (!valid_) return Code::BadEpoch;
    if (translation == Translation::None) return Code::Unattested;
    if (translation == Translation::IommuMapped && !config_.iommuAttested)
        return Code::Unattested;
    if (translation == Translation::PhysicalUnverified &&
        !config_.allowPhysicalUnverified)
        return Code::Unattested;
    if (!busPages || pages == 0u || pages > kMaxBufferPages)
        return Code::BadPageList;
    for (size_t i = 0; i < pages; ++i) {
        if (gmc11::makeSystemPte(busPages[i], true).code != gmc11::Code::Pass)
            return Code::BadAddress;
        for (size_t j = 0; j < i; ++j)
            if (busPages[j] == busPages[i]) return Code::DuplicatePage;
        if (pageInUse(busPages[i])) return Code::PageInUse;
    }
    Slot *free = nullptr;
    for (Slot &s : slots_)
        if (s.state == State::Free) { free = &s; break; }
    if (!free) return Code::NoBuffer;

    const uint32_t generation = free->generation;
    *free = Slot{};
    free->generation = generation;
    free->state = State::Host;
    free->translation = translation;
    free->pages = pages;
    free->bytes = static_cast<uint64_t>(pages) * gmc11::kPageBytes;
    for (size_t i = 0; i < pages; ++i) free->busPages[i] = busPages[i];
    out.slot = static_cast<uint32_t>(free - slots_);
    out.generation = generation;
    out.epoch = config_.epoch;
    return Code::Pass;
}

inline Code Backend::map(Handle handle) {
    Slot *s = nullptr;
    const Code c = resolve(handle, s);
    if (c != Code::Pass) return c;
    if (!config_.gpuGateOpen) return Code::GateClosed;
    if (s->state == State::Quarantined) return Code::Quarantined;
    if (s->state != State::Host) return Code::WrongState;

    size_t start = kTableEntries;
    for (size_t i = 0; i + s->pages <= kTableEntries && start == kTableEntries; ++i) {
        size_t j = 0;
        while (j < s->pages && table_[i + j] == 0u) ++j;
        if (j == s->pages) start = i;
    }
    if (start == kTableEntries) return Code::NoTable;

    const gmc11::Aperture a = aperture();
    const uint64_t va = a.gpuVaBase + start * gmc11::kPageBytes;
    const gmc11::Result r = gmc11::mapSystemPages(
        a, va, s->busPages, s->pages, true, table_, kTableEntries);
    if (r.code != gmc11::Code::Pass) return Code::GartRejected;
    s->tableIndex = start;
    s->gpuVa = va;
    s->state = State::Mapped;
    return Code::Pass;
}

inline Code Backend::unmap(Handle handle) {
    Slot *s = nullptr;
    const Code c = resolve(handle, s);
    if (c != Code::Pass) return c;
    if (s->state == State::Quarantined) return Code::Quarantined;
    if (s->state != State::Mapped) return Code::WrongState;
    if (busy(*s)) return Code::Busy;

    const gmc11::Result r = gmc11::unmapSystemPages(
        aperture(), s->gpuVa, s->pages, table_, kTableEntries);
    if (r.code != gmc11::Code::Pass) return Code::GartRejected;
    s->gpuVa = 0u;
    s->tableIndex = 0u;
    s->state = State::Host;
    return Code::Pass;
}

inline Code Backend::release(Handle handle) {
    Slot *s = nullptr;
    const Code c = resolve(handle, s);
    if (c != Code::Pass) return c;
    if (s->state == State::Quarantined) return Code::Quarantined;
    if (s->state != State::Host) return Code::WrongState;
    if (busy(*s)) return Code::Busy;

    const uint32_t next = s->generation + 1u;
    *s = Slot{};
    s->generation = next;
    return Code::Pass;
}

inline Code Backend::quarantine(Handle handle) {
    Slot *s = nullptr;
    const Code c = resolve(handle, s);
    if (c != Code::Pass) return c;
    s->state = State::Quarantined;
    return Code::Pass;
}

inline Code Backend::submitCopy(sdma6::Ring &ring, const CopyRequest &r,
                                uint64_t &fence) {
    fence = 0u;
    Slot *src = nullptr;
    Slot *dst = nullptr;
    Code c = resolve(r.source, src);
    if (c != Code::Pass) return c;
    c = resolve(r.destination, dst);
    if (c != Code::Pass) return c;
    if (src->state == State::Quarantined || dst->state == State::Quarantined)
        return Code::Quarantined;
    if (src->state != State::Mapped || dst->state != State::Mapped)
        return Code::WrongState;
    if (r.bytes == 0u) return Code::BadSize;
    if (r.sourceOffset > src->bytes || r.bytes > src->bytes - r.sourceOffset ||
        r.destinationOffset > dst->bytes ||
        r.bytes > dst->bytes - r.destinationOffset)
        return Code::BadRange;

    const sdma6::CopyPacket packet = sdma6::makeCopy(
        src->gpuVa + r.sourceOffset, dst->gpuVa + r.destinationOffset,
        r.bytes);
    switch (packet.status) {
    case sdma6::CopyCode::Pass: break;
    case sdma6::CopyCode::EmptyCopy:
    case sdma6::CopyCode::TooLarge: return Code::BadSize;
    case sdma6::CopyCode::OverlappingMemory: return Code::BadRange;
    default: return Code::BadAddress;
    }
    const sdma6::RingResult published = ring.publish(&packet, 1u);
    if (published.code != sdma6::RingCode::Pass) return Code::RingRejected;
    fence = published.fence;
    src->lastFence = fence;
    dst->lastFence = fence;
    return Code::Pass;
}

constexpr bool kHardwareSubmissionEnabled = false;
static_assert(!kHardwareSubmissionEnabled,
              "The DMA backend is host-verified only; no GPU submission path");

} // namespace dma_backend
} // namespace navi33
