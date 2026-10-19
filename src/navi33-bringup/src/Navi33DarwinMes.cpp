// SPDX-License-Identifier: MIT
// Only GRBM_GFX_CNTL, CP_MES_CNTL and CP_MES_PRGRM_CNTR_START/_HI can be
// written. No VRAM or ROM mapping.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Gfx11MesStart.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
namespace navi33 { namespace darwin_psp {
namespace {
struct Io { IOMemoryMap *map=nullptr; mes_start::Regs regs{}; };
bool read32(void *ctx,uint64_t off,uint32_t *out) {
    Io *io=static_cast<Io *>(ctx);
    if(!out || !io->map || (off&3u) || off>io->map->getLength() ||
       io->map->getLength()-off<4u)return false;
    *out=*reinterpret_cast<const volatile uint32_t *>(
        static_cast<uintptr_t>(io->map->getVirtualAddress())+off);
    return true;
}
bool write32(void *ctx,uint64_t off,uint32_t value) {
    Io *io=static_cast<Io *>(ctx);
    if(!mes_start::allowed(io->regs,off) || !io->map || (off&3u) ||
       off>io->map->getLength() || io->map->getLength()-off<4u)return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->map->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");return true;
}
void sleepMs(void *,uint32_t ms){IOSleep(ms);}
}
n33_psp_load_result executeMes(IOPCIDevice *pci,const Request &req) {
    auto out=mes_start::blank();out.state=N33_PSPL_BAD_REQUEST;
    if(!pci || !req.attempted || (req.mode!=N33_MES_START_PLAN && req.mode!=N33_MES_START_RUN))
        return out;
    IPDiscovery d;
    if(!req.rom || req.romBytes<64u || req.romBytes>IPDiscovery::kMaxBlob || !d.parse(req.rom,req.romBytes)) {
        out.state=N33_PSPL_BAD_ROM;return out;
    }
    IODeviceMemory *bar5=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if(!bar5){out.state=N33_PSPL_NO_BARS;return out;}
    Io io{};io.regs=mes_start::resolve(d,bar5->getLength());
    if(!io.regs.valid){out.state=N33_PSPL_BAD_ROM;return out;}
    io.map=bar5->map();
    if(!io.map || io.map->getLength()<bar5->getLength()) {
        if(io.map)io.map->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    const bool run=req.mode==N33_MES_START_RUN;
    mes_start::Hw hw{&io,read32,run ? write32 : nullptr,sleepMs};
    out=mes_start::execute(io.regs,hw,run,req.writeGate,*req.attempted);
    io.map->release();
    IOLog("Navi33Probe: MES %s state %u steps %u writes %u cntl %x->%x grbm %x RLC %x CP_STAT %x "
          "version %x/%x pc %x/%x samples %u ms %u\n",
          run ? "RUN" : "PLAN",out.state,out.steps_done,out.register_writes,
          out.mes_cntl_before,out.mes_cntl_after,out.mes_grbm_before,out.mes_rlc,out.mes_cp_stat,
          out.mes_version[0],out.mes_version[1],out.mes_pc[0],out.mes_pc[1],
          out.mes_samples,out.elapsed_ms);
    return out;
}
} }
