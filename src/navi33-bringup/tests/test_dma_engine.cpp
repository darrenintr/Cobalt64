// Navi33DmaEngine on the host. Two builds of this file:
//  - default: the write path is compiled out. Every doorbell must refuse and
//    no register may be written, whatever the runtime gate says.
//  - -DCOBALT_NAVI33_ENABLE_UNSAFE_MMIO: the same engine drives a simulated
//    SDMA6 register block. Copies run through the device model, which keeps
//    its own read pointer, and complete only through the retire path.
// Nothing here touches a GPU or a kext.
#include "dma_mock_gpu.h"
#include "../src/Navi33DmaEngine.h"
#include <cassert>
#include <cstdio>
#include <map>

using namespace navi33;
using dma_engine::Config;
using dma_engine::Engine;
using dma_engine::Port;
using dma_backend::Code;

namespace {

constexpr uint64_t kEpoch = 3u;
constexpr size_t kRingDwords = 256u;
constexpr size_t kCopyBytes = 4096u;

struct FakeBlock {
    std::map<uint32_t, uint32_t> regs;
    uint32_t writes = 0u;
    bool failWrites = false;
};

bool readReg(void *ctx, uint32_t reg, uint32_t *value) {
    auto *b = static_cast<FakeBlock *>(ctx);
    const auto it = b->regs.find(reg);
    *value = it == b->regs.end() ? 0u : it->second;
    return true;
}

bool writeReg(void *ctx, uint32_t reg, uint32_t value) {
    auto *b = static_cast<FakeBlock *>(ctx);
    if (b->failWrites) return false;
    ++b->writes;
    b->regs[reg] = value;
    return true;
}

Port portFor(FakeBlock &b) {
    Port p;
    p.ctx = &b;
    p.read = readReg;
    p.write = writeReg;
    return p;
}

Config config(bool hardwareSubmit) {
    Config c;
    c.backend.epoch = kEpoch;
    c.backend.iommuAttested = true;
    c.backend.gpuGateOpen = true;
    c.hardwareSubmit = hardwareSubmit;
    return c;
}

uint64_t page(size_t i) { return 0x2000000ull + i * gmc11::kPageBytes; }

// Two 2-page buffers, filled with a pattern in the source, mapped and ready.
struct Buffers {
    dma_mock::PhysicalMemory mem;
    uint64_t srcPages[2]{};
    uint64_t dstPages[2]{};
    dma_backend::Handle src{}, dst{};
};

void setUp(Engine &e, Buffers &b) {
    for (size_t i = 0; i < 2; ++i) {
        b.srcPages[i] = page(i);
        b.dstPages[i] = page(2 + i);
        b.mem.addPage(b.srcPages[i]);
        b.mem.addPage(b.dstPages[i]);
        for (size_t k = 0; k < gmc11::kPageBytes; ++k)
            (*b.mem.find(b.srcPages[i]))[k] =
                static_cast<uint8_t>(0x40u + i * 13u + k * 3u);
    }
    assert(e.attach(dma_backend::Translation::IommuMapped, b.srcPages, 2,
                    b.src) == Code::Pass);
    assert(e.attach(dma_backend::Translation::IommuMapped, b.dstPages, 2,
                    b.dst) == Code::Pass);
    assert(e.map(b.src) == Code::Pass);
    assert(e.map(b.dst) == Code::Pass);
}

void testLifecycle() {
    Engine e(config(false));
    Buffers b;
    setUp(e, b);
    uint64_t fence = 0u;
    const dma_backend::CopyRequest req{b.src, 0u, b.dst, 0u, kCopyBytes};
    assert(e.copy(req, fence) == Code::WrongState);   // not started
    uint32_t ring[kRingDwords]{};
    assert(e.start(ring, 12u) == dma_engine::Code::BadRing);
    assert(e.start(ring, kRingDwords) == dma_engine::Code::Pass);
    assert(e.start(ring, kRingDwords) == dma_engine::Code::AlreadyStarted);
    assert(e.copy(req, fence) == Code::Pass && fence == 1u);
    assert(e.stop() == dma_engine::Code::Busy);       // work in flight
    assert(e.unmap(b.src) == Code::Busy);
    std::puts("  lifecycle: start once, copy only while started, stop refuses in flight");
}

void testGateClosedTouchesNothing() {
    Engine e(config(false));
    Buffers b;
    setUp(e, b);
    uint32_t ring[kRingDwords]{};
    assert(e.start(ring, kRingDwords) == dma_engine::Code::Pass);
    uint64_t fence = 0u;
    assert(e.copy({b.src, 0u, b.dst, 0u, kCopyBytes}, fence) == Code::Pass);

    FakeBlock block;
    block.regs[sdma6_mmio::kRptr] = 0u;
    const Port port = portFor(block);
    assert(e.ringDoorbell(port) == dma_engine::Code::WritesDisabled);
    assert(block.writes == 0u);
    assert(e.pollCompletion(port) == dma_engine::Code::Pass);
    assert(e.completedFence() == 0u);
    std::puts("  gate: runtime-closed engine performs no register write and completes nothing");
}

void testFailedReadIsNotDone() {
    Engine e(config(false));
    uint32_t ring[kRingDwords]{};
    assert(e.start(ring, kRingDwords) == dma_engine::Code::Pass);
    FakeBlock block;
    block.regs[sdma6_mmio::kRptr] = UINT32_MAX;   // block not answering
    assert(e.pollCompletion(portFor(block)) == dma_engine::Code::ReadFailed);
    Port none;
    assert(e.pollCompletion(none) == dma_engine::Code::NoPort);
    std::puts("  reads: all-ones and missing port are refused, never treated as done");
}

#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
void testCompiledOutWritesRefused() {
    // Even with the runtime gate open, a build without the flag cannot write.
    Engine e(config(true));
    Buffers b;
    setUp(e, b);
    uint32_t ring[kRingDwords]{};
    assert(e.start(ring, kRingDwords) == dma_engine::Code::Pass);
    uint64_t fence = 0u;
    assert(e.copy({b.src, 0u, b.dst, 0u, kCopyBytes}, fence) == Code::Pass);
    FakeBlock block;
    assert(e.ringDoorbell(portFor(block)) == dma_engine::Code::WritesDisabled);
    assert(block.writes == 0u);
    std::puts("  build: write path compiled out; doorbell refused with zero writes");
}
#else

bool sameFirstPage(Buffers &b) {
    for (size_t k = 0; k < gmc11::kPageBytes; ++k)
        if ((*b.mem.find(b.srcPages[0]))[k] != (*b.mem.find(b.dstPages[0]))[k])
            return false;
    return true;
}

// Runs the simulated SDMA block up to the write pointer the engine last wrote.
// The device keeps its own monotonic read position and publishes it in RPTR.
struct Device {
    uint64_t from = 0u;
    size_t copies = 0u;
};

dma_mock::RunResult runDevice(FakeBlock &block, const uint32_t *ring,
                              Device &dev, Buffers &b,
                              const dma_backend::Backend &backend) {
    const uint64_t mask = kRingDwords - 1u;
    const uint64_t wptrDwords = (block.regs[sdma6_mmio::kWptr] >> 2u) & mask;
    const uint64_t to = dev.from + ((wptrDwords - (dev.from & mask)) & mask);
    const dma_mock::RunResult r = dma_mock::executeRange(
        ring, kRingDwords, dev.from, to, backend.table(), backend.aperture(),
        b.mem, dev.copies);
    block.regs[sdma6_mmio::kRptr] =
        static_cast<uint32_t>((dev.from & mask) << 2u);
    return r;
}

void testDeviceRunWrapsAndRetires() {
    Engine e(config(true));
    Buffers b;
    setUp(e, b);
    uint32_t ring[kRingDwords]{};
    assert(e.start(ring, kRingDwords) == dma_engine::Code::Pass);

    FakeBlock block;
    const Port port = portFor(block);
    Device dev;
    uint64_t lastFence = 0u;
    size_t issued = 0u;
    // 256 dwords hold 36 copies at most. 120 copies force several wraps,
    // and 256 is not a multiple of 7, so padding is exercised as well.
    while (issued < 120u) {
        uint64_t fence = 0u;
        const Code c = e.copy({b.src, 0u, b.dst, 0u, kCopyBytes}, fence);
        if (c == Code::RingRejected) {
            assert(e.ringDoorbell(port) == dma_engine::Code::Pass);
            assert(runDevice(block, ring, dev, b, e.backend()) !=
                   dma_mock::RunResult::Fault);
            assert(e.pollCompletion(port) == dma_engine::Code::Pass);
            continue;
        }
        assert(c == Code::Pass && fence == lastFence + 1u);
        lastFence = fence;
        ++issued;
    }
    assert(e.ringDoorbell(port) == dma_engine::Code::Pass);
    assert(runDevice(block, ring, dev, b, e.backend()) !=
           dma_mock::RunResult::Fault);
    assert(e.pollCompletion(port) == dma_engine::Code::Pass);

    assert(dev.copies == issued);
    assert(dev.from > 3u * kRingDwords);            // wrapped several times
    assert(e.completedFence() == lastFence);
    assert(sameFirstPage(b));
    assert(e.unmap(b.src) == Code::Pass);            // nothing in flight now
    assert(e.unmap(b.dst) == Code::Pass);
    std::printf("  device: %zu copies, %llu ring dwords, fences 1..%llu completed\n",
                issued, static_cast<unsigned long long>(dev.from),
                static_cast<unsigned long long>(lastFence));
}

void testRetireMismatchRefused() {
    Engine e(config(true));
    Buffers b;
    setUp(e, b);
    uint32_t ring[kRingDwords]{};
    assert(e.start(ring, kRingDwords) == dma_engine::Code::Pass);
    uint64_t fence = 0u;
    assert(e.copy({b.src, 0u, b.dst, 0u, kCopyBytes}, fence) == Code::Pass);
    FakeBlock block;
    block.regs[sdma6_mmio::kRptr] = 14u << 2u;   // two items beyond the one published
    assert(e.pollCompletion(portFor(block)) == dma_engine::Code::RetireMismatch);
    assert(e.completedFence() == 0u);
    std::puts("  retire: a read pointer past the published work is refused");
}

void testWriteFailureReported() {
    Engine e(config(true));
    uint32_t ring[kRingDwords]{};
    assert(e.start(ring, kRingDwords) == dma_engine::Code::Pass);
    FakeBlock block;
    block.failWrites = true;
    assert(e.ringDoorbell(portFor(block)) == dma_engine::Code::PortFailed);
    assert(block.writes == 0u);
    std::puts("  write: a failed port write is reported, not ignored");
}
#endif

} // namespace

int main() {
    std::puts("Navi33 DMA engine host tests");
    testLifecycle();
    testGateClosedTouchesNothing();
    testFailedReadIsNotDone();
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
    testCompiledOutWritesRefused();
    std::puts("Navi33 DMA engine (write path compiled out): PASS (host-only)");
#else
    testDeviceRunWrapsAndRetires();
    testRetireMismatchRefused();
    testWriteFailureReported();
    std::puts("Navi33 DMA engine (unsafe MMIO build, simulated block): PASS (host-only)");
#endif
    return 0;
}
