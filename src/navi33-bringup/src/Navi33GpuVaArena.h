// Fixed-capacity GPU VA reservation ledger (CPU-only).
// Not a macOS memory allocator, DMA mapping, VRAM allocator or Kext service.
// Callers must serialize; reserve() never publishes hardware VM mappings.
#pragma once
#include "Navi33Gmc11Gart.h"

namespace navi33 {
namespace va {

constexpr size_t kMaxReservations = 64u;

enum class Code : uint8_t {
    Pass = 0, InvalidRange, NoSlots, NoSpace, NotReserved
};

struct Reservation {
    uint64_t address=0, size=0;
    bool active=false;
};

struct Result {
    Code code=Code::InvalidRange;
    uint64_t address=0;
};

class Arena {
public:
    bool reset(uint64_t base, uint64_t bytes) {
        for (auto &r: reservations_) r = Reservation{};
        base_=0;length_=0;
        if (!base || !bytes || (base & (gmc11::kPageBytes-1u)) ||
            (bytes & (gmc11::kPageBytes-1u)) ||
            base >= gmc11::kMaxConservativeDmaAddress ||
            bytes > gmc11::kMaxConservativeDmaAddress - base) return false;
        base_=base;length_=bytes;return true;
    }
    Result reserve(uint64_t bytes,uint64_t alignment=gmc11::kPageBytes) {
        if(!base_ || !bytes || (bytes & (gmc11::kPageBytes-1u)) ||
           alignment < gmc11::kPageBytes ||
           (alignment & (alignment-1u)) != 0u ||
           bytes > length_) return {Code::InvalidRange,0u};
        size_t freeSlot=kMaxReservations;
        for(size_t i=0;i<kMaxReservations;++i)
            if(!reservations_[i].active){freeSlot=i;break;}
        if(freeSlot==kMaxReservations) return {Code::NoSlots,0u};

        uint64_t cursor=base_;
        const uint64_t end=base_+length_;
        // First-fit. Each time a live overlapping range is found, advance
        // beyond it; don't rely on insertion order. <=64 descriptors.
        for(size_t iteration=0;iteration<=kMaxReservations;++iteration) {
            if(cursor > UINT64_MAX - (alignment-1u)) return {Code::NoSpace,0u};
            const uint64_t aligned=(cursor+alignment-1u)&~(alignment-1u);
            if(aligned > end || bytes > end-aligned)
                return {Code::NoSpace,0u};
            bool conflict=false;
            uint64_t next=aligned;
            for(const auto &r: reservations_) {
                if(!r.active) continue;
                if(aligned < r.address+r.size &&
                   r.address < aligned+bytes) {
                    conflict=true;
                    if(r.address+r.size>next) next=r.address+r.size;
                }
            }
            if(!conflict) {
                reservations_[freeSlot]={aligned,bytes,true};
                return {Code::Pass,aligned};
            }
            if(next<=cursor) return {Code::NoSpace,0u};
            cursor=next;
        }
        return {Code::NoSpace,0u};
    }
    Code release(uint64_t address) {
        for(auto &r: reservations_)
            if(r.active && r.address==address) {
                r=Reservation{};return Code::Pass;
            }
        return Code::NotReserved;
    }
    bool contains(uint64_t address,uint64_t bytes) const {
        if(!bytes || address>UINT64_MAX-bytes)return false;
        for(const auto &r: reservations_)
            if(r.active && address>=r.address &&
               address-r.address<r.size &&
               bytes<=r.size-(address-r.address)) return true;
        return false;
    }
    size_t count() const {
        size_t result=0;
        for(const auto &r: reservations_) if(r.active)++result;
        return result;
    }
private:
    uint64_t base_=0,length_=0;
    Reservation reservations_[kMaxReservations]{};
};

} // namespace va
} // namespace navi33
