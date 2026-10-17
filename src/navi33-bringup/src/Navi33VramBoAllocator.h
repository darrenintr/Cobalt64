// SPDX-License-Identifier: MIT
// Navi33 host-testable ledger for an EXTERNALLY OWNED VRAM slice.
//
// The ledger never discovers free VRAM, opens PCI, maps BAR0, programs GMC,
// writes VRAM or flashes VBIOS. It may only subdivide a slice that a future
// independent PCI/display/firmware owner has already exclusively RESERVED.
// In particular, BAR0 presence and zero-looking VRAM are NOT free-space proof.
#pragma once
#include "Navi33Gmc11VramGeometry.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace vram_bo {
constexpr size_t kMaxProtected=64u;
constexpr size_t kMaxLeases=32u;
struct Range {
    uint64_t offset=0u; // 4 KiB-aligned offset into actual VRAM, NOT PCI addr
    uint64_t bytes=0u;
};
inline bool validRange(const Range &r,uint64_t limit) {
    return r.bytes!=0u && !(r.offset&(vram_geometry::kPage-1u)) &&
           !(r.bytes&(vram_geometry::kPage-1u)) &&
           r.offset<=limit && r.bytes<=limit-r.offset;
}
inline bool intersects(const Range &a,const Range &b) {
    // Only called on already validated, non-wrapping half-open intervals.
    return a.offset<b.offset+b.bytes && b.offset<a.offset+a.bytes;
}
struct Authority {
    // Must come from actual kernel PCI/VRAM resource ownership, not N33N
    // user input. There is currently NO production Kext caller.
    bool pciDeviceExclusive=false;
    bool allDisplayClientsStopped=false;
    bool firmwareAndDisplayRangesInventoried=false;
    bool independentRecoveryAvailable=false;
};
constexpr bool authoritative(const Authority &a) {
    return a.pciDeviceExclusive && a.allDisplayClientsStopped &&
           a.firmwareAndDisplayRangesInventoried &&
           a.independentRecoveryAvailable;
}
enum class Code : uint8_t {
    NotConfigured=0, NoAuthority, InvalidGeometry, InvalidSlice,
    BadReservation, Overlap, NoSpace, LeaseLimit, InvalidToken,
    Quarantined, Pass
};
struct Token {
    uint64_t generation=0;
    uint32_t slot=0;
};
struct Allocation {
    Code code=Code::NotConfigured;
    Token token{};
    Range range{};
    // Output is arithmetic from a *pre-owned VRAM BO*, not proof the
    // GPU has mapped or accessed it.
    vram_geometry::Location address{};
};
class Arena {
public:
    Code configure(vram_geometry::Geometry geo,Authority a,Range owned,
                   const Range *protectedRanges,size_t protectedCount) {
        if(configured_ || quarantined_)return Code::Quarantined;
        if(!authoritative(a))return Code::NoAuthority;
        if(geo.realBytes==0u || geo.mcWindowBytes<geo.realBytes ||
           geo.cpuVisibleBytes==0u || geo.cpuVisibleBytes>geo.realBytes ||
           (geo.realBytes&(vram_geometry::kPage-1u)) ||
           !vram_geometry::locate(geo,0u,vram_geometry::kPage).valid)
            return Code::InvalidGeometry;
        if(!validRange(owned,geo.realBytes))
            return Code::InvalidSlice;
        if(protectedCount>kMaxProtected ||
           (protectedCount && !protectedRanges))
            return Code::BadReservation;
        for(size_t i=0;i<protectedCount;++i) {
            if(!validRange(protectedRanges[i],geo.realBytes))
                return Code::BadReservation;
            for(size_t j=0;j<i;++j)
                if(intersects(protectedRanges[i],protectedRanges[j]))
                    return Code::Overlap;
        }
        geo_=geo;
        owned_=owned;
        for(size_t i=0;i<protectedCount;++i)
            protected_[i]=protectedRanges[i];
        protectedCount_=protectedCount;
        configured_=true;
        return Code::Pass;
    }
    Allocation allocate(const void *client,uint64_t bytes,
                        bool requireCpuBar0Visible=true) {
        Allocation a{};
        if(quarantined_) {a.code=Code::Quarantined;return a;}
        if(!configured_)return a;
        if(!client || !bytes || (bytes&(vram_geometry::kPage-1u))) {
            a.code=Code::InvalidSlice;return a;
        }
        uint32_t freeSlot=static_cast<uint32_t>(kMaxLeases);
        for(uint32_t i=0u;i<kMaxLeases;++i)
            if(!leases_[i].active) {freeSlot=i;break;}
        if(freeSlot==kMaxLeases || generation_==UINT64_MAX) {
            a.code=Code::LeaseLimit;return a;
        }
        // First-fit jumps to the end of the earliest colliding range.
        // Each collision advances by >= one full page; capped by the
        // number of fixed reservations + active leases.
        uint64_t candidate=owned_.offset;
        const uint64_t ownedEnd=owned_.offset+owned_.bytes;
        bool placed=false;
        for(size_t retry=0u;retry<=kMaxProtected+kMaxLeases;++retry) {
            if(candidate>ownedEnd || bytes>ownedEnd-candidate)
                break;
            const Range proposed{candidate,bytes};
            uint64_t advance=candidate;
            for(size_t i=0u;i<protectedCount_;++i)
                if(intersects(proposed,protected_[i]) &&
                   protected_[i].offset+protected_[i].bytes>advance)
                    advance=protected_[i].offset+protected_[i].bytes;
            for(size_t i=0u;i<kMaxLeases;++i)
                if(leases_[i].active &&
                   intersects(proposed,leases_[i].range) &&
                   leases_[i].range.offset+leases_[i].range.bytes>advance)
                    advance=leases_[i].range.offset+leases_[i].range.bytes;
            if(advance==candidate) {placed=true;break;}
            candidate=advance;
        }
        if(!placed) {a.code=Code::NoSpace;return a;}
        const auto address=vram_geometry::locate(geo_,candidate,bytes);
        if(!address.valid ||
           (requireCpuBar0Visible && !address.cpuBar0Visible)) {
            a.code=Code::NoSpace;return a;
        }
        ++generation_;
        leases_[freeSlot]={client,{candidate,bytes},generation_,true};
        a.code=Code::Pass;
        a.token={generation_,freeSlot};
        a.range={candidate,bytes};
        a.address=address;
        return a;
    }
    bool holds(const void *client,Token token) const {
        if(!configured_ || quarantined_ || !client ||
           token.generation==0u || token.slot>=kMaxLeases)
            return false;
        const Entry &e=leases_[token.slot];
        return e.active && e.owner==client && e.generation==token.generation;
    }
    bool release(const void *client,Token token) {
        if(!holds(client,token))return false;
        leases_[token.slot]={};
        return true;
    }
    // Called BEFORE any attempted GPU page-table/SDMA register write.
    // Once a possibly-live GPU transaction begins, memory must stay pinned.
    void quarantine() {
        if(configured_)quarantined_=true;
    }
    bool isQuarantined() const {return quarantined_;}
    bool ready() const {return configured_ && !quarantined_;}
    size_t liveLeases() const {
        size_t n=0u;
        for(size_t i=0;i<kMaxLeases;++i)if(leases_[i].active)++n;
        return n;
    }
private:
    struct Entry {
        const void *owner=nullptr;
        Range range{};
        uint64_t generation=0u;
        bool active=false;
    };
    vram_geometry::Geometry geo_{};
    Range owned_{};
    Range protected_[kMaxProtected]{};
    size_t protectedCount_=0u;
    Entry leases_[kMaxLeases]{};
    uint64_t generation_=0u;
    bool configured_=false;
    bool quarantined_=false;
};
// Deliberately no global allocator, BAR0 writer or reset/unquarantine API.
// Each Arena must be serialized by its *actual* future PCI owner.
constexpr bool kProductionPhysicalAllocationEnabled=false;
static_assert(!kProductionPhysicalAllocationEnabled,
              "This VRAM ledger is not a real PCI VRAM BO allocator");
} } // namespace navi33::vram_bo
