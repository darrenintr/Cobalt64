// Navi33DmaBackend: buffer lifecycle, address validation, ownership, fence
// synchronization and fault handling, driven through the mock SDMA engine.
// Host-only. No IOKit, MMIO or GPU is touched.
#include "dma_mock_gpu.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace navi33;
using namespace navi33::dma_backend;

namespace {

constexpr uint64_t kEpoch = 7u;
constexpr uint64_t kFirstPage = 0x1000000ull;

uint64_t page(size_t i) { return kFirstPage + i * gmc11::kPageBytes; }

Config openConfig() {
    Config c;
    c.epoch = kEpoch;
    c.iommuAttested = true;
    c.gpuGateOpen = true;
    return c;
}

// Allocates `count` simulated physical pages starting at index `first`.
uint64_t pagesFor(dma_mock::PhysicalMemory &mem, size_t first, size_t count,
                  uint64_t *out) {
    for (size_t i = 0; i < count; ++i) {
        out[i] = page(first + i);
        mem.addPage(out[i]);
    }
    return count;
}

void fill(dma_mock::PhysicalMemory &mem, const uint64_t *pages, size_t count,
          uint8_t seed) {
    for (size_t i = 0; i < count; ++i) {
        auto *p = mem.find(pages[i]);
        assert(p);
        for (size_t b = 0; b < gmc11::kPageBytes; ++b)
            (*p)[b] = static_cast<uint8_t>(seed + i * 31u + b * 7u);
    }
}

// Returns true when `n` bytes at buffer offset `offA` of A equal the bytes at
// buffer offset `offB` of B. Both buffers are given by their page lists.
bool equalRange(dma_mock::PhysicalMemory &mem, const uint64_t *pagesA,
                size_t offA, const uint64_t *pagesB, size_t offB, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const size_t a = offA + i;
        const size_t b = offB + i;
        const uint8_t x = (*mem.find(pagesA[a / gmc11::kPageBytes]))
                              [a % gmc11::kPageBytes];
        const uint8_t y = (*mem.find(pagesB[b / gmc11::kPageBytes]))
                              [b % gmc11::kPageBytes];
        if (x != y) return false;
    }
    return true;
}

void testBadEpochRefused() {
    Config none; // epoch zero
    Backend dead(none);
    uint64_t pages[1]{page(0)};
    Handle h;
    assert(dead.attach(Translation::IommuMapped, pages, 1, h) == Code::BadEpoch);

    Backend b(openConfig());
    dma_mock::PhysicalMemory mem;
    assert(pagesFor(mem, 0, 1, pages) == 1);
    assert(b.attach(Translation::IommuMapped, pages, 1, h) == Code::Pass);
    Handle stale = h;
    stale.epoch = kEpoch + 1u;
    assert(b.map(stale) == Code::BadEpoch);
    assert(b.unmap(stale) == Code::BadEpoch);
    assert(b.release(stale) == Code::BadEpoch);
    assert(b.state(stale) == State::Free);
    assert(b.state(h) == State::Host);
    std::puts("  epoch: zero epoch and stale-epoch handles refused");
}

void testTranslationGates() {
    dma_mock::PhysicalMemory mem;
    uint64_t pages[1]{};
    pagesFor(mem, 0, 1, pages);
    Handle h;

    Config c = openConfig();
    c.iommuAttested = false;
    Backend noAttest(c);
    assert(noAttest.attach(Translation::IommuMapped, pages, 1, h) ==
           Code::Unattested);
    assert(noAttest.attach(Translation::None, pages, 1, h) == Code::Unattested);
    assert(noAttest.attach(Translation::PhysicalUnverified, pages, 1, h) ==
           Code::Unattested);

    Backend closed(openConfig());
    assert(closed.attach(Translation::PhysicalUnverified, pages, 1, h) ==
           Code::Unattested);
    assert(closed.liveBuffers() == 0u);

    Config allow = openConfig();
    allow.allowPhysicalUnverified = true;
    Backend physical(allow);
    assert(physical.attach(Translation::PhysicalUnverified, pages, 1, h) ==
           Code::Pass);
    assert(physical.map(h) == Code::Pass);
    std::puts("  translation: IOMMU attestation and physical opt-in both required");
}

