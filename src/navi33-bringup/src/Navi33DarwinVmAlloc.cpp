// SPDX-License-Identifier: MIT
// VMID1 gradient on the VRAM heap (G39), IOKit side (runner: Navi33Gfx11VmAlloc.h).
// Registers: G38's allowlist only. VRAM: the CPU-visible BAR0 is mapped, but every
// CPU read or write must lie inside one live, unpinned block of the provider's
// heap, so the console, the bring-up windows (the PSP ring) and the firmware top
// stay untouchable. BAR0 offset == VRAM offset is proven as for the PSP stages.
// The GPU writes the console through VMID 1; the CPU only reads it back, through
// a read-only map.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Discovery.h"
#include "Navi33Gfx11VmAlloc.h"
#include "Navi33Psp13Loader.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_psp {
namespace {
namespace pl=psp13load;
namespace vg=vm_gradient;
namespace va=vm_alloc;
constexpr uint64_t kProofBytes=16u*1024u*1024u;
// The VM's host console (vm/prepare-card.sh): 1024x768x32, 4096-byte lines.
constexpr uint64_t kVmConsoleBytes=1024u*768u*4u;
struct Io { IOMemoryMap *regs=nullptr, *bar=nullptr, *screen=nullptr; vg::Regs r{}; const vram_heap::Heap *heap=nullptr; };
bool read32(void *ctx,uint64_t off,uint32_t *out) {
    IOMemoryMap *map=static_cast<Io *>(ctx)->regs;
    if(!map || !out || (off&3u) || off>map->getLength() || map->getLength()-off<4u)return false;
    *out=*reinterpret_cast<const volatile uint32_t *>(
        static_cast<uintptr_t>(map->getVirtualAddress())+off);
    return true;
}
bool write32(void *ctx,uint64_t off,uint32_t value) {
    Io *io=static_cast<Io *>(ctx);
    if(!vg::allowed(io->r,off) || !io->regs || (off&3u) || off>io->regs->getLength() ||
       io->regs->getLength()-off<4u)return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->regs->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
// Only inside one live, unpinned heap block, and inside the mapped BAR0.
volatile uint32_t *vram(Io *io,uint64_t off,uint32_t dwords) {
    const uint64_t len=4ull*dwords;
    if(!io->bar || !io->heap || !dwords || (off&3u) || !io->heap->covers(off,len) ||
       off>io->bar->getLength() || len>io->bar->getLength()-off)return nullptr;
    return reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(io->bar->getVirtualAddress())+off);
}
bool vramWrite(void *ctx,uint64_t off,const uint32_t *src,uint32_t dwords) {
    volatile uint32_t *w=vram(static_cast<Io *>(ctx),off,dwords);
    if(!w || !src)return false;
    for(uint32_t i=0;i<dwords;++i)w[i]=src[i];
    __asm__ __volatile__("sfence; mfence" ::: "memory");
    return true;
}
bool vramRead(void *ctx,uint64_t off,uint32_t *dst,uint32_t dwords) {
    volatile uint32_t *w=vram(static_cast<Io *>(ctx),off,dwords);
    if(!w || !dst)return false;
    for(uint32_t i=0;i<dwords;++i)dst[i]=w[i];
    return true;
}
bool screenRead(void *ctx,uint32_t off,uint32_t *dst) {
    IOMemoryMap *m=static_cast<Io *>(ctx)->screen;
    if(!m || !dst || (off&3u) || off>m->getLength() || m->getLength()-off<4u)return false;
    *dst=*reinterpret_cast<const volatile uint32_t *>(static_cast<uintptr_t>(m->getVirtualAddress())+off);
    return true;
}
void sleepMs(void *,uint32_t ms) { IOSleep(ms); }
} // namespace

