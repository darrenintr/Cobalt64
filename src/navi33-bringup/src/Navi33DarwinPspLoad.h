// SPDX-License-Identifier: MIT
// IOKit side of the PSP13 sOS load (Navi33Psp13Loader.h). The only code in
// Navi33Probe.kext that writes the GPU: C2PMSG_35/36 and the 1 MiB fw_pri
// window of BAR0, nothing else is mapped writable.
#pragma once
#include "Navi33NativeABI.h"
#include <stddef.h>
#include <stdint.h>
class IOPCIDevice;
namespace navi33 { namespace vram_heap { class Heap; } namespace gmc11_vm { class Space; } }

namespace navi33 { namespace darwin_psp {
struct Request {
    uint32_t mode=0u;             // N33_PSP_LOAD_PLAN or N33_PSP_LOAD_RUN
    const uint8_t *rom=nullptr;   // IP Discovery
    size_t romBytes=0u;
    const uint8_t *fw=nullptr;    // psp_13_0_7_sos.bin
    size_t fwBytes=0u;
    uint64_t consolePhys=0u, consoleBytes=0u; // boot_args->Video, physical
    // navi33-hostfb-offset/-bytes: the host's view, used only without the above.
    bool hostConsole=false;
    uint64_t hostConsoleOffset=0u, hostConsoleBytes=0u;
    bool writeGate=false;         // this mode's boot-arg is 1 and no navi33-psp-off
    uint32_t stopAfter=0u;        // ring modes: 1 ring, 2 +LOAD_TOC, 3 +SETUP_TMR;
                                  // fw modes: records [startAt, stopAfter), 27 = +AUTOLOAD_RLC
    uint32_t startAt=0u;
    bool autoloadGate=false;      // navi33-psp-autoload=1 (and no navi33-psp-off)
    bool *attempted=nullptr;      // provider-wide, one load per boot
    vram_heap::Heap *heap=nullptr;   // the provider's VRAM heap (VM_ALLOC modes)
    gmc11_vm::Space *vm1=nullptr;    // VMID 1's address space (VM_ALLOC modes)
};
// Caller holds the provider's PSP lock and has checked the PCI identity.
n33_psp_load_result execute(IOPCIDevice *pci,const Request &req);
// Ring stage (modes RING_PLAN / RING_RUN), Navi33DarwinPspRing.cpp.
n33_psp_load_result executeRing(IOPCIDevice *pci,const Request &req);
// IP firmware stage (modes FW_PLAN / FW_RUN), Navi33DarwinPspFw.cpp.
n33_psp_load_result executeFw(IOPCIDevice *pci,const Request &req);
n33_psp_load_result executeSmu(IOPCIDevice *pci,const Request &req);
// MES start (modes MES_START_PLAN / MES_START_RUN), Navi33DarwinMes.cpp.
n33_psp_load_result executeMes(IOPCIDevice *pci,const Request &req);
// MEC queue (modes MEC_QUEUE_PLAN / MEC_QUEUE_RUN), Navi33DarwinMecQueue.cpp.
n33_psp_load_result executeMec(IOPCIDevice *pci,const Request &req);
// Gradient (modes GRADIENT_PLAN / GRADIENT_RUN), Navi33DarwinGradient.cpp.
n33_psp_load_result executeGradient(IOPCIDevice *pci,const Request &req);
// VMID1 queue (modes VM_QUEUE_PLAN / VM_QUEUE_RUN), Navi33DarwinVmid.cpp.
n33_psp_load_result executeVm(IOPCIDevice *pci,const Request &req);
// VMID1 gradient (modes VM_GRADIENT_PLAN / VM_GRADIENT_RUN), Navi33DarwinVmGradient.cpp.
n33_psp_load_result executeVmGradient(IOPCIDevice *pci,const Request &req);
// VMID1 gradient on the VRAM heap (modes VM_ALLOC_PLAN / VM_ALLOC_RUN), Navi33DarwinVmAlloc.cpp.
n33_psp_load_result executeVmAlloc(IOPCIDevice *pci,const Request &req);
} } // namespace navi33::darwin_psp
