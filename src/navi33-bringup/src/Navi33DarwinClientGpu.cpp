// SPDX-License-Identifier: MIT
// IOKit side of the per-client GPU contexts (see Navi33DarwinClientGpu.h).
#include "Navi33DarwinClientGpu.h"
#include "Navi33Discovery.h"
#include "Navi33EmbeddedFirmware.h"
#include "Navi33Gfx11VmAlloc.h"
#include "Navi33Psp13Loader.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_client {
namespace {
namespace pl=psp13load;
namespace cg=client_gpu;
constexpr uint64_t kProofBytes=16u*1024u*1024u;
bool read32(void *ctx,uint64_t off,uint32_t *out) {
    IOMemoryMap *map=static_cast<Host *>(ctx)->regs;
    if(!map || !out || (off&3u) || off>map->getLength() || map->getLength()-off<4u)return false;
    *out=*reinterpret_cast<const volatile uint32_t *>(static_cast<uintptr_t>(map->getVirtualAddress())+off);
    return true;
}
bool write32(void *ctx,uint64_t off,uint32_t value) {
    Host *h=static_cast<Host *>(ctx);
    if(!cg::allowed(h->r,off) || !h->regs || (off&3u) || off>h->regs->getLength() || h->regs->getLength()-off<4u)
        return false;
    *reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(h->regs->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
// Only inside one live, unpinned heap block, and inside the mapped BAR0.
volatile uint32_t *vram(Host *h,uint64_t off,uint32_t dwords) {
    const uint64_t len=4ull*dwords;
    if(!h->bar || !h->heap || !dwords || (off&3u) || !h->heap->covers(off,len) ||
       off>h->bar->getLength() || len>h->bar->getLength()-off)return nullptr;
    return reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(h->bar->getVirtualAddress())+off);
}
bool vramWrite(void *ctx,uint64_t off,const uint32_t *src,uint32_t dwords) {
    volatile uint32_t *w=vram(static_cast<Host *>(ctx),off,dwords);
    if(!w || !src)return false;
    for(uint32_t i=0;i<dwords;++i)w[i]=src[i];
    __asm__ __volatile__("sfence; mfence" ::: "memory");
    return true;
}
bool vramRead(void *ctx,uint64_t off,uint32_t *dst,uint32_t dwords) {
    volatile uint32_t *w=vram(static_cast<Host *>(ctx),off,dwords);
    if(!w || !dst)return false;
    for(uint32_t i=0;i<dwords;++i)dst[i]=w[i];
    return true;
}
// Only the six client queues' and the gfx ring's 64-bit doorbells, inside the mapped BAR2.
bool doorbell(void *ctx,uint32_t off,uint64_t value) {
    IOMemoryMap *m=static_cast<Host *>(ctx)->doorbells;
    bool client=off==navi33::gfx_ring::kDoorbellByte;
    for(uint32_t i=0;i<cg::kClients;++i)if(off==cg::doorbellByte(i))client=true;
    if(!m || !client || off>m->getLength() || m->getLength()-off<8u)return false;
    __asm__ __volatile__("mfence" ::: "memory");     // ring and WPTR in VRAM first
    *reinterpret_cast<volatile uint64_t *>(static_cast<uintptr_t>(m->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
// Short waits spin (the TLB ACK); longer ones sleep so a waiting client does not burn a CPU.
void delayUs(void *,uint32_t us) { if(us<50u)IODelay(us); else IOSleep((us+999u)/1000u); }
} // namespace

client_gpu::Hw hw(Host &h) { return {&h,read32,write32,vramWrite,vramRead,delayUs,doorbell}; }

client_gpu::Code start(Host &h,IOPCIDevice *pci,const darwin_psp::Request &req,vram_heap::Heap &heap) {
    if(h.startCode>=0)return static_cast<cg::Code>(h.startCode);
    cg::Code code=cg::Code::HwFailed;
    IPDiscovery disc;
    IODeviceMemory *bar5=pci ? pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5) : nullptr;
    IODeviceMemory *bar0=pci ? pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0) : nullptr;
    IODeviceMemory *bar2=pci ? pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2) : nullptr;
    if(!bar5 || !bar0 || !bar2 || !disc.parse(embedded::kIpd,embedded::kIpdBytes)){h.startCode=int(code);return code;}
    const uint64_t bar5Bytes=static_cast<uint64_t>(bar5->getLength());
    const pl::Range bar0Range{static_cast<uint64_t>(bar0->getPhysicalAddress()),static_cast<uint64_t>(bar0->getLength())};
    const pl::Range boot{req.consolePhys,req.consoleBytes};
    const pl::Range console=boot.phys ? boot :
        pl::hostConsole(boot,req.hostConsole,req.hostConsoleOffset,req.hostConsoleBytes,bar0Range);
    const pl::Registers pregs=pl::resolve(disc,bar5Bytes);
    h.r=cg::resolve(disc,bar5Bytes);h.heap=&heap;
    if(pregs.valid && h.r.valid)h.regs=bar5->map();
    pl::Snapshot snap{};
    if(h.regs && pl::readSnapshot(pregs,read32,&h,snap)) {
        // The same proof as every stage: an active scanout at the console's BAR0
        // offset shows BAR0 offset == VRAM offset.
        const pl::Plan win=pl::planWindow(snap,console,bar0Range,true,kProofBytes);
        if(win.state==N33_PSPL_PLAN_OK &&
           vm_alloc::reservations(heap,bar0Range.bytes,win.consoleOffset,console.bytes)) {
            h.fbBaseMc=win.fbBaseMc;h.bar0Phys=bar0Range.phys;h.bar0Bytes=bar0Range.bytes;
            h.bar=bar0->map(kIOMapInhibitCache);
            h.doorbells=bar2->map(kIOMapInhibitCache);
            if(h.bar && static_cast<uint64_t>(h.bar->getLength())==bar0Range.bytes && h.doorbells &&
               h.doorbells->getLength()>=cg::doorbellByte(cg::kClients-1u)+8u)
                code=h.dev.start(h.r,hw(h),heap,h.fbBaseMc);
        } else code=cg::Code::NotReady;
    }
    IOLog("Navi33Probe: client GPU start %u (FB MC 0x%llx, BAR0 %llu MiB, heap free %llu MiB)\n",
          static_cast<unsigned>(code),h.fbBaseMc,h.bar0Bytes>>20,heap.freeBytes()>>20);
    if(code!=cg::Code::Ok) {
        if(h.bar){h.bar->release();h.bar=nullptr;}
        if(h.doorbells){h.doorbells->release();h.doorbells=nullptr;}
        if(h.regs){h.regs->release();h.regs=nullptr;}
    }
    h.startCode=int(code);
    return code;
}

IOMemoryDescriptor *boMemory(Host &h,IOPCIDevice *pci,uint32_t slot,uint32_t handle) {
    uint64_t off=0,bytes=0;
    IODeviceMemory *bar0=pci ? pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0) : nullptr;
    if(!bar0 || !h.heap || !h.dev.boRange(slot,handle,off,bytes) || !h.heap->covers(off,bytes) ||
       off+bytes>h.bar0Bytes)return nullptr;
    return IODeviceMemory::withSubRange(bar0,off,bytes);
}
} } // namespace navi33::darwin_client