n33_psp_load_result executeVmAlloc(IOPCIDevice *pci,const Request &req) {
    auto out=va::blank();out.state=N33_PSPL_BAD_REQUEST;
    if(!pci || !req.attempted || !req.heap || !req.vm1 ||
       (req.mode!=N33_VM_ALLOC_PLAN && req.mode!=N33_VM_ALLOC_RUN))
        return out;
    IPDiscovery disc;
    if(!req.rom || req.romBytes<64u || req.romBytes>IPDiscovery::kMaxBlob ||
       !disc.parse(req.rom,req.romBytes)) {out.state=N33_PSPL_BAD_ROM;return out;}
    IODeviceMemory *bar5=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    IODeviceMemory *bar0=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if(!bar5 || !bar0 || bar5->getLength()<4u) {out.state=N33_PSPL_NO_BARS;return out;}
    const uint64_t bar5Bytes=static_cast<uint64_t>(bar5->getLength());
    const pl::Range bar0Range{static_cast<uint64_t>(bar0->getPhysicalAddress()),
                              static_cast<uint64_t>(bar0->getLength())};
    const pl::Range boot{req.consolePhys,req.consoleBytes};
    const pl::Range console=boot.phys ? boot :
        pl::hostConsole(boot,req.hostConsole,req.hostConsoleOffset,req.hostConsoleBytes,bar0Range);
    const pl::Registers pregs=pl::resolve(disc,bar5Bytes);
    Io io{};io.r=vg::resolve(disc,bar5Bytes);io.heap=req.heap;
    if(!pregs.valid || !io.r.valid) {out.state=N33_PSPL_BAD_ROM;return out;}
    io.regs=bar5->map();
    if(!io.regs || static_cast<uint64_t>(io.regs->getLength())<bar5Bytes) {
        if(io.regs)io.regs->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    pl::Snapshot snap{};
    if(!pl::readSnapshot(pregs,read32,&io,snap)) {
        io.regs->release();out.state=N33_PSPL_READ_FAILED;return out;
    }
    // The same proof as every stage: an active scanout at the console's BAR0
    // offset shows BAR0 offset == VRAM offset.
    const pl::Plan win=pl::planWindow(snap,console,bar0Range,true,kProofBytes);
    if(win.state!=N33_PSPL_PLAN_OK) {
        io.regs->release();out.state=win.state;out.flags|=win.flags;return out;
    }
    const va::Layout layout{win.fbBaseMc,bar0Range.bytes};
    vg::Screen screen{};
    if(console.bytes==kVmConsoleBytes)
        screen={win.consoleOffset,static_cast<uint32_t>(console.bytes),1024u,768u,1024u};
    const bool run=req.mode==N33_VM_ALLOC_RUN;
    va::Hw hw{&io,read32,nullptr,nullptr,vramRead,nullptr,sleepMs};
    out=va::execute(io.r,layout,screen,req.fw,req.fwBytes,*req.heap,*req.vm1,hw,false,req.writeGate,*req.attempted);
    out.flags|=win.flags;
    if(run && out.state==N33_PSPL_PLAN_OK) {
        if(!req.writeGate) out.state=N33_PSPL_WRITE_DISABLED;
        else if(*req.attempted) out.state=N33_PSPL_ALREADY_ATTEMPTED;
        else {
            // The whole CPU-visible VRAM; vram() admits only heap blocks.
            io.bar=bar0->map(kIOMapInhibitCache);
            if(screen.width) {
                IODeviceMemory *s=IODeviceMemory::withSubRange(bar0,win.consoleOffset,console.bytes);
                if(s) {io.screen=s->map(kIOMapInhibitCache|kIOMapReadOnly);s->release();}
            }
            if(!io.bar || static_cast<uint64_t>(io.bar->getLength())!=bar0Range.bytes || (screen.width && !io.screen)) {
                out.state=N33_PSPL_MAP_FAILED;
            } else {
                IOLog("Navi33Probe: VRAM heap gradient RUN starts, FB MC 0x%llx BAR0 %llu MiB screen %ux%u at VRAM 0x%llx\n",
                      layout.fbBaseMc,layout.visibleBytes>>20,screen.width,screen.height,screen.vram);
                hw.write=write32;hw.vramWrite=vramWrite;hw.screenRead=screenRead;
                out=va::execute(io.r,layout,screen,req.fw,req.fwBytes,*req.heap,*req.vm1,hw,true,true,*req.attempted);
                out.flags|=win.flags|N33_PSPL_FLAG_WRITE_GATE_OPEN;
            }
            if(io.screen)io.screen->release();
            if(io.bar)io.bar->release();
        }
    }
    io.regs->release();
    IOLog("Navi33Probe: VRAM heap gradient %s state %u steps %u writes %u ctx1 %x scratch %x fence %x "
          "rptr %u->%u fences %x/%x bad %u/%u fallbacks %u pixel(1,1) %08x screen %x fault %x/%x "
          "root 0x%x tables %u ms %u heap free %llu MiB\n",run ? "RUN" : "PLAN",out.state,out.steps_done,
          out.register_writes,out.vm_ctx_before,out.vm_scratch,out.vm_fence,out.grad_rptr_before,
          out.grad_rptr_after,out.grad_fence_a,out.grad_fence_b,out.grad_bad_pixels,out.grad_screen_bad,
          out.grad_fallbacks,out.grad_first_pixel,out.grad_screen,out.vm_fault_status,out.vm_fault_addr,
          out.vm_window<<12,out.vm_reserved&0xffffu,out.elapsed_ms,req.heap->freeBytes()>>20);
    return out;
}
} } // namespace navi33::darwin_psp
