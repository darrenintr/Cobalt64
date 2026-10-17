// SPDX-License-Identifier: MIT
// Navi33 VRAM buffer-object LEDGER for a future EXCLUSIVELY owned GPU.
// Bookkeeping only: no BAR0 mappings, no VRAM write, no MMIO and no PCI
// device ownership acquisition. NEVER treat an MMHUB FB window as free VRAM.
// The future driver MUST separately prove and reserve the exclusive pool.
#pragma once
#include "Navi33NativeABI.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace vram_bo {
constexpr uint64_t kPage=4096ull;
constexpr uint64_t kMaxAddress=1ull<<48u;
constexpr size_t kMaxObjects=64u;
constexpr size_t kMaxProtected=32u;
enum class Code : uint8_t {
    Pass=0, NotConfigured, InvalidWindow, MissingOwnedPool,
    InvalidPool, InvalidProtectedRange, TooManyProtected, NoSpace,
    NoObjectSlots, BadRequest, BadTicket, Active, GpuMayAccess,
    ExhaustedEpoch
};
enum class Kind : uint8_t { GartRoot=1, Ring=2, Scratch=3 };
struct Span {uint64_t start=0u,bytes=0u;}; // Offset into VRAM, not MC addr
inline bool validSpan(Span s,uint64_t limit) {
    return s.bytes && !(s.start&(kPage-1ull)) &&
        !(s.bytes&(kPage-1ull)) &&
        s.start<limit && s.bytes<=limit-s.start;
}
inline bool overlaps(Span a,Span b) {
    return a.start<b.start+b.bytes && b.start<a.start+a.bytes;
}
inline uint64_t alignUp(uint64_t x,uint64_t a) {
    if(!a || x>UINT64_MAX-(a-1ull))return UINT64_MAX;
    return (x+a-1ull)&~(a-1ull);
}
struct Ticket {
    uint32_t slot=UINT32_MAX;
    uint64_t generation=0u;
};
struct Addresses {
    Code code=Code::NotConfigured;
    uint64_t locationMc=0u; // MMHUB FB_LOCATION_BASE + VRAM offset
    uint64_t pteMcOffset=0u; // MMHUB FB_OFFSET + VRAM offset
    uint64_t candidateBar0ByteOffset=0u;
    bool cpuVisibleCandidate=false;
};
class Ledger {
public:
    Code configure(const n33_vram_geometry &geometry,
                   Span externallyOwnedPool,
                   const Span *protectedSpans,size_t protectedCount) {
        if(configured_ || activeCount_ || poisoned_)return Code::Active;
        if(geometry.abi_version!=N33_ABI_VERSION ||
           geometry.size!=sizeof(geometry) ||
           geometry.state!=N33_VRAM_OBSERVED ||
           !(geometry.flags&N33_VRAM_FLAG_MMIO_STABLE) ||
           geometry.reserved ||
           !geometry.fb_window_bytes ||
           (geometry.fb_window_bytes&(kPage-1ull)) ||
           geometry.fb_window_bytes>64ull<<30u ||
           geometry.fb_base_bytes>=kMaxAddress ||
           geometry.mc_fb_offset_bytes>=kMaxAddress ||
           geometry.fb_window_bytes>kMaxAddress-geometry.fb_base_bytes ||
           geometry.fb_window_bytes>kMaxAddress-geometry.mc_fb_offset_bytes ||
           geometry.fb_top_inclusive_bytes!=
               geometry.fb_base_bytes+geometry.fb_window_bytes-1ull)
            return Code::InvalidWindow;
        if(!externallyOwnedPool.bytes)return Code::MissingOwnedPool;
        if(!validSpan(externallyOwnedPool,geometry.fb_window_bytes))
            return Code::InvalidPool;
        if(protectedCount>kMaxProtected)return Code::TooManyProtected;
        if(protectedCount && !protectedSpans)
            return Code::InvalidProtectedRange;
        for(size_t i=0;i<protectedCount;++i)
            if(!validSpan(protectedSpans[i],geometry.fb_window_bytes))
                return Code::InvalidProtectedRange;
        geometry_=geometry;pool_=externallyOwnedPool;
        protectedCount_=protectedCount;
        for(size_t i=0;i<protectedCount;++i)
            protected_[i]=protectedSpans[i];
        configured_=true;
        // No VRAM address has been allocated by software or hardware.
        return Code::Pass;
    }
    struct Allocation {Code code=Code::NotConfigured;Ticket ticket{};Span span{};};
    Allocation allocate(Kind type,uint64_t bytes,uint64_t alignment=4096u) {
        Allocation r{};
        if(!configured_)return r;
        r.code=Code::BadRequest;
        if(type!=Kind::GartRoot && type!=Kind::Ring &&
           type!=Kind::Scratch)return r;
        if(!bytes || (bytes&(kPage-1ull)) || alignment<kPage ||
           (alignment&(alignment-1ull)) || alignment>2ull*1024ull*1024ull ||
           bytes>pool_.bytes)return r;
        size_t slot=kMaxObjects;
        for(size_t i=0;i<kMaxObjects;++i)
            if(!objects_[i].used){slot=i;break;}
        if(slot==kMaxObjects){r.code=Code::NoObjectSlots;return r;}
        if(nextEpoch_==UINT64_MAX){r.code=Code::ExhaustedEpoch;return r;}
        uint64_t candidate=alignUp(pool_.start,alignment);
        const uint64_t end=pool_.start+pool_.bytes;
        for(size_t passes=0;passes<=kMaxObjects+kMaxProtected;++passes) {
            if(candidate>end || bytes>end-candidate) {
                r.code=Code::NoSpace;return r;
            }
            bool conflict=false;
            uint64_t right=candidate;
            const Span want{candidate,bytes};
            for(size_t i=0;i<protectedCount_;++i)
                if(overlaps(want,protected_[i])) {
                    conflict=true;
                    const uint64_t bound=protected_[i].start+
                                         protected_[i].bytes;
                    if(bound>right)right=bound;
                }
            for(size_t i=0;i<kMaxObjects;++i)
                if(objects_[i].used && overlaps(want,objects_[i].span)) {
                    conflict=true;
                    const uint64_t bound=objects_[i].span.start+
                                         objects_[i].span.bytes;
                    if(bound>right)right=bound;
                }
            if(conflict) {
                candidate=alignUp(right,alignment);
                continue;
            }
            const uint64_t epoch=++nextEpoch_;
            objects_[slot]={true,false,false,type,{candidate,bytes},epoch};
            ++activeCount_;
            r.code=Code::Pass;
            r.ticket={static_cast<uint32_t>(slot),epoch};
            r.span={candidate,bytes};
            return r;
        }
        r.code=Code::NoSpace;return r;
    }
    bool holds(Ticket t) const {
        return configured_ && t.slot<kMaxObjects && t.generation &&
               objects_[t.slot].used &&
               objects_[t.slot].generation==t.generation;
    }
    bool pin(Ticket t) {
        if(!holds(t) || objects_[t.slot].pinned)return false;
        objects_[t.slot].pinned=true;return true;
    }
    bool unpin(Ticket t) {
        if(!holds(t) || !objects_[t.slot].pinned ||
           objects_[t.slot].gpuTouched)return false;
        objects_[t.slot].pinned=false;return true;
    }
    // Mark BEFORE any device access that might see the pages; afterward
    // release/unpin are forbidden without independently proven GPU reset.
    bool markGpuMayAccess(Ticket t) {
        if(!holds(t) || !objects_[t.slot].pinned)return false;
        objects_[t.slot].gpuTouched=true;poisoned_=true;return true;
    }
    bool release(Ticket t) {
        if(!holds(t) || objects_[t.slot].pinned ||
           objects_[t.slot].gpuTouched)return false;
        objects_[t.slot]=Object{};
        --activeCount_;return true;
    }
    Code address(Ticket t,Addresses *out) const {
        if(!out)return Code::BadRequest;
        *out=Addresses{};
        if(!holds(t))return Code::BadTicket;
        const auto span=objects_[t.slot].span;
        out->locationMc=geometry_.fb_base_bytes+span.start;
        out->pteMcOffset=geometry_.mc_fb_offset_bytes+span.start;
        // BAR0 is a *candidate* for CPU visibility, NOT an owned mapping.
        out->candidateBar0ByteOffset=span.start;
        out->cpuVisibleCandidate=
            span.start<geometry_.assigned_bar0_bytes &&
            span.bytes<=geometry_.assigned_bar0_bytes-span.start;
        out->code=Code::Pass;
        return Code::Pass;
    }
    bool mayReconfigure() const {
        return !configured_ && !activeCount_ && !poisoned_;
    }
    bool memoryMustRemainPinned() const {return poisoned_;}
    size_t activeCount() const {return activeCount_;}
private:
    struct Object {
        bool used=false,pinned=false,gpuTouched=false;
        Kind kind=Kind::Scratch;
        Span span{};
        uint64_t generation=0u;
    };
    n33_vram_geometry geometry_{};
    Span pool_{};
    Span protected_[kMaxProtected]{};
    Object objects_[kMaxObjects]{};
    size_t protectedCount_=0u,activeCount_=0u;
    uint64_t nextEpoch_=0u;
    bool configured_=false,poisoned_=false;
};
// These addresses are architectural candidates ONLY. In particular,
// neither address() nor pin() controls real macOS IOPCIDevice/BAR0/VRAM.
} } // namespace navi33::vram_bo
