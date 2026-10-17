/* SPDX-License-Identifier: MIT
 * N33N ABI v6 read-only RX7600 framebuffer and NBIO capacity probe.
 * Compiled for macOS only. Requires a signed/loaded Navi33Probe Kext,
 * trusted ATY,bin_image IP Discovery, and explicit boot args:
 * navi33-probe=1 navi33-client=1 navi33-vram-read=1
 * No GPU VRAM, firmware, MMIO or PCI configuration is modified.
 */
#include "../src/Navi33NativeABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *stateText(uint32_t state) {
    switch(state) {
    case N33_VRAM_DISABLED: return "disabled by boot argument";
    case N33_VRAM_NO_ROM: return "missing ATY,bin_image IP Discovery ROM";
    case N33_VRAM_BAD_DISCOVERY: return "invalid Navi33 IP inventory";
    case N33_VRAM_NO_BAR5: return "missing PCI register BAR5";
    case N33_VRAM_BAD_OFFSETS: return "unsupported IP version or BAR5 bounds";
    case N33_VRAM_MAP_FAILED: return "failed to map BAR5 read-only window";
    case N33_VRAM_MMIO_FAILED: return "invalid GPU MMIO read";
    case N33_VRAM_UNSTABLE: return "GPU register values changed during read";
    case N33_VRAM_DISAGREE: return "MMHUB and GFXHUB disagree";
    case N33_VRAM_PAIRED_READ: return "matching read-only framebuffer geometry";
    default: return "unknown status";
    }
}
static const char *capacityState(uint32_t state) {
    switch(state) {
    case N33_NBIO_NO_INVENTORY: return "NBIO IP not found";
    case N33_NBIO_UNSUPPORTED: return "unsupported NBIO IP revision";
    case N33_NBIO_MISSING_BASE: return "missing NBIO BAR5 base segment";
    case N33_NBIO_BAD_OFFSET: return "NBIO register outside assigned BAR5";
    case N33_NBIO_READ_FAILED: return "NBIO MMIO read error";
    case N33_NBIO_UNSTABLE: return "NBIO memory size changed between reads";
    case N33_NBIO_INVALID_CAPACITY: return "invalid NBIO MiB size";
    case N33_NBIO_STABLE: return "stable NBIO register capacity";
    case N33_NBIO_GEOMETRY_MISMATCH: return "capacity does not fit FB location";
    default: return "unknown NBIO status";
    }
}
int main(int argc,char **argv) {
    if(argc!=2 || strcmp(argv[1],"--read")!=0) {
        fputs("Usage: sudo n33vram --read\nNo GPU register writes are performed.\n",stderr);
        return 1;
    }
    if(geteuid()!=0) {
        fputs("Requires root for the N33N diagnostic user client.\n",stderr);
        return 2;
    }
    io_service_t svc=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!svc) {fputs("Navi33Probe Kext not loaded.\n",stderr);return 3;}
    io_connect_t conn=IO_OBJECT_NULL;
    kern_return_t kr=IOServiceOpen(svc,mach_task_self(),N33_UC_TYPE,&conn);
    IOObjectRelease(svc);
    if(kr!=KERN_SUCCESS) {
        fprintf(stderr,"N33N open failed: 0x%x\n",kr);return 4;
    }
    const uint64_t input[2]={N33_ABI_VERSION,0u};
    uint64_t hello[3]={0u,0u,0u};
    uint32_t count=3u;
    kr=IOConnectCallScalarMethod(conn,N33_SEL_HELLO,input,2u,hello,&count);
    if(kr!=KERN_SUCCESS || count!=3u || hello[0]!=N33_ABI_VERSION ||
       hello[1]!=N33_UC_TYPE || hello[2]!=N33_SEL_COUNT) {
        fputs("N33N ABI version mismatch.\n",stderr);
        IOServiceClose(conn);return 5;
    }
    struct n33_vram_geometry_status status={0};
    size_t bytes=sizeof(status);
    kr=IOConnectCallStructMethod(conn,N33_SEL_VRAM_GEOMETRY,
                                 NULL,0u,&status,&bytes);
    IOServiceClose(conn);
    if(kr!=KERN_SUCCESS || bytes!=sizeof(status) ||
       status.size!=sizeof(status) ||
       status.abi_version!=N33_ABI_VERSION) {
        fprintf(stderr,"Paired GMC11 FB read refused: 0x%x\n",kr);
        return 6;
    }
    printf("Navi33 VRAM register status: %u (%s)\n",
           status.state,stateText(status.state));
    printf("MMHUB3: FB_BASE=%08x TOP=%08x OFFSET=%08x revision=%u\n",
           status.mm_fb_base,status.mm_fb_top,status.mm_fb_offset,
           status.mmhub_revision);
    printf("GFXHUB: FB_BASE=%08x TOP=%08x OFFSET=%08x\n",
           status.gfx_fb_base,status.gfx_fb_top,status.gfx_fb_offset);
    printf("BAR0 assigned: %u MiB; BAR5 assigned: %u KiB; MMIO reads: %u\n",
           status.bar0_mib,status.bar5_kib,status.read_count);
    printf("Candidate GPU physical FB base: 0x%llx\n",
           (unsigned long long)status.candidate_physical_fb_base);
    printf("NBIO4.3 capacity state: %u (%s); revision=%08x; reads=%u\\n",
           status.nbio_state,capacityState(status.nbio_state),
           status.nbio_revision,status.nbio_reads);
    printf("Observed NBIO capacity: %u MiB\\n",status.nbio_memsize_mib);
    if((status.flags&N33_VRAM_FLAG_MC_GEOMETRY_VALID)!=0u) {
        printf("Cross-checked GPU MC start: 0x%llx; VRAM bytes: %llu\\n",
               (unsigned long long)status.verified_mc_start,
               (unsigned long long)status.verified_vram_bytes);
    } else {
        puts("No cross-checked GPU MC/VRAM geometry available.");
    }
    puts("No physical VRAM BO has been allocated or pinned by Cobalt64.");
    puts("Free ranges, PCI/firmware/SDMA ownership, real GPU compute,");
    puts("GART activation, and native Metal remain UNPROVEN.");
    return (status.flags&N33_VRAM_FLAG_MC_GEOMETRY_VALID)!=0u ? 0 : 7;
}
