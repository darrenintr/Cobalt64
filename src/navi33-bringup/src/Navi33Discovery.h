// Bounded, side-effect-free AMD IP Discovery parser.
// Format reference: Linux drivers/gpu/drm/amd/include/discovery.h (MIT).
// Consumes caller-provided ROM/TMR bytes; NEVER reads or writes hardware.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace navi33 {

namespace ipid {
constexpr uint16_t MP1 = 1u, GC = 11u, MMHUB = 34u, OSSSYS = 40u;
constexpr uint16_t HDP = 41u, SDMA0 = 42u, SDMA1 = 43u, NBIF = 108u;
constexpr uint16_t MP0 = 255u, DMU = 271u;
}

struct IPEntry {
    static constexpr size_t kMaxBases = 16;
    uint16_t hwId = 0;
    uint8_t instance = 0;
    uint8_t major = 0, minor = 0, revision = 0;
    uint8_t subRevision = 0, variant = 0;
    uint8_t numBases = 0;
    uint64_t bases[kMaxBases]{}; // Raw dword bases; never interpreted as MMIO.
};

class IPDiscovery {
public:
    static constexpr size_t kMaxBlob = 4u * 1024u * 1024u;
    static constexpr uint16_t kMaxIPs = 256u;

    bool parse(const uint8_t *buf, size_t len) {
        reset();
        if (!buf || len < 12u || len > kMaxBlob) return false;
        data_ = buf; size_ = len;
        for (size_t candidate = 0; candidate <= len - 12u; candidate += 4u) {
            if (u32(candidate) != 0x28211407u) continue;
            if (validate(candidate)) {
                valid_ = true;
                binaryOffset_ = candidate;
                return true;
            }
        }
        reset();
        return false;
    }

    bool valid() const { return valid_; }
    size_t binaryOffset() const { return binaryOffset_; }
    uint16_t count() const { return valid_ ? count_ : 0u; }
    bool is64BitBase() const { return valid_ && wide_; }

    bool entryAt(uint16_t index, IPEntry &out) const {
        out = IPEntry{};
        if (!valid_ || index >= count_) return false;
        size_t pos = recordsBegin_;
        for (uint16_t i = 0; i < index; ++i) {
            const size_t bytes = recordBytes(pos);
            if (!bytes) return false;
            pos += bytes;
        }
        if (!recordBytes(pos)) return false;
        out.hwId = u16(pos);
        out.instance = data_[pos + 2u];
        out.numBases = data_[pos + 3u];
        out.major = data_[pos + 4u];
        out.minor = data_[pos + 5u];
        out.revision = data_[pos + 6u];
        out.subRevision = data_[pos + 7u] & 0x0fu;
        out.variant = data_[pos + 7u] >> 4u;
        for (uint8_t i = 0; i < out.numBases; ++i) {
            const size_t p = pos + 8u + static_cast<size_t>(i) * (wide_ ? 8u : 4u);
            out.bases[i] = wide_ ? u64(p) : u32(p);
        }
        return true;
    }

    // Duplicate hardware IDs are ambiguous: fail closed rather than guessing.
    bool find(uint16_t id, uint8_t instance, IPEntry &out) const {
        bool found = false;
        IPEntry candidate{};
        for (uint16_t i = 0; i < count(); ++i) {
            if (!entryAt(i, candidate)) return false;
            if (candidate.hwId != id || candidate.instance != instance) continue;
            if (found) return false;
            out = candidate;
            found = true;
        }
        return found;
    }

    // Identifies a supplied snapshot. NEVER authorizes actual GPU init.
    // Navi33 revisions are sourced from the Linux amdgpu hardware table.
    bool isNavi33Candidate() const {
        return versionMatches(ipid::GC, 11u, 0u, 2u) &&
               versionMatches(ipid::SDMA0, 6u, 0u, 2u) &&
               versionMatches(ipid::MP0, 13u, 0u, 7u) &&
               versionMatches(ipid::MP1, 13u, 0u, 7u) &&
               versionMatches(ipid::DMU, 3u, 2u, 1u);
    }

private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t binaryOffset_ = 0, tableBegin_ = 0, tableEnd_ = 0, recordsBegin_ = 0;
    uint16_t count_ = 0;
    bool wide_ = false, valid_ = false;

