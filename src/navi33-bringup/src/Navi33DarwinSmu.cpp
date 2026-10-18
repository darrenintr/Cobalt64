// SPDX-License-Identifier: MIT
// Only MP1 C2PMSG_66/82/90 can be written. No VRAM or ROM mapping.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Smu13Enable.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
namespace navi33 { namespace darwin_psp {
namespace {
struct Io { IOMemoryMap *map=nullptr; smu13::Regs regs{}; };
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
    if(!io->regs.valid || (off!=io->regs.msg && off!=io->regs.arg && off!=io->regs.resp) ||
       !io->map || (off&3u) || off>io->map->getLength() ||
       io->map->getLength()-off<4u)return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->map->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");return true;
}
void sleepMs(void *,uint32_t ms){IOSleep(ms);}
void trace(void *,uint32_t msg,uint32_t param,uint32_t reply) {
    IOLog("Navi33Probe: SMU msg %x param %08x %s %08x\n",msg,param,
          reply ? "response" : "SEND",reply);
}
}
n33_psp_load_result executeSmu(IOPCIDevice *pci,const Request &req) {
    auto out=smu13::blank();out.state=N33_PSPL_BAD_REQUEST;
    if(!pci || !req.attempted || (req.mode!=N33_SMU_ENABLE_PLAN && req.mode!=N33_SMU_ENABLE_RUN))return out;
    IPDiscovery d;
    if(!req.rom || req.romBytes<64u || req.romBytes>IPDiscovery::kMaxBlob || !d.parse(req.rom,req.romBytes)) {
        out.state=N33_PSPL_BAD_ROM;return out;
    }
    IODeviceMemory *bar5=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if(!bar5){out.state=N33_PSPL_NO_BARS;return out;}
    Io io{};io.regs=smu13::resolve(d,bar5->getLength());
    if(!io.regs.valid){out.state=N33_PSPL_BAD_ROM;return out;}
    io.map=bar5->map();
    if(!io.map || io.map->getLength()<bar5->getLength()) {
        if(io.map)io.map->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    const bool run=req.mode==N33_SMU_ENABLE_RUN;
    smu13::Hw hw{&io,read32,run ? write32 : nullptr,sleepMs,trace};
    out=smu13::execute(io.regs,hw,run,req.writeGate,*req.attempted);
    io.map->release();
    IOLog("Navi33Probe: SMU %s state %u mask %08x:%08x completed %u writes %u "
          "last %x/%x replies %x/%x/%x core %x->%x reset %x->%x RLC %x->%x ms %u\n",
          run ? "RUN" : "PLAN",out.state,out.smu_mask_high,out.smu_mask_low,
          out.steps_done,out.register_writes,out.smu_last_message,out.smu_last_argument,
          out.smu_responses[0],out.smu_responses[1],out.smu_responses[2],
          out.smu_core_before,out.smu_core_after,out.smu_reset_before,out.smu_reset_after,
          out.smu_rlc_before,out.smu_rlc_after,out.elapsed_ms);
    return out;
}
} }
