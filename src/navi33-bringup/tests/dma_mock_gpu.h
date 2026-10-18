// Host-only mock of the SDMA6 copy engine for Navi33DmaBackend tests.
// It consumes published copy packets exactly as the engine would: every byte
// is translated through the GART shadow table, then copied between simulated
// physical pages. A translation fault stops the engine BEFORE the packet is
// consumed, as a hung engine would, so the fence never completes.
// This models transactions only. It is not evidence about silicon.
#pragma once
#include "../src/Navi33DmaBackend.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace navi33 {
namespace dma_mock {

class PhysicalMemory {
public:
    using Page = std::array<uint8_t, gmc11::kPageBytes>;

    void addPage(uint64_t bus) { pages_[bus] = Page{}; }
    Page *find(uint64_t bus) {
        auto it = pages_.find(bus);
        return it == pages_.end() ? nullptr : &it->second;
    }

private:
    std::map<uint64_t, Page> pages_;
};

enum class RunResult : uint8_t { Idle = 0, Done, Fault };

// Translates one page-bounded chunk of a GPU virtual range. Fails closed on
// out-of-aperture, invalid PTE, or a bus page the simulated memory lacks.
inline bool resolveChunk(const uint64_t *table, const gmc11::Aperture &a,
                         PhysicalMemory &mem, uint64_t va, uint64_t remaining,
                         uint64_t &bus, uint64_t &length) {
    if (va < a.gpuVaBase || va - a.gpuVaBase >= a.lengthBytes) return false;
    const uint64_t index = (va - a.gpuVaBase) / gmc11::kPageBytes;
    if (gmc11::parseSystemPte(table[index]).code != gmc11::Code::Pass)
        return false;
    const uint64_t page = table[index] & gmc11::kAddressMask;
    const uint64_t offset = va & (gmc11::kPageBytes - 1u);
    if (!mem.find(page)) return false;
    length = std::min<uint64_t>(remaining, gmc11::kPageBytes - offset);
    bus = page + offset;
    return true;
}

// Processes one published packet. Two passes: translate everything first, so
// a fault never leaves a half-written destination.
inline RunResult runOne(sdma6::Ring &ring, const uint64_t *table,
                        const gmc11::Aperture &a, PhysicalMemory &mem,
                        size_t &copies) {
    sdma6::CopyDescription d{};
    const sdma6::RingCode peek = ring.peek(d);
    if (peek == sdma6::RingCode::NoPublishedPacket) return RunResult::Idle;
    if (peek != sdma6::RingCode::Pass) return RunResult::Fault;

    for (uint64_t off = 0; off < d.bytes;) {
        uint64_t srcBus = 0, srcLen = 0, dstBus = 0, dstLen = 0;
        const uint64_t remaining = d.bytes - off;
        if (!resolveChunk(table, a, mem, d.source + off, remaining,
                          srcBus, srcLen) ||
            !resolveChunk(table, a, mem, d.destination + off, remaining,
                          dstBus, dstLen))
            return RunResult::Fault;
        off += std::min(srcLen, dstLen);
    }
    for (uint64_t off = 0; off < d.bytes;) {
        uint64_t srcBus = 0, srcLen = 0, dstBus = 0, dstLen = 0;
        const uint64_t remaining = d.bytes - off;
        resolveChunk(table, a, mem, d.source + off, remaining, srcBus, srcLen);
        resolveChunk(table, a, mem, d.destination + off, remaining,
                     dstBus, dstLen);
        const uint64_t step = std::min(srcLen, dstLen);
        const PhysicalMemory::Page *from = mem.find(srcBus & ~(gmc11::kPageBytes - 1u));
        PhysicalMemory::Page *to = mem.find(dstBus & ~(gmc11::kPageBytes - 1u));
        std::memcpy(to->data() + (dstBus & (gmc11::kPageBytes - 1u)),
                    from->data() + (srcBus & (gmc11::kPageBytes - 1u)), step);
        off += step;
    }
    if (ring.consume() != sdma6::RingCode::Pass) return RunResult::Fault;
    ++copies;
    return RunResult::Done;
}

// Runs until the ring is empty or a fault stops the engine.
inline RunResult runAll(sdma6::Ring &ring, const uint64_t *table,
                        const gmc11::Aperture &a, PhysicalMemory &mem,
                        size_t &copies) {
    RunResult last = RunResult::Idle;
    for (;;) {
        const RunResult r = runOne(ring, table, a, mem, copies);
        if (r != RunResult::Done) return r == RunResult::Idle ? last : r;
        last = RunResult::Done;
    }
}

} // namespace dma_mock
} // namespace navi33
