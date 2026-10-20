// SPDX-License-Identifier: MIT
// VMID1 gradient stage (G38), IOKit side (runner: Navi33Gfx11VmGradient.h). Writes
// only the runner's allowlist (G36's GFXHUB/queue registers plus SH_MEM_CONFIG and
// SH_MEM_BASES) and a 1 MiB sub-range of BAR0 at VRAM 252 MiB. BAR0 offset == VRAM
// offset is proven as for the PSP stages; the page tables hold VRAM offsets from
// that proof. The GPU writes the console framebuffer through VMID 1; the CPU only
// reads it back, through a read-only map.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Discovery.h"
#include "Navi33Gfx11VmGradient.h"
#include "Navi33Psp13Loader.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_psp {
namespace {
namespace pl=psp13load;
namespace vg=vm_gradient;
constexpr uint64_t kProofBytes=16u*1024u*1024u, kWindowFromProof=12u*1024u*1024u;   // PSP 240..248, SDMA 248, MEC 250, VMID1 251
// The VM's host console (vm/prepare-card.sh): 1024x768x32, 4096-byte lines.
constexpr uint64_t kVmConsoleBytes=1024u*768u*4u;
struct Io { IOMemoryMap *regs=nullptr, *win=nullptr, *screen=nullptr; vg::Regs r{}; };
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
volatile uint32_t *window(Io *io,uint32_t off,uint32_t dwords) {
    const uint64_t len=4ull*dwords;
    if(!io->win || io->win->getLength()!=vg::kWindowBytes || (off&3u) ||
       off>vg::kWindowBytes || len>vg::kWindowBytes-off)return nullptr;
    return reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->win->getVirtualAddress())+off);
}
bool vramWrite(void *ctx,uint32_t off,const uint32_t *src,uint32_t dwords) {
    volatile uint32_t *w=window(static_cast<Io *>(ctx),off,dwords);
    if(!w || !src)return false;
    for(uint32_t i=0;i<dwords;++i)w[i]=src[i];
    __asm__ __volatile__("sfence; mfence" ::: "memory");
    return true;
}
bool vramRead(void *ctx,uint32_t off,uint32_t *dst,uint32_t dwords) {
    volatile uint32_t *w=window(static_cast<Io *>(ctx),off,dwords);
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

n33_psp_load_result executeVmGradient(IOPCIDevice *pci,const Request &req) {
    auto out=vg::blank();out.state=N33_PSPL_BAD_REQUEST;
    if(!pci || !req.attempted || (req.mode!=N33_VM_GRADIENT_PLAN && req.mode!=N33_VM_GRADIENT_RUN))
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
    Io io{};io.r=vg::resolve(disc,bar5Bytes);
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
    const pl::Plan win=pl::planWindow(snap,console,bar0Range,true,kProofBytes);
    if(win.state!=N33_PSPL_PLAN_OK) {
        io.regs->release();out.state=win.state;out.flags|=win.flags;return out;
    }
    const vg::Layout layout{win.fwPriMc+kWindowFromProof,win.fwPriOffset+kWindowFromProof};
    // The console (its BAR0 offset is its VRAM offset, as the plan proved) is mapped
    // into VMID 1 only for the VM's known geometry, as G35 drew it.
    vg::Screen screen{};
    if(console.bytes==kVmConsoleBytes)
        screen={win.consoleOffset,static_cast<uint32_t>(console.bytes),1024u,768u,1024u};
    const bool run=req.mode==N33_VM_GRADIENT_RUN;
    // The window is mapped only for a run that passed the plan and the gates.
    vg::Hw hw{&io,read32,nullptr,nullptr,vramRead,nullptr,sleepMs};
    out=vg::execute(io.r,layout,screen,req.fw,req.fwBytes,hw,false,req.writeGate,*req.attempted);
    out.flags|=win.flags;
    if(run && out.state==N33_PSPL_PLAN_OK) {
        if(!req.writeGate) out.state=N33_PSPL_WRITE_DISABLED;
        else if(*req.attempted) out.state=N33_PSPL_ALREADY_ATTEMPTED;
        else {
            IODeviceMemory *sub=IODeviceMemory::withSubRange(bar0,win.fwPriOffset+kWindowFromProof,
                                                             vg::kWindowBytes);
            if(sub) {io.win=sub->map(kIOMapInhibitCache);sub->release();}
            if(screen.width) {
                IODeviceMemory *s=IODeviceMemory::withSubRange(bar0,win.consoleOffset,console.bytes);
                if(s) {io.screen=s->map(kIOMapInhibitCache|kIOMapReadOnly);s->release();}
            }
            if(!io.win || io.win->getLength()!=vg::kWindowBytes || (screen.width && !io.screen)) {
                out.state=N33_PSPL_MAP_FAILED;
            } else {
                IOLog("Navi33Probe: VMID1 gradient RUN starts, window MC 0x%llx VRAM 0x%llx screen %ux%u at VRAM 0x%llx\n",
                      layout.windowMc,layout.windowVram,screen.width,screen.height,screen.vram);
                hw.write=write32;hw.vramWrite=vramWrite;hw.screenRead=screenRead;
                out=vg::execute(io.r,layout,screen,req.fw,req.fwBytes,hw,true,true,*req.attempted);
                out.flags|=win.flags|N33_PSPL_FLAG_WRITE_GATE_OPEN;
            }
            if(io.screen)io.screen->release();
            if(io.win)io.win->release();
        }
    }
    io.regs->release();
    IOLog("Navi33Probe: VMID1 gradient %s state %u steps %u writes %u L1 %x L2 %x ctx1 %x scratch %x "
          "fence %x rptr %u->%u fences %x/%x bad %u/%u fallbacks %u pixel(1,1) %08x screen %x "
          "fault %x/%x window 0x%x MiB ms %u\n",run ? "RUN" : "PLAN",out.state,out.steps_done,
          out.register_writes,out.vm_l1_before,out.vm_l2_before,out.vm_ctx_before,out.vm_scratch,
          out.vm_fence,out.grad_rptr_before,out.grad_rptr_after,out.grad_fence_a,out.grad_fence_b,
          out.grad_bad_pixels,out.grad_screen_bad,out.grad_fallbacks,out.grad_first_pixel,out.grad_screen,
          out.vm_fault_status,out.vm_fault_addr,out.vm_window,out.elapsed_ms);
    return out;
}
} } // namespace navi33::darwin_psp
