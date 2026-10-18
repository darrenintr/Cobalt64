// Host-only mock of the SDMA6 copy engine for Navi33DmaBackend and
// Navi33DmaEngine tests. It executes copy packets exactly as the engine would:
// every byte is translated through the GART shadow table, then copied between
// simulated physical pages. A translation fault stops the engine BEFORE the
// packet is consumed, as a hung engine would, so the fence never completes.
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

// Executes one decoded copy. Two passes: translate everything first, so a
// fault never leaves a half-written destination.
inline bool copyBytes(const sdma6::CopyDescription &d, const uint64_t *table,
                      const gmc11::Aperture &a, PhysicalMemory &mem) {
    for (uint64_t off = 0; off < d.bytes;) {
        uint64_t srcBus = 0, srcLen = 0, dstBus = 0, dstLen = 0;
        const uint64_t remaining = d.bytes - off;
        if (!resolveChunk(table, a, mem, d.source + off, remaining,
                          srcBus, srcLen) ||
            !resolveChunk(table, a, mem, d.destination + off, remaining,
                          dstBus, dstLen))
            return false;
        off += std::min(srcLen, dstLen);
    }
    for (uint64_t off = 0; off < d.bytes;) {
        uint64_t srcBus = 0, srcLen = 0, dstBus = 0, dstLen = 0;
        const uint64_t remaining = d.bytes - off;
        resolveChunk(table, a, mem, d.source + off, remaining, srcBus, srcLen);
        resolveChunk(table, a, mem, d.destination + off, remaining,
                     dstBus, dstLen);
        const uint64_t step = std::min(srcLen, dstLen);
        const PhysicalMemory::Page *from =
            mem.find(srcBus & ~(gmc11::kPageBytes - 1u));
        PhysicalMemory::Page *to =
            mem.find(dstBus & ~(gmc11::kPageBytes - 1u));
        std::memcpy(to->data() + (dstBus & (gmc11::kPageBytes - 1u)),
                    from->data() + (srcBus & (gmc11::kPageBytes - 1u)), step);
        off += step;
    }
    return true;
}

// Processes one item of the software ring: padding or one copy packet.
inline RunResult runOne(sdma6::Ring &ring, const uint64_t *table,
                        const gmc11::Aperture &a, PhysicalMemory &mem,
                        size_t &copies) {
    const uint64_t item = ring.nextItemDwords();
    if (item == 0u) return RunResult::Idle;
    if (item != sdma6::kCopyPacketDwords) {
        // Padding up to the ring end: the engine skips it and runs on.
        if (ring.consume() != sdma6::RingCode::Pass) return RunResult::Fault;
        return runOne(ring, table, a, mem, copies);
    }
    sdma6::CopyDescription d{};
    if (ring.peek(d) != sdma6::RingCode::Pass) return RunResult::Fault;
    if (!copyBytes(d, table, a, mem)) return RunResult::Fault;
    if (ring.consume() != sdma6::RingCode::Pass) return RunResult::Fault;
    ++copies;
    return RunResult::Done;
}

// Runs until the software ring is empty or a fault stops the engine.
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

// Device-side execution from raw ring dwords. The device keeps its own read
// pointer `from` (monotonic, in dwords) and runs items up to `to`. The engine's
// software ring is not touched, as on hardware.
inline RunResult executeRange(const uint32_t *storage, size_t ringDwords,
                              uint64_t &from, uint64_t to,
                              const uint64_t *table, const gmc11::Aperture &a,
                              PhysicalMemory &mem, size_t &copies) {
    const uint64_t mask = ringDwords - 1u;
    RunResult last = RunResult::Idle;
    while (from < to) {
        const uint64_t pos = from & mask;
        if (storage[pos] == 0u) {
            const uint64_t pad = ringDwords - pos;
            if (from + pad > to) return RunResult::Fault;
            from += pad;
            continue;
        }
        // SDMA never accepts a packet that crosses the ring end; the software
        // ring must pad instead. Reject it rather than wrapping silently.
        if (pos + sdma6::kCopyPacketDwords > ringDwords) return RunResult::Fault;
        if (from + sdma6::kCopyPacketDwords > to) return RunResult::Fault;
        uint32_t words[sdma6::kCopyPacketDwords]{};
        for (size_t j = 0; j < sdma6::kCopyPacketDwords; ++j)
            words[j] = storage[(from + j) & mask];
        const sdma6::CopyDescription d =
            sdma6::inspectCopy(words, sdma6::kCopyPacketDwords);
        if (d.status != sdma6::CopyCode::Pass) return RunResult::Fault;
        if (!copyBytes(d, table, a, mem)) return RunResult::Fault;
        from += sdma6::kCopyPacketDwords;
        ++copies;
        last = RunResult::Done;
    }
    return last;
}

} // namespace dma_mock
} // namespace navi33
