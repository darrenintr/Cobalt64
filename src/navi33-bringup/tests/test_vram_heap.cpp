// SPDX-License-Identifier: MIT
#include "../src/Navi33Gmc11Vm.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <tuple>
#include <vector>
using namespace navi33;
namespace vh=vram_heap;
namespace gv=gmc11_vm;
constexpr uint64_t MiB=1ull<<20;
// Page-granular reference model: 0 free, 1 allocated, 2 reserved.
struct Model {
    std::vector<uint8_t> page;
    explicit Model(uint64_t total):page(total/vh::kPage,0u){}
    bool allFree(uint64_t off,uint64_t bytes) const {
        for(uint64_t p=off/vh::kPage;p<(off+bytes)/vh::kPage;++p)if(page[p])return false;
        return true;
    }
    void set(uint64_t off,uint64_t bytes,uint8_t v){for(uint64_t p=off/vh::kPage;p<(off+bytes)/vh::kPage;++p)page[p]=v;}
    uint64_t freeBytes() const {uint64_t n=0;for(auto v:page)if(!v)n+=vh::kPage;return n;}
};
// Fake VRAM for the page-table walk: qwords by VRAM offset; zeroPage clears.
struct Vram {
    std::map<uint64_t,uint64_t> q; std::vector<uint64_t> zeroed; vh::Heap *heap=nullptr; unsigned failAt=0,ops=0;
    static bool zero(void *c,uint64_t off) {
        auto &v=*static_cast<Vram *>(c);
        if(++v.ops==v.failAt)return false;
        assert(off%4096u==0 && v.heap->covers(off,4096u));
        for(uint64_t i=0;i<512;++i)v.q.erase(off+8u*i);
        v.zeroed.push_back(off);return true;
    }
    static bool put(void *c,uint64_t off,uint64_t val) {
        auto &v=*static_cast<Vram *>(c);
        if(++v.ops==v.failAt)return false;
        assert(off%8u==0 && v.heap->covers(off,8u));   // only ever inside a live table page
        v.q[off]=val;return true;
    }
    uint64_t at(uint64_t off) const { auto i=q.find(off); return i==q.end() ? 0u : i->second; }
    // The GFXHUB walk at depth 3 from the root; returns false on an invalid entry.
    bool translate(uint64_t root,uint64_t va,uint64_t &pa,uint64_t &flags) const {
        uint64_t table=root;
        for(uint32_t l=3;;--l) {
            const uint64_t e=at(table+8u*gv::index(va,l));
            if(!(e&1u))return false;
            if(l && (e&~(gv::kAddrMask|1u)))return false;     // a PDE carries VALID only
            table=e&gv::kAddrMask;
            if(!l){pa=table+(va&0xfffu);flags=e&~gv::kAddrMask;return true;}
        }
    }
    gv::Io io(){return {this,zero,put};}
};
int main() {
    // 1. RX 7600 geometry: 8176 MiB, BAR0 256 MiB; the kext's reservations.
    {   vh::Heap h;assert(h.configure(8176u*MiB,256u*MiB)==vh::Code::Ok && h.consistent());
        assert(h.reserve(0,16u*MiB,1)==vh::Code::Ok && h.reserve(240u*MiB,16u*MiB,2)==vh::Code::Ok &&
               h.reserve(8064u*MiB,112u*MiB,3)==vh::Code::Ok);
        assert(h.reserve(8u*MiB,4096,9)==vh::Code::NotFree && h.reserve(250u*MiB,4096,9)==vh::Code::NotFree);
        assert(h.extents()==2u && h.freeBytes()==(8176u-144u)*MiB && h.consistent());
        auto v=h.alloc(4096,4096,vh::Place::Visible,7);
        assert(v.code==vh::Code::Ok && v.offset==16u*MiB);           // bottom-up in BAR0
        auto a=h.alloc(64u*MiB,2u*MiB,vh::Place::Anywhere,7);
        assert(a.code==vh::Code::Ok && a.offset==8000u*MiB);         // top-down below the top reservation
        auto big=h.alloc(225u*MiB,4096,vh::Place::Visible,7);
        assert(big.code==vh::Code::NoSpace);                         // 16..240 MiB holds only 224 MiB
        auto vis=h.alloc(224u*MiB-4096u,4096,vh::Place::Visible,7);
        assert(vis.code==vh::Code::Ok && vis.offset==16u*MiB+4096u && vis.offset+vis.bytes==240u*MiB);
        assert(h.alloc(4096,4096,vh::Place::Visible,7).code==vh::Code::NoSpace);   // BAR0 full
        assert(h.alloc(4096,4096,vh::Place::Anywhere,7).code==vh::Code::Ok);       // the rest is not
        assert(!h.covers(0,4096) && h.covers(0,4096,true) && h.covers(16u*MiB,4096) && !h.covers(16u*MiB,8192+224u*MiB));
        assert(h.free(v.h)==vh::Code::Ok && h.free(v.h)==vh::Code::BadHandle && h.consistent());
        vh::Handle pinned{};for(uint32_t i=0;i<vh::kMaxBlocks;++i){vh::Block b{};if(h.lookup({i,1u},b)&&b.pinned){pinned={i,1u};break;}}
        assert(pinned.gen && h.free(pinned)==vh::Code::Pinned);
        // A stale handle whose slot was reused by a newer block frees nothing.
        auto n=h.alloc(4096,4096,vh::Place::Visible,7);
        assert(n.h.slot==v.h.slot && n.h.gen!=v.h.gen && h.free(v.h)==vh::Code::BadHandle && h.covers(n.offset,4096));
        assert(h.usedBytes(3)==112u*MiB);
    }
    // 2. Arguments.
    {   vh::Heap h;assert(h.alloc(4096,4096,vh::Place::Visible,0).code==vh::Code::NotConfigured);
        assert(h.configure(0,0)==vh::Code::BadArgs && h.configure(8192,4096*3)==vh::Code::BadArgs);
        assert(h.configure(64u*MiB,16u*MiB)==vh::Code::Ok);
        for(auto [b,al]:{std::pair<uint64_t,uint64_t>{0,4096},{100,4096},{4096,100},{4096,6144},{4096,2048},{128u*MiB,4096}})
            assert(h.alloc(b,al,vh::Place::Visible,0).code==vh::Code::BadArgs);
        assert(h.reserve(60u*MiB,8u*MiB,0)==vh::Code::BadArgs && h.reserve(1,4096,0)==vh::Code::BadArgs);
    }
    // 3. Random alloc/free against the page model: never overlapping, inside the
    //    zone asked for, aligned; freeing everything gives back one extent.
    for(int seed=1;seed<=20;++seed) {
        std::mt19937_64 rng(seed);
        const uint64_t total=64u*MiB,visible=16u*MiB;
        vh::Heap h;Model m(total);assert(h.configure(total,visible)==vh::Code::Ok);
        assert(h.reserve(0,MiB,1)==vh::Code::Ok);m.set(0,MiB,2);
        assert(h.reserve(32u*MiB,4u*MiB,1)==vh::Code::Ok);m.set(32u*MiB,4u*MiB,2);
        struct Live{vh::Handle h;uint64_t off,bytes;};std::vector<Live> live;
        for(int op=0;op<20000;++op) {
            if(live.empty() || rng()%3) {
                const uint64_t pages=1u+rng()%((rng()%8)==0 ? 2048u : 16u), bytes=pages*vh::kPage;
                const uint64_t align=vh::kPage<<(rng()%6);
                const vh::Place pl=(rng()%2) ? vh::Place::Visible : vh::Place::Anywhere;
                auto a=h.alloc(bytes,align,pl,5);
                if(a.code==vh::Code::Ok) {
                    assert(a.bytes==bytes && !(a.offset%align) && m.allFree(a.offset,bytes));
                    if(pl==vh::Place::Visible)assert(a.offset+bytes<=visible);
                    m.set(a.offset,bytes,1);live.push_back({a.h,a.offset,bytes});
                } else {
                    assert(a.code==vh::Code::NoSpace || a.code==vh::Code::NoSlot);
                    if(a.code==vh::Code::NoSlot)assert(live.size()+2u==vh::kMaxBlocks);   // + the 2 reservations
                }
            } else {
                const size_t i=rng()%live.size();
                assert(h.free(live[i].h)==vh::Code::Ok);m.set(live[i].off,live[i].bytes,0);
                live[i]=live.back();live.pop_back();
            }
            if(op%997==0)assert(h.consistent() && h.freeBytes()==m.freeBytes());
        }
        for(auto &l:live)assert(h.free(l.h)==vh::Code::Ok);
        assert(h.consistent() && h.freeBytes()==total-5u*MiB && h.extents()==2u && h.liveBlocks()==2u);
    }
    // 4. Worst-case fragmentation: every other page allocated, then freed in a
    //    scrambled order; the extent list never runs out of room.
    {   vh::Heap h;assert(h.configure(2u*vh::kMaxBlocks*vh::kPage,2u*vh::kMaxBlocks*vh::kPage)==vh::Code::Ok);
        std::vector<vh::Handle> all;
        for(size_t i=0;i<vh::kMaxBlocks;++i){auto a=h.alloc(vh::kPage,vh::kPage,vh::Place::Visible,0);assert(a.code==vh::Code::Ok);all.push_back(a.h);}
        for(size_t i=0;i<vh::kMaxBlocks;i+=2)assert(h.free(all[i])==vh::Code::Ok);
        for(size_t i=0;i<vh::kMaxBlocks/2;++i){auto a=h.alloc(vh::kPage,vh::kPage,vh::Place::Anywhere,0);assert(a.code==vh::Code::Ok);}
        assert(h.liveBlocks()==vh::kMaxBlocks && h.consistent());
        assert(h.alloc(vh::kPage,vh::kPage,vh::Place::Anywhere,0).code==vh::Code::NoSlot);
    }
    // 5. The address space: four levels from heap pages, walked as the GFXHUB does.
    {   vh::Heap h;assert(h.configure(8176u*MiB,256u*MiB)==vh::Code::Ok && h.reserve(0,16u*MiB,1)==vh::Code::Ok);
        Vram vr;vr.heap=&h;gv::Space s;
        assert(s.map(0x1000,0x2000000,4096,gv::kPteVram)==gv::Code::NotReady);
        assert(s.init(h,vr.io())==gv::Code::Ok && s.tables()==1u && s.rootVram()==16u*MiB);
        const uint64_t va=0x55d55e000000ull;
        auto bo=h.alloc(20u*4096u,4096,vh::Place::Visible,8);assert(bo.code==vh::Code::Ok);
        assert(s.map(va,bo.offset,bo.bytes,gv::kPteVram)==gv::Code::Ok && s.tables()==4u);
        // A second range in the next 2 MiB needs only one more PTB; a far one needs a new PDB1/PDB0/PTB.
        assert(s.map(va+2u*MiB,0,3u*MiB,gv::kPteVram)==gv::Code::Ok && s.tables()==6u);
        assert(s.map(0x7f0000000000ull,bo.offset,4096,gv::kPteRead|gv::kPteValid)==gv::Code::Ok && s.tables()==9u);
        for(uint64_t o=0;o<bo.bytes;o+=4096){uint64_t pa=0,f=0;assert(vr.translate(s.rootVram(),va+o+12,pa,f) && pa==bo.offset+o+12 && f==gv::kPteVram);}
        for(uint64_t o=0;o<3u*MiB;o+=4096){uint64_t pa=0,f=0;assert(vr.translate(s.rootVram(),va+2u*MiB+o,pa,f) && pa==o);}
        uint64_t pa=0,f=0;
        assert(!vr.translate(s.rootVram(),va+bo.bytes,pa,f) && !vr.translate(s.rootVram(),va-4096,pa,f));
        assert(vr.translate(s.rootVram(),0x7f0000000000ull,pa,f) && f==(gv::kPteRead|gv::kPteValid));
        // Every table page was zeroed before anything pointed at it, and is a heap block.
        assert(vr.zeroed.size()==9u && h.usedBytes(gv::kTagTables)==9u*4096u);
        assert(s.unmap(va,4096)==gv::Code::Ok && !vr.translate(s.rootVram(),va,pa,f) &&
               vr.translate(s.rootVram(),va+4096,pa,f));
        assert(s.unmap(0x100000000ull,4096)==gv::Code::NotMapped);
        for(auto [v,p,b,fl]:{std::tuple<uint64_t,uint64_t,uint64_t,uint64_t>{va+1,0,4096,gv::kPteVram},{va,1,4096,gv::kPteVram},
                {va,0,4095,gv::kPteVram},{1ull<<47,0,4096,gv::kPteVram},{(1ull<<47)-4096,0,8192,gv::kPteVram},
                {va,0,4096,0},{va,0,4096,gv::kPteVram|(1ull<<54)},{va,1ull<<48,4096,gv::kPteVram}})
            assert(s.map(v,p,b,fl)==gv::Code::BadArgs);
    }
    // 6. A failed table write leaves no table behind that the parent points at.
    for(unsigned at=1;at<=8;++at) {
        vh::Heap h;assert(h.configure(256u*MiB,256u*MiB)==vh::Code::Ok);
        Vram vr;vr.heap=&h;vr.failAt=at;gv::Space s;
        const gv::Code c0=s.init(h,vr.io());
        if(c0!=gv::Code::Ok){assert(at==1 && h.liveBlocks()==0u);continue;}
        const gv::Code c=s.map(0x55d55e000000ull,64u*MiB,4096,gv::kPteVram);
        assert(c==gv::Code::WriteFailed);
        assert(h.liveBlocks()==s.tables());      // freed pages are not counted as tables
        uint64_t pa=0,f=0;assert(!vr.translate(s.rootVram(),0x55d55e000000ull,pa,f));
    }
    puts("VRAM heap: reservations, BAR0 zone, top-down placement, 20x20000 random ops against a page model, "
         "fragmentation bound; GMC11 four-level address space walked as the hub does: passed");
}