    void reset() {
        data_ = nullptr; size_ = 0; binaryOffset_ = 0;
        tableBegin_ = 0; tableEnd_ = 0; recordsBegin_ = 0;
        count_ = 0; wide_ = false; valid_ = false;
    }
    bool in(size_t pos, size_t bytes, size_t end) const {
        return pos <= end && bytes <= end - pos;
    }
    uint16_t u16(size_t p) const {
        return static_cast<uint16_t>(data_[p]) |
               static_cast<uint16_t>(static_cast<uint16_t>(data_[p + 1u]) << 8u);
    }
    uint32_t u32(size_t p) const {
        return static_cast<uint32_t>(u16(p)) | (static_cast<uint32_t>(u16(p + 2u)) << 16u);
    }
    uint64_t u64(size_t p) const {
        return static_cast<uint64_t>(u32(p)) | (static_cast<uint64_t>(u32(p + 4u)) << 32u);
    }
    uint16_t checksum(size_t begin, size_t end) const {
        uint16_t sum = 0;
        for (size_t i = begin; i < end; ++i)
            sum = static_cast<uint16_t>(sum + data_[i]);
        return sum;
    }
    size_t recordBytes(size_t pos) const {
        if (!in(pos, 8u, tableEnd_)) return 0u;
        const size_t n = data_[pos + 3u];
        if (n > IPEntry::kMaxBases) return 0u;
        const size_t bytes = 8u + n * (wide_ ? 8u : 4u);
        return in(pos, bytes, tableEnd_) ? bytes : 0u;
    }
    bool validate(size_t start) {
        // v1 binary_header table_list at +12 (six slots); v2 at +16.
        const uint16_t major = u16(start + 4u);
        if (major > 2u) return false;
        const uint16_t bytes = u16(start + 10u);
        if (bytes < 20u || !in(start, bytes, size_)) return false;
        const size_t end = start + bytes;
        if (checksum(start + 10u, end) != u16(start + 8u)) return false;
        const size_t info = start + (major == 2u ? 16u : 12u);
        uint16_t tableCount = 6u; // Linux discovery.h TOTAL_TABLES for v1.
        if (major == 2u) {
            if (!in(start + 12u, 4u, end)) return false;
            const uint16_t nTables = u16(start + 12u);
            if (nTables < 1u || nTables > 16u) return false;
            tableCount = nTables;
        }
        if (!in(info, 8u * tableCount, end)) return false;
        const uint16_t relative = u16(info);
        const uint16_t expectedChecksum = u16(info + 2u);
        const uint16_t tableSize = u16(info + 4u);
        const size_t minOffset = info + 8u * tableCount - start;
        if (relative < minOffset || tableSize < 80u ||
            !in(start + relative, tableSize, end)) return false;
        tableBegin_ = start + relative;
        tableEnd_ = tableBegin_ + tableSize;
        if (u32(tableBegin_) != 0x53445049u ||
            u16(tableBegin_ + 6u) != tableSize) return false;
        if (checksum(tableBegin_, tableEnd_) != expectedChecksum) return false;
        const uint16_t version = u16(tableBegin_ + 4u);
        if (version < 1u || version > 4u) return false;
        if (u16(tableBegin_ + 12u) != 1u) return false; // Navi33: one die
        const uint16_t dieRelative = u16(tableBegin_ + 16u);
        const size_t die = start + dieRelative;
        if (!dieRelative || die < tableBegin_ + 80u ||
            !in(die, 4u, tableEnd_) || u16(die) != 0u) return false;
        count_ = u16(die + 2u);
        if (count_ == 0u || count_ > kMaxIPs) return false;
        recordsBegin_ = die + 4u;
        wide_ = version == 4u && (data_[tableBegin_ + 78u] & 1u) != 0u;
        size_t pos = recordsBegin_;
        for (uint16_t i = 0; i < count_; ++i) {
            const size_t stride = recordBytes(pos);
            if (!stride) return false;
            pos += stride;
        }
        return true;
    }
    bool versionMatches(uint16_t id, uint8_t maj, uint8_t min, uint8_t rev) const {
        IPEntry e{};
        return find(id, 0u, e) && e.numBases != 0u &&
               e.major == maj && e.minor == min && e.revision == rev;
    }
};

} // namespace navi33
