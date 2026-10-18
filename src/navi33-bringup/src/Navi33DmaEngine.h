// Navi33DmaEngine - the SDMA6 copy engine as one unit: GPU-visible buffers
// (dma_backend), the batched command ring (sdma6::Ring), and the register
// path that tells the hardware where the ring ends and how far it got.
//
// A register write needs three independent things: a build that defines
// COBALT_NAVI33_ENABLE_UNSAFE_MMIO (sdma6_mmio::kWriteBackendCompiled), the
// caller setting Config::hardwareSubmit, and a register port. The production
// kext defines none of them. The queue must already be configured and enabled
// by an owner (sdma6_mmio::Queue); this engine never writes RB_CNTL or RB_BASE.
// Register reads have no side effects and are not gated by the build flag.
// Ring is one-shot: stop() is terminal, so a buffer's fence cannot be reused.
#pragma once
#include "Navi33DmaBackend.h"
#include "Navi33Sdma6MmioQueue.h"

namespace navi33 {
namespace dma_engine {

enum class Code : uint8_t {
    Pass = 0, NotStarted, AlreadyStarted, Busy, BadRing, WritesDisabled,
    NoPort, PortFailed, ReadFailed, RetireMismatch, RingFault
};

// Plain function pointers, matching sdma6_mmio::IO. A null pointer is refused.
struct Port {
    void *ctx = nullptr;
    bool (*read)(void *ctx, uint32_t reg, uint32_t *value) = nullptr;
    bool (*write)(void *ctx, uint32_t reg, uint32_t value) = nullptr;
};

struct Config {
    dma_backend::Config backend{};
    bool hardwareSubmit = false;  // runtime gate, closed by default
    // 0 = SDMA0 queue 0. Use sdma6_mmio::kQueue1DwordDelta for queue 1.
    uint32_t queueDelta = 0u;
};

class Engine {
public:
    explicit Engine(const Config &config)
        : config_(config), backend_(config.backend) {}

    // `ringStorage` must be CPU-visible and, on hardware, the device-mapped
    // ring page. ringDwords is a power of two in [8, 4096].
    Code start(uint32_t *ringStorage, size_t ringDwords);
    Code stop();
    bool started() const { return started_; }

    dma_backend::Code attach(dma_backend::Translation translation,
                             const uint64_t *busPages, size_t pages,
                             dma_backend::Handle &out) {
        return backend_.attach(translation, busPages, pages, out);
    }
    dma_backend::Code map(dma_backend::Handle handle) {
        return backend_.map(handle);
    }
    dma_backend::Code unmap(dma_backend::Handle handle) {
        return backend_.unmap(handle);
    }
    dma_backend::Code release(dma_backend::Handle handle) {
        return backend_.release(handle);
    }
    dma_backend::Code quarantine(dma_backend::Handle handle) {
        return backend_.quarantine(handle);
    }

    // Queues one SDMA copy. Nothing reaches hardware until ringDoorbell().
    dma_backend::Code copy(const dma_backend::CopyRequest &request,
                           uint64_t &fence);

    // Writes the ring's write pointer (bytes, ring-relative) for every queued
    // copy. Same order as sdma6_mmio::Queue::submit(): high word, then low.
    Code ringDoorbell(const Port &port);
    // Reads the hardware read pointer and retires only the whole items it has
    // passed. Fences complete in the backend only through this path.
    Code pollCompletion(const Port &port);

    uint64_t completedFence() const { return backend_.completedFence(); }
    const dma_backend::Backend &backend() const { return backend_; }

private:
    Config config_;
    dma_backend::Backend backend_;
    sdma6::Ring ring_;
    size_t ringDwords_ = 0u;
    bool started_ = false;
    bool everStarted_ = false;
};

inline Code Engine::start(uint32_t *ringStorage, size_t ringDwords) {
    if (started_ || everStarted_) return Code::AlreadyStarted;
    if (!ring_.reset(ringStorage, ringDwords)) return Code::BadRing;
    ringDwords_ = ringDwords;
    started_ = true;
    everStarted_ = true;
    return Code::Pass;
}

inline Code Engine::stop() {
    if (!started_) return Code::NotStarted;
    if (ring_.inFlightBatches() != 0u) return Code::Busy;
    started_ = false;
    return Code::Pass;
}

inline dma_backend::Code Engine::copy(const dma_backend::CopyRequest &request,
                                      uint64_t &fence) {
    if (!started_) {
        fence = 0u;
        return dma_backend::Code::WrongState;
    }
    return backend_.submitCopy(ring_, request, fence);
}

inline Code Engine::ringDoorbell(const Port &port) {
    if (!sdma6_mmio::kWriteBackendCompiled || !config_.hardwareSubmit)
        return Code::WritesDisabled;
    if (!started_) return Code::NotStarted;
    if (!port.write) return Code::NoPort;
    // The ring never reports full at equal pointers (one dword stays free),
    // so a ring-relative write pointer is unambiguous.
    const uint64_t pos = ring_.publishedDwords() & (ringDwords_ - 1u);
    const uint32_t wptr = static_cast<uint32_t>(pos << 2u);
    if (!port.write(port.ctx, sdma6_mmio::kWptrHi + config_.queueDelta, 0u) ||
        !port.write(port.ctx, sdma6_mmio::kWptr + config_.queueDelta, wptr))
        return Code::PortFailed;
    return Code::Pass;
}

inline Code Engine::pollCompletion(const Port &port) {
    if (!started_) return Code::NotStarted;
    if (!port.read) return Code::NoPort;
    uint32_t rptr = 0u;
    // All-ones means the block is not answering, not "work done".
    if (!port.read(port.ctx, sdma6_mmio::kRptr + config_.queueDelta, &rptr) ||
        rptr == UINT32_MAX)
        return Code::ReadFailed;

    // Hardware pointers are ring-relative, so the movement since the last
    // retire is modular. Items must not be claimed beyond what was published.
    const uint64_t mask = ringDwords_ - 1u;
    const uint64_t hwDwords = (static_cast<uint64_t>(rptr) >> 2u) & mask;
    const uint64_t consumed = ring_.consumedDwords();
    const uint64_t delta = (hwDwords - (consumed & mask)) & mask;
    if (delta > ring_.publishedDwords() - consumed)
        return Code::RetireMismatch;

    // Retire whole items only. A read taken mid-packet waits for the next poll.
    const uint64_t target = consumed + delta;
    for (;;) {
        const uint64_t item = ring_.nextItemDwords();
        if (item == 0u || ring_.consumedDwords() + item > target) break;
        if (ring_.consume() != sdma6::RingCode::Pass) return Code::RingFault;
    }
    backend_.observeCompletion(ring_.lastCompletedFence());
    return Code::Pass;
}

} // namespace dma_engine
} // namespace navi33
