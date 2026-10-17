// SPDX-License-Identifier: MIT
// IOKit side of the PSP13 sOS load (Navi33Psp13Loader.h). The only code in
// Navi33Probe.kext that writes the GPU: C2PMSG_35/36 and the 1 MiB fw_pri
// window of BAR0, nothing else is mapped writable.
#pragma once
#include "Navi33NativeABI.h"
#include <stddef.h>
#include <stdint.h>
class IOPCIDevice;

namespace navi33 { namespace darwin_psp {
struct Request {
    uint32_t mode=0u;             // N33_PSP_LOAD_PLAN or N33_PSP_LOAD_RUN
    const uint8_t *rom=nullptr;   // IP Discovery
    size_t romBytes=0u;
    const uint8_t *fw=nullptr;    // psp_13_0_7_sos.bin
    size_t fwBytes=0u;
    uint64_t consolePhys=0u, consoleBytes=0u; // getConsoleInfo()
    bool writeGate=false;         // navi33-psp-load=1 and no navi33-psp-off
    bool *attempted=nullptr;      // provider-wide, one load per boot
};
// Caller holds the provider's PSP lock and has checked the PCI identity.
n33_psp_load_result execute(IOPCIDevice *pci,const Request &req);
} } // namespace navi33::darwin_psp
