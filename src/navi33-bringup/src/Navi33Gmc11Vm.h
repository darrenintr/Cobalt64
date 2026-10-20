// SPDX-License-Identifier: MIT
// One GPU virtual address space on GMC 11 (Phase B2): the four-level page table
// amdgpu builds for this ASIC (gmc_v11_0.c amdgpu_vm_adjust_size(adev, 256 * 1024,
// 9, 3, 48): root PDB2, then PDB1, PDB0, PTB, 9 bits each over 4 KiB pages, a
// 48-bit VA). Every table page comes from the VRAM heap (CPU-visible, since the
// CPU writes the entries through BAR0) and is zeroed before its parent points
// at it. Entries as gmc_v11_0_get_vm_pde/pte: a directory entry is the next
// table's VRAM offset | VALID (no translate_further on GMC11), a PTE is the page's
// VRAM offset | the caller's flags. The CPU writes go through callbacks; this
// header never touches hardware. Mappings are made before a context uses them:
// changing a live mapping also needs a TLB invalidation, which is not here yet.
#pragma once
#include "Navi33VramHeap.h"
namespace navi33 { namespace gmc11_vm {
namespace vh=vram_heap;
constexpr uint64_t kPage=4096u, kPdeValid=1u;
constexpr uint64_t kVaLimit=1ull<<47;          // below the GMC hole (AMDGPU_GMC_HOLE_START)
constexpr uint64_t kAddrMask=0x0000fffffffff000ull;
// amdgpu_vm.h: VALID, SYSTEM, SNOOPED, TMZ, EXECUTABLE, READABLE, WRITEABLE, MTYPE_NV10 (48..50).
constexpr uint64_t kPteValid=1u<<0, kPteSystem=1u<<1, kPteExec=1u<<4, kPteRead=1u<<5, kPteWrite=1u<<6;
constexpr uint64_t kPteVram=kPteValid|kPteExec|kPteRead|kPteWrite;   // MTYPE NC (0)
constexpr uint64_t kPteAllowed=0x7fu|(7ull<<48);
constexpr uint32_t kDepth=3u;                   // PAGE_TABLE_DEPTH for context 1..15
constexpr size_t kMaxTables=128u;
constexpr uint32_t kTagTables=0x50544231u;      // heap tag of table pages
enum class Code : uint8_t { Ok=0, NotReady, BadArgs, NoMemory, NoSlot, WriteFailed, NotMapped };
struct Io {
    void *ctx=nullptr;
    bool (*zeroPage)(void *,uint64_t vramOffset)=nullptr;
    bool (*putEntry)(void *,uint64_t vramOffset,uint64_t value)=nullptr;
};
constexpr uint32_t index(uint64_t va,uint32_t level) { return static_cast<uint32_t>(va>>(12u+9u*level))&511u; }

class Space {
public:
    // Allocates and zeroes the root (PDB2).
    Code init(vh::Heap &heap,const Io &io) {
        for(Table &t:t_)t=Table{};    // in place, not a stack temporary
        n_=0u;root_=Table{};heap_=nullptr;io_=Io{};
        if(!io.zeroPage || !io.putEntry)return Code::BadArgs;
        heap_=&heap;io_=io;
        const vh::Alloc a=heap.alloc(kPage,kPage,vh::Place::Visible,kTagTables);
        if(a.code!=vh::Code::Ok)return Code::NoMemory;
        root_={3u,0u,a.offset,a.h};
        if(!io.zeroPage(io.ctx,a.offset)){heap.free(a.h);root_={};return Code::WriteFailed;}
        return Code::Ok;
    }
    bool ready() const { return heap_ && root_.h.gen; }   // offset 0 is a valid table page
    uint64_t rootVram() const { return root_.vram; }
    size_t tables() const { return ready() ? 1u+n_ : 0u; }
    // Map [va, va+bytes) to VRAM [pa, pa+bytes), 4 KiB pages.
    Code map(uint64_t va,uint64_t pa,uint64_t bytes,uint64_t flags) {
        if(!ready())return Code::NotReady;
        if(!bytes || (va%kPage) || (pa%kPage) || (bytes%kPage) || va>=kVaLimit || bytes>kVaLimit-va ||
           (pa&~kAddrMask) || pa>kAddrMask-bytes+1u || !(flags&kPteValid) || (flags&~kPteAllowed))
            return Code::BadArgs;
        for(uint64_t o=0;o<bytes;o+=kPage) {
            uint64_t ptb=0;
            const Code c=walk(va+o,true,ptb);
            if(c!=Code::Ok)return c;
            if(!io_.putEntry(io_.ctx,ptb+8u*index(va+o,0),(pa+o)|flags))return Code::WriteFailed;
        }
        return Code::Ok;
    }
    Code unmap(uint64_t va,uint64_t bytes) {
        if(!ready())return Code::NotReady;
        if(!bytes || (va%kPage) || (bytes%kPage) || va>=kVaLimit || bytes>kVaLimit-va)return Code::BadArgs;
        for(uint64_t o=0;o<bytes;o+=kPage) {
            uint64_t ptb=0;
            if(walk(va+o,false,ptb)!=Code::Ok)return Code::NotMapped;
            if(!io_.putEntry(io_.ctx,ptb+8u*index(va+o,0),0u))return Code::WriteFailed;
        }
        return Code::Ok;
    }
    // Give every table page back to the heap. Only once no context can walk them
    // any more (the context is disabled and its TLB flushed).
    void destroy() {
        if(!heap_)return;
        for(size_t i=0;i<n_;++i)heap_->free(t_[i].h);
        if(root_.h.gen)heap_->free(root_.h);
        for(Table &t:t_)t=Table{};
        n_=0u;root_=Table{};heap_=nullptr;
    }
    // The VRAM offset of the PTB covering va, if it exists (for tests and diagnostics).
    bool ptbOf(uint64_t va,uint64_t &ptb) { return ready() && walk(va,false,ptb)==Code::Ok; }
private:
    struct Table { uint32_t level=0; uint64_t prefix=0, vram=0; vh::Handle h{}; };
    // Prefix of the table at `level` covering va: the VA bits above that table's span.
    static uint64_t prefixOf(uint64_t va,uint32_t level) { return va>>(12u+9u*(level+1u)); }
    Table *find(uint32_t level,uint64_t prefix) {
        for(size_t i=0;i<n_;++i)if(t_[i].level==level && t_[i].prefix==prefix)return &t_[i];
        return nullptr;
    }
    // Find (or create) PDB1, PDB0 and the PTB on va's path; returns the PTB.
    Code walk(uint64_t va,bool create,uint64_t &ptb) {
        uint64_t parent=root_.vram;
        for(uint32_t level=kDepth;level-- >0;) {
            Table *t=find(level,prefixOf(va,level));
            if(!t) {
                if(!create)return Code::NotMapped;
                if(n_==kMaxTables)return Code::NoSlot;
                const vh::Alloc a=heap_->alloc(kPage,kPage,vh::Place::Visible,kTagTables);
                if(a.code!=vh::Code::Ok)return Code::NoMemory;
                if(!io_.zeroPage(io_.ctx,a.offset)) {heap_->free(a.h);return Code::WriteFailed;}
                // The page is zero before the parent points at it.
                if(!io_.putEntry(io_.ctx,parent+8u*index(va,level+1u),a.offset|kPdeValid)) {
                    heap_->free(a.h);return Code::WriteFailed;
                }
                t_[n_]={level,prefixOf(va,level),a.offset,a.h};
                t=&t_[n_++];
            }
            parent=t->vram;
        }
        ptb=parent;return Code::Ok;
    }
    vh::Heap *heap_=nullptr;
    Io io_{};
    Table root_{};
    Table t_[kMaxTables]{};
    size_t n_=0u;
};
} }