void testAddressValidation() {
    Backend b(openConfig());
    dma_mock::PhysicalMemory mem;
    uint64_t pages[kMaxBufferPages + 1]{};
    pagesFor(mem, 0, kMaxBufferPages + 1, pages);
    Handle h;

    assert(b.attach(Translation::IommuMapped, pages, 0, h) == Code::BadPageList);
    assert(b.attach(Translation::IommuMapped, nullptr, 1, h) == Code::BadPageList);
    assert(b.attach(Translation::IommuMapped, pages, kMaxBufferPages + 1, h) ==
           Code::BadPageList);

    uint64_t zero[1]{0u};
    assert(b.attach(Translation::IommuMapped, zero, 1, h) == Code::BadAddress);
    uint64_t unaligned[1]{page(0) + 8u};
    assert(b.attach(Translation::IommuMapped, unaligned, 1, h) == Code::BadAddress);
    uint64_t huge[1]{1ull << 48u};
    assert(b.attach(Translation::IommuMapped, huge, 1, h) == Code::BadAddress);

    uint64_t dup[2]{page(0), page(0)};
    assert(b.attach(Translation::IommuMapped, dup, 2, h) == Code::DuplicatePage);

    // Refused attaches must leave no slot behind.
    assert(b.liveBuffers() == 0u);
    assert(b.attach(Translation::IommuMapped, pages, 2, h) == Code::Pass);
    uint64_t clash[1]{page(1)};
    assert(b.attach(Translation::IommuMapped, clash, 1, h) == Code::PageInUse);
    assert(b.liveBuffers() == 1u);
    std::puts("  addresses: alignment, range, duplicates and aliasing refused");
}

void testGateAndSlots() {
    Config c = openConfig();
    c.gpuGateOpen = false;
    Backend b(c);
    dma_mock::PhysicalMemory mem;
    uint64_t pages[1]{};
    pagesFor(mem, 0, 1, pages);
    Handle h;
    assert(b.attach(Translation::IommuMapped, pages, 1, h) == Code::Pass);
    assert(b.map(h) == Code::GateClosed);
    assert(b.state(h) == State::Host);

    Backend full(openConfig());
    for (size_t i = 0; i < kMaxBuffers; ++i) {
        Handle x;
        uint64_t p[1]{page(10 + i)};
        mem.addPage(p[0]);
        assert(full.attach(Translation::IommuMapped, p, 1, x) == Code::Pass);
    }
    Handle extra;
    uint64_t p[1]{page(100)};
    mem.addPage(p[0]);
    assert(full.attach(Translation::IommuMapped, p, 1, extra) == Code::NoBuffer);
    std::puts("  gate: GART install refused while closed; slot table bounded");
}

void testCopyEndToEnd() {
    Backend b(openConfig());
    dma_mock::PhysicalMemory mem;
    uint64_t srcPages[4]{}, dstPages[4]{};
    pagesFor(mem, 0, 4, srcPages);
    pagesFor(mem, 4, 4, dstPages);
    fill(mem, srcPages, 4, 0x3au);
    fill(mem, dstPages, 4, 0x00u);

    Handle src, dst;
    assert(b.attach(Translation::IommuMapped, srcPages, 4, src) == Code::Pass);
    assert(b.attach(Translation::IommuMapped, dstPages, 4, dst) == Code::Pass);
    assert(b.map(src) == Code::Pass);
    assert(b.map(dst) == Code::Pass);
    assert(b.state(src) == State::Mapped && b.state(dst) == State::Mapped);
    assert(b.table()[0] != 0u); // src is the first mapping in the aperture

    uint32_t storage[64]{};
    sdma6::Ring ring;
    assert(ring.reset(storage, 64));

    const CopyRequest req{src, 0x800u, dst, 0x100u, 8192u + 256u};
    uint64_t fence = 0u;
    assert(b.submitCopy(ring, req, fence) == Code::Pass);
    assert(fence == 1u);
    assert(ring.inFlightBatches() == 1u);

    // Work in flight: buffers are busy and cannot be torn down.
    assert(b.unmap(src) == Code::Busy);
    assert(b.unmap(dst) == Code::Busy);
    assert(b.release(src) == Code::WrongState); // still mapped
    assert(ring.lastCompletedFence() == 0u);

    size_t copies = 0u;
    const gmc11::Aperture a = b.aperture();
    assert(dma_mock::runAll(ring, b.table(), a, mem, copies) ==
           dma_mock::RunResult::Done);
    assert(copies == 1u);
    b.observeCompletion(ring.lastCompletedFence());
    assert(b.completedFence() == 1u);
    assert(equalRange(mem, srcPages, 0x800u, dstPages, 0x100u, 8192u + 256u));
    assert(b.unmap(src) == Code::Pass);
    assert(b.unmap(dst) == Code::Pass);
    assert(b.release(src) == Code::Pass);
    assert(b.state(src) == State::Free);

    // The old handle is dead; the freed slot comes back with a new generation.
    assert(b.map(src) == Code::BadHandle);
    Handle again;
    assert(b.attach(Translation::IommuMapped, srcPages, 4, again) == Code::Pass);
    assert(again.slot == src.slot && again.generation != src.generation);
    std::puts("  copy: mapped SDMA6 copy moved bytes through the GART, fence completed");
}

