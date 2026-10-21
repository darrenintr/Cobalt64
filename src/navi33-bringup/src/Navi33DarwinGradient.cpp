// SPDX-License-Identifier: MIT
// Gradient stage, IOKit side (runner: Navi33Gfx11Gradient.h). Writes only the
// runner's allowlist (GRBM select, SH_MEM_CONFIG, CP_PQ_WPTR_POLL_CNTL1, the
// queue's WPTR as fallback) and G34's 1 MiB VRAM window. The GPU itself writes
// the console framebuffer; the CPU only reads it back, through a read-only map.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Discovery.h"
#include "Navi33Gfx11Gradient.h"
#include "Navi33Psp13Loader.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_psp {
namespace {
namespace pl=psp13load;
namespace mq=mec_queue;
namespace gr=gradient;
constexpr uint64_t kProofBytes=16u*1024u*1024u, kWindowFromProof=10u*1024u*1024u;
// The VM's host console (vm/prepare-card.sh): 1024x768x32, 4096-byte lines.
constexpr uint64_t kVmConsoleBytes=1024u*768u*4u;
struct Io { IOMemoryMap *regs=nullptr, *win=nullptr, *screen=nullptr; gr::Regs r{}; };
bool read32(void *ctx,uint64_t off,uint32_t *out) {
    IOMemoryMap *map=static_cast<Io *>(ctx)->regs;
    if(!map || !out || (off&3u) || off>map->getLength() || map->getLength()-off<4u)return false;
    *out=*reinterpret_cast<const volatile uint32_t *>(
        static_cast<uintptr_t>(map->getVirtualAddress())+off);
    return true;
}
bool write32(void *ctx,uint64_t off,uint32_t value) {
    Io *io=static_cast<Io *>(ctx);
    if(!gr::allowed(io->r,off) || !io->regs || (off&3u) || off>io->regs->getLength() ||
       io->regs->getLength()-off<4u)return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->regs->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
volatile uint32_t *window(Io *io,uint32_t off,uint32_t dwords) {
    const uint64_t len=4ull*dwords;
    if(!io->win || io->win->getLength()!=mq::kWindowBytes || (off&3u) ||
       off>mq::kWindowBytes || len>mq::kWindowBytes-off)return nullptr;
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

n33_psp_load_result executeGradient(IOPCIDevice *pci,const Request &req) {
    auto out=gr::blank();out.state=N33_PSPL_BAD_REQUEST;
    if(!pci || !req.attempted || (req.mode!=N33_GRADIENT_PLAN && req.mode!=N33_GRADIENT_RUN))
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
    Io io{};io.r=gr::resolve(disc,bar5Bytes);
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
    const mq::Layout layout{win.fwPriMc+kWindowFromProof};
    gr::Screen screen{};
    if(boot.phys)  // a relocated console has the same geometry (console::resolve)
        screen=gr::consoleScreen(win.fbBaseMc+win.consoleOffset,console.bytes,req.consoleWidth,
                                 req.consoleHeight,req.consoleRow,req.consoleDepth);
    else if(console.bytes==kVmConsoleBytes)
        screen={win.fbBaseMc+win.consoleOffset,1024u,768u,1024u};
    // The window is read in the plan too (G34's fence and WPTR), never written there.
    IODeviceMemory *sub=IODeviceMemory::withSubRange(bar0,win.fwPriOffset+kWindowFromProof,mq::kWindowBytes);
    if(sub) {io.win=sub->map(kIOMapInhibitCache);sub->release();}
    if(!io.win || io.win->getLength()!=mq::kWindowBytes) {
        if(io.win)io.win->release();
        io.regs->release();out.state=N33_PSPL_MAP_FAILED;return out;
    }
    const bool run=req.mode==N33_GRADIENT_RUN;
    gr::Hw hw{&io,read32,nullptr,nullptr,vramRead,nullptr,sleepMs};
    out=gr::execute(io.r,layout,screen,req.fw,req.fwBytes,hw,false,req.writeGate,*req.attempted);
    if(run && out.state==N33_PSPL_PLAN_OK) {
        if(!req.writeGate) out.state=N33_PSPL_WRITE_DISABLED;
        else if(*req.attempted) out.state=N33_PSPL_ALREADY_ATTEMPTED;
        else {
            if(screen.width) {
                IODeviceMemory *s=IODeviceMemory::withSubRange(bar0,win.consoleOffset,console.bytes);
                if(s) {io.screen=s->map(kIOMapInhibitCache|kIOMapReadOnly);s->release();}
            }
            if(screen.width && !io.screen) out.state=N33_PSPL_MAP_FAILED;
            else {
                IOLog("Navi33Probe: gradient RUN starts, window MC 0x%llx screen %ux%u at MC 0x%llx\n",
                      layout.windowMc,screen.width,screen.height,screen.mc);
                hw.write=write32;hw.vramWrite=vramWrite;hw.screenRead=screenRead;
                out=gr::execute(io.r,layout,screen,req.fw,req.fwBytes,hw,true,true,*req.attempted);
            }
            if(io.screen)io.screen->release();
        }
    }
    io.win->release();
    io.regs->release();
    IOLog("Navi33Probe: gradient %s state %u steps %u writes %u SH_MEM %x rptr %u->%u fences %x/%x "
          "bad %u/%u fallbacks %u pixel(1,1) %08x screen %x ms %u\n",run ? "RUN" : "PLAN",out.state,
          out.steps_done,out.register_writes,out.grad_shmem_before,out.grad_rptr_before,
          out.grad_rptr_after,out.grad_fence_a,out.grad_fence_b,out.grad_bad_pixels,
          out.grad_screen_bad,out.grad_fallbacks,out.grad_first_pixel,out.grad_screen,out.elapsed_ms);
    return out;
}
} } // namespace navi33::darwin_psp
