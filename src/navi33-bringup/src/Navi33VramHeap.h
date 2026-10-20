// SPDX-License-Identifier: MIT
// The kernel's VRAM allocator (Phase B2). One Heap covers all of VRAM by offset
// (0 = MC FB base); ranges that firmware, the boot console or earlier bring-up
// stages own are carved out once with reserve() and can never be freed. The
// CPU-visible part (the BAR0 aperture, 256 MiB here) is the low end, as on
// amdgpu: Visible allocations come bottom-up from it, Anywhere allocations
// top-down so they leave BAR0 space free. Free space is a sorted, coalesced
// extent list; blocks carry a generation so a stale handle never frees a newer
// block. Fixed capacity, no dynamic memory, no hardware access: callers
// serialize and do the VRAM writes themselves (only inside covers()).
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace navi33 { namespace vram_heap {
constexpr uint64_t kPage=4096u;
constexpr size_t kMaxBlocks=4096u;
// Free extents are separated by live blocks, so there are at most live + 1 of
// them: with this capacity carve() and insert() can never run out of room.
constexpr size_t kMaxExtents=kMaxBlocks+1u;
enum class Code : uint8_t { Ok=0, NotConfigured, BadArgs, NoSpace, NoSlot, BadHandle, Pinned, NotFree };
enum class Place : uint8_t { Visible=0, Anywhere };
struct Handle { uint32_t slot=0; uint32_t gen=0; };   // gen 0 is never issued
struct Alloc { Code code=Code::NotConfigured; Handle h{}; uint64_t offset=0, bytes=0; };
struct Block { uint64_t offset=0, bytes=0; uint32_t tag=0, gen=0; bool live=false, pinned=false; };
constexpr bool pow2(uint64_t v) { return v && !(v&(v-1u)); }

class Heap {
public:
    Code configure(uint64_t total,uint64_t visible) {
        // Cleared in place: a Heap temporary would not fit on a kernel stack.
        for(Block &blk:blk_)blk=Block{};
        for(Extent &e:ext_)e=Extent{};
        total_=0u;visible_=0u;nExt_=0u;gen_=0u;
        if(!total || (total%kPage) || !visible || (visible%kPage) || visible>total)return Code::BadArgs;
        total_=total;visible_=visible;ext_[0]={0u,total};nExt_=1u;
        return Code::Ok;
    }
    // Carve [offset, offset+bytes) out for good; it must be entirely free.
    Code reserve(uint64_t offset,uint64_t bytes,uint32_t tag) {
        if(!total_)return Code::NotConfigured;
        if(!bytes || (offset%kPage) || (bytes%kPage) || offset>total_ || bytes>total_-offset)return Code::BadArgs;
        const size_t i=extentHolding(offset,bytes);
        if(i==nExt_)return Code::NotFree;
        const size_t slot=freeSlot();
        if(slot==kMaxBlocks)return Code::NoSlot;
        if(!carve(i,offset,bytes))return Code::NoSlot;
        blk_[slot]={offset,bytes,tag,nextGen(),true,true};
        return Code::Ok;
    }
    Alloc alloc(uint64_t bytes,uint64_t align,Place place,uint32_t tag) {
        Alloc a{};
        if(!total_)return a;
        a.code=Code::BadArgs;
        if(!bytes || (bytes%kPage) || align<kPage || !pow2(align) || bytes>total_)return a;
        const size_t slot=freeSlot();
        if(slot==kMaxBlocks){a.code=Code::NoSlot;return a;}
        a.code=Code::NoSpace;
        size_t at=nExt_; uint64_t off=0u;
        if(place==Place::Visible) {
            for(size_t i=0;i<nExt_ && at==nExt_;++i) {
                const uint64_t lo=alignUp(ext_[i].offset,align), end=ext_[i].offset+ext_[i].bytes;
                const uint64_t top=end<visible_ ? end : visible_;
                if(lo>=ext_[i].offset && lo<top && bytes<=top-lo){at=i;off=lo;}
            }
        } else {
            for(size_t i=nExt_;i-- >0 && at==nExt_;) {
                const uint64_t end=ext_[i].offset+ext_[i].bytes;
                if(bytes>ext_[i].bytes)continue;
                const uint64_t lo=(end-bytes)&~(align-1u);
                if(lo>=ext_[i].offset){at=i;off=lo;}
            }
        }
        if(at==nExt_)return a;
        if(!carve(at,off,bytes)){a.code=Code::NoSlot;return a;}
        blk_[slot]={off,bytes,tag,nextGen(),true,false};
        a.code=Code::Ok;a.h={static_cast<uint32_t>(slot),blk_[slot].gen};a.offset=off;a.bytes=bytes;
        return a;
    }
    Code free(Handle h) {
        if(!total_)return Code::NotConfigured;
        if(!valid(h))return Code::BadHandle;
        Block &b=blk_[h.slot];
        if(b.pinned)return Code::Pinned;
        if(!insert(b.offset,b.bytes))return Code::NoSlot;   // unreachable, see kMaxExtents
        b=Block{};
        return Code::Ok;
    }
    bool lookup(Handle h,Block &out) const { if(!valid(h))return false; out=blk_[h.slot]; return true; }
    // Is [offset, offset+bytes) inside one live block (allocated or reserved)?
    bool covers(uint64_t offset,uint64_t bytes,bool allowPinned=false) const {
        if(!bytes || offset>total_ || bytes>total_-offset)return false;
        for(const Block &b:blk_)
            if(b.live && (allowPinned || !b.pinned) && offset>=b.offset && offset-b.offset<b.bytes &&
               bytes<=b.bytes-(offset-b.offset))return true;
        return false;
    }
    uint64_t total() const { return total_; }
    uint64_t visible() const { return visible_; }
    uint64_t freeBytes() const { uint64_t n=0u; for(size_t i=0;i<nExt_;++i)n+=ext_[i].bytes; return n; }
    uint64_t largestFree() const { uint64_t n=0u; for(size_t i=0;i<nExt_;++i)if(ext_[i].bytes>n)n=ext_[i].bytes; return n; }
    size_t extents() const { return nExt_; }
    size_t liveBlocks() const { size_t n=0u; for(const Block &b:blk_)if(b.live)++n; return n; }
    uint64_t usedBytes(uint32_t tag) const {
        uint64_t n=0u; for(const Block &b:blk_)if(b.live && b.tag==tag)n+=b.bytes; return n;
    }
    // Self-check for tests and debug builds: sorted, coalesced, in range, and
    // free + live blocks tile VRAM exactly.
    bool consistent() const {
        uint64_t sum=0u;
        for(size_t i=0;i<nExt_;++i) {
            const Extent &e=ext_[i];
            if(!e.bytes || (e.offset%kPage) || (e.bytes%kPage) || e.offset>total_ || e.bytes>total_-e.offset)return false;
            if(i && ext_[i-1].offset+ext_[i-1].bytes>=e.offset)return false;   // overlap or not coalesced
            sum+=e.bytes;
        }
        for(const Block &b:blk_)if(b.live)sum+=b.bytes;
        return sum==total_;
    }
private:
    struct Extent { uint64_t offset=0, bytes=0; };
    static uint64_t alignUp(uint64_t v,uint64_t a) { return v>UINT64_MAX-(a-1u) ? 0u : (v+a-1u)&~(a-1u); }
    bool valid(Handle h) const {
        return total_ && h.gen && h.slot<kMaxBlocks && blk_[h.slot].live && blk_[h.slot].gen==h.gen;
    }
    size_t freeSlot() const { for(size_t i=0;i<kMaxBlocks;++i)if(!blk_[i].live)return i; return kMaxBlocks; }
    uint32_t nextGen() { if(++gen_==0u)gen_=1u; return gen_; }
    size_t extentHolding(uint64_t off,uint64_t bytes) const {
        for(size_t i=0;i<nExt_;++i)
            if(off>=ext_[i].offset && off-ext_[i].offset<ext_[i].bytes && bytes<=ext_[i].bytes-(off-ext_[i].offset))
                return i;
        return nExt_;
    }
    // Remove [off, off+bytes) from extent i, which holds it; may split it in two.
    bool carve(size_t i,uint64_t off,uint64_t bytes) {
        const Extent e=ext_[i];
        const uint64_t left=off-e.offset, right=e.offset+e.bytes-(off+bytes);
        if(left && right) {
            if(nExt_==kMaxExtents)return false;
            for(size_t j=nExt_;j>i+1u;--j)ext_[j]=ext_[j-1u];
            ++nExt_;
            ext_[i]={e.offset,left};ext_[i+1u]={off+bytes,right};
        } else if(left) ext_[i].bytes=left;
        else if(right) ext_[i]={off+bytes,right};
        else { for(size_t j=i;j+1u<nExt_;++j)ext_[j]=ext_[j+1u]; --nExt_; }
        return true;
    }
    // Return [off, off+bytes) to the free list, merging with its neighbours.
    bool insert(uint64_t off,uint64_t bytes) {
        size_t i=0;
        while(i<nExt_ && ext_[i].offset<off)++i;
        const bool joinLeft=i>0u && ext_[i-1u].offset+ext_[i-1u].bytes==off;
        const bool joinRight=i<nExt_ && off+bytes==ext_[i].offset;
        if(joinLeft && joinRight) {
            ext_[i-1u].bytes+=bytes+ext_[i].bytes;
            for(size_t j=i;j+1u<nExt_;++j)ext_[j]=ext_[j+1u];
            --nExt_;
        } else if(joinLeft) ext_[i-1u].bytes+=bytes;
        else if(joinRight) ext_[i]={off,bytes+ext_[i].bytes};
        else {
            if(nExt_==kMaxExtents)return false;
            for(size_t j=nExt_;j>i;--j)ext_[j]=ext_[j-1u];
            ++nExt_;ext_[i]={off,bytes};
        }
        return true;
    }
    uint64_t total_=0u, visible_=0u;
    Extent ext_[kMaxExtents]{};
    size_t nExt_=0u;
    Block blk_[kMaxBlocks]{};
    uint32_t gen_=0u;
};
} }