void testSubmitValidation() {
    Backend b(openConfig());
    dma_mock::PhysicalMemory mem;
    uint64_t pages[2]{};
    pagesFor(mem, 0, 2, pages);
    Handle a, c;
    assert(b.attach(Translation::IommuMapped, pages, 2, a) == Code::Pass);
    uint64_t other[2]{};
    pagesFor(mem, 2, 2, other);
    assert(b.attach(Translation::IommuMapped, other, 2, c) == Code::Pass);
    assert(b.map(a) == Code::Pass);
    assert(b.map(c) == Code::Pass);

    uint32_t storage[64]{};
    sdma6::Ring ring;
    assert(ring.reset(storage, 64));
    uint64_t fence = 99u;

    assert(b.submitCopy(ring, {a, 0u, c, 0u, 0u}, fence) == Code::BadSize);
    assert(b.submitCopy(ring, {a, 8192u, c, 0u, 1u}, fence) == Code::BadRange);
    assert(b.submitCopy(ring, {a, 0u, c, 8000u, 1000u}, fence) == Code::BadRange);
    // Same buffer, overlapping ranges: the engine would read what it writes.
    assert(b.submitCopy(ring, {a, 0u, a, 100u, 200u}, fence) == Code::BadRange);
    assert(fence == 0u);
    assert(ring.inFlightBatches() == 0u);

    // A backend ring that was never reset must refuse, not publish.
    sdma6::Ring dead;
    assert(b.submitCopy(dead, {a, 0u, c, 0u, 64u}, fence) == Code::RingRejected);
    assert(fence == 0u);

    Backend unmapped(openConfig());
    Handle u;
    uint64_t up[1]{page(50)};
    mem.addPage(up[0]);
    assert(unmapped.attach(Translation::IommuMapped, up, 1, u) == Code::Pass);
    assert(unmapped.submitCopy(ring, {u, 0u, u, 0u, 64u}, fence) ==
           Code::WrongState);
    std::puts("  submit: size, range, overlap and ring refusals publish nothing");
}

void testFaultQuarantine() {
    Backend b(openConfig());
    dma_mock::PhysicalMemory mem;
    uint64_t srcPages[2]{}, dstPages[2]{};
    pagesFor(mem, 0, 2, srcPages);
    pagesFor(mem, 2, 2, dstPages);
    fill(mem, srcPages, 2, 0x5cu);
    Handle src, dst;
    assert(b.attach(Translation::IommuMapped, srcPages, 2, src) == Code::Pass);
    assert(b.attach(Translation::IommuMapped, dstPages, 2, dst) == Code::Pass);
    assert(b.map(src) == Code::Pass);
    assert(b.map(dst) == Code::Pass);

    uint32_t storage[64]{};
    sdma6::Ring ring;
    assert(ring.reset(storage, 64));
    uint64_t fence = 0u;
    assert(b.submitCopy(ring, {src, 0u, dst, 0u, 4096u}, fence) == Code::Pass);

    // Simulate a GART that lost the source mapping (stale TLB or PTE). The
    // engine must fault without consuming the packet or completing the fence.
    uint64_t staleTable[kTableEntries]{};
    std::memcpy(staleTable, b.table(), sizeof staleTable);
    for (size_t i = 0; i < kTableEntries; ++i) {
        const uint64_t bus = staleTable[i] & gmc11::kAddressMask;
        if (staleTable[i] != 0u && (bus == srcPages[0] || bus == srcPages[1]))
            staleTable[i] = 0u;
    }
    size_t copies = 0u;
    assert(dma_mock::runAll(ring, staleTable, b.aperture(), mem, copies) ==
           dma_mock::RunResult::Fault);
    assert(copies == 0u);
    assert(ring.inFlightBatches() == 1u);
    b.observeCompletion(ring.lastCompletedFence());
    assert(b.completedFence() == 0u);
    // Destination must be untouched by the faulted copy.
    for (size_t i = 0; i < gmc11::kPageBytes; ++i)
        assert((*mem.find(dstPages[0]))[i] == 0u);

    assert(b.quarantine(src) == Code::Pass);
    assert(b.state(src) == State::Quarantined);
    assert(b.release(src) == Code::Quarantined);
    assert(b.unmap(src) == Code::Quarantined);
    assert(b.map(src) == Code::Quarantined);
    assert(b.submitCopy(ring, {src, 0u, dst, 0u, 64u}, fence) ==
           Code::Quarantined);
    // Its pages stay reserved: a new buffer may not alias them.
    Handle alias;
    assert(b.attach(Translation::IommuMapped, srcPages, 1, alias) ==
           Code::PageInUse);
    std::puts("  faults: fault stops the engine before consumption; quarantine keeps pages reserved");
}

} // namespace

int main() {
    static_assert(!kHardwareSubmissionEnabled,
                  "DMA backend must not claim hardware submission");
    std::puts("Navi33 DMA backend host tests");
    testBadEpochRefused();
    testTranslationGates();
    testAddressValidation();
    testGateAndSlots();
    testCopyEndToEnd();
    testSubmitValidation();
    testFaultQuarantine();
    std::puts("Navi33 DMA backend -> GMC11 PTE -> SDMA6 copy (mock engine): PASS (host-only)");
    return 0;
}
