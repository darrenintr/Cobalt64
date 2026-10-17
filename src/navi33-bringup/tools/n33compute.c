/* SPDX-License-Identifier: MIT
 * Read real RX 7600 firmware / SDMA / MMHUB / GFXHUB state via N33N.
 * Compile on macOS, run manually:
 *   sudo ./n33compute --probe
 *
 * NO GPU compute is claimed, no firmware is loaded, no MMIO writes.
 */
#include "../src/Navi33NativeABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *why(uint32_t value) {
    switch(value) {
    case N33_HW_DISABLED: return "disabled by boot argument";
    case N33_HW_NO_PROVIDER: return "RX7600 PCI provider unavailable";
    case N33_HW_NO_ROM: return "no ATY,bin_image ROM supplied to macOS";
    case N33_HW_BAD_ROM: return "IP Discovery signature/version invalid";
    case N33_HW_NO_BAR5: return "no BAR5 register resource";
    case N33_HW_BAD_OFFSETS: return "cannot resolve correct PSP/SDMA/HUB offsets";
    case N33_HW_MAP_FAILED: return "cannot map BAR5 device registers";
    case N33_HW_MMIO_READ_FAILED: return "GPU register read failed";
    case N33_HW_PSP_NOT_RUNNING: return "PSP13 SOS not alive";
    case N33_HW_SDMA_READ_FAILED: return "SDMA0 register reads unavailable";
    case N33_HW_SHARED_GPU_STATE: return "VM context or SDMA queue active / unowned";
    case N33_HW_NO_OWNED_GPU_VM: return "GPU memory/queue ownership not established";
    case N33_HW_COMPUTE_COMPLETE: return "RESERVED, not implemented";
    default: return "unknown code";
    }
}
int main(int argc,char **argv) {
    const int usingFile=(argc==3 && strcmp(argv[1],"--rom")==0);
    const int usingQueue=(argc==2 && strcmp(argv[1],"--queue")==0);
    if(!usingFile && !usingQueue &&
       (argc!=2 || strcmp(argv[1],"--probe")!=0)) {
        fputs("Usage: sudo n33compute --probe | --queue | --rom rx7600.rom\n"
              "All operations are read-only hardware diagnostics.\n",stderr);
        return 1;
    }
    if(geteuid()!=0) {
        fputs("N33N requires root/admin.\n",stderr);
        return 2;
    }
    io_service_t svc=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!svc) {fputs("Navi33Probe service not loaded.\n",stderr);return 3;}
    io_connect_t conn=IO_OBJECT_NULL;
    kern_return_t kr=IOServiceOpen(svc,mach_task_self(),N33_UC_TYPE,&conn);
    IOObjectRelease(svc);
    if(kr!=KERN_SUCCESS) {
        fprintf(stderr,"N33N open refused (boot flags?): 0x%x\n",kr);
        return 4;
    }
    const uint64_t in[2]={N33_ABI_VERSION,0u};
    uint64_t hello[3]={0,0,0};uint32_t count=3u;
    kr=IOConnectCallScalarMethod(conn,N33_SEL_HELLO,in,2u,hello,&count);
    if(kr!=KERN_SUCCESS || count!=3u || hello[0]!=N33_ABI_VERSION ||
       hello[1]!=N33_UC_TYPE || hello[2]!=N33_SEL_COUNT) {
        fputs("N33N ABI version mismatch.\n",stderr);
        IOServiceClose(conn);return 5;
    }
    if(usingQueue) {
        struct n33_queue_census_status q={0};
        size_t qBytes=sizeof(q);
        kr=IOConnectCallStructMethod(conn,N33_SEL_QUEUE_CENSUS,
                                     NULL,0u,&q,&qBytes);
        IOServiceClose(conn);
        if(kr!=KERN_SUCCESS || qBytes!=sizeof(q) ||
           q.abi_version!=N33_ABI_VERSION ||
           q.size!=sizeof(q) || q.reserved0 || q.reserved1) {
            fprintf(stderr,"N33N Queue0 census failed: 0x%x\n",kr);
            return 6;
        }
        printf("RX7600 physical SDMA6 Queue0: state=%u reads=%u flags=0x%08x\n",
               q.state,q.read_count,q.flags);
        if(q.state==N33_QUEUE_OBSERVED) {
            printf("RB_CNTL=%08x DOORBELL=%08x\n",
                   q.ring_control,q.doorbell_control);
            printf("RPTR=%08x:%08x WPTR=%08x:%08x\n",
                   q.ring_rptr_hi,q.ring_rptr_lo,
                   q.ring_wptr_hi,q.ring_wptr_lo);
            printf("RB_BASE=%08x:%08x (256-byte units)\n",
                   q.ring_base_hi,q.ring_base_lo);
            puts((q.flags&N33_QUEUE_FLAG_APPARENTLY_PARKED)
                  ? "Registers appear parked, but ownership is UNPROVEN."
                  : "Existing Queue0 registers are in use/configured; no takeover.");
        } else {
            puts("Queue0 observation unavailable or unstable; no takeover.");
        }
        puts("GPU Copy submitted: NO; this only reads physical BAR5.");
        return q.state==N33_QUEUE_OBSERVED ? 0 : 7;
    }
    void *rom=NULL;
    size_t romLen=0u;
    if(usingFile) {
        FILE *fp=fopen(argv[2],"rb");
        if(!fp) {
            perror("Cannot open RX7600 VBIOS");
            IOServiceClose(conn);return 8;
        }
        long len=-1;
        if(fseek(fp,0L,SEEK_END)==0)len=ftell(fp);
        if(len<64L || len>4L*1024L*1024L ||
           fseek(fp,0L,SEEK_SET)!=0) {
            fputs("VBIOS ROM must be 64 bytes to 4 MiB.\n",stderr);
            fclose(fp);IOServiceClose(conn);return 8;
        }
        romLen=(size_t)len;
        rom=malloc(romLen);
        if(!rom || fread(rom,1u,romLen,fp)!=romLen) {
            fputs("Failed to read VBIOS.\n",stderr);
            free(rom);fclose(fp);IOServiceClose(conn);return 8;
        }
        fclose(fp);
    }
    struct n33_hw_preflight hw={0};
    size_t size=sizeof(hw);
    kr=IOConnectCallStructMethod(conn,
        usingFile?N33_SEL_HW_ROM_PREFLIGHT:N33_SEL_HW_PREFLIGHT,
        rom,romLen,&hw,&size);
    free(rom);
    IOServiceClose(conn);
    if(kr!=KERN_SUCCESS || size!=sizeof(hw) ||
       hw.abi_version!=N33_ABI_VERSION ||
       hw.size!=sizeof(hw) || hw.reserved0) {
        fprintf(stderr,"N33N live diagnostic failed: 0x%x\n",kr);
        return 6;
    }
    printf("RX7600 physical preflight: reason %u (%s)\n",
           hw.reason,why(hw.reason));
    printf("BAR5=%u KiB; actual MMIO reads=%u; flags=0x%08x\n",
           hw.bar5_kib,hw.mmio_reads,hw.flags);
    printf("PSP13 state=%u alive=%08x SOS version=%08x\n",
           hw.psp_state,hw.psp_alive,hw.sos_version);
    printf("SDMA6 state=%u RB_CNTL=%08x RPTR=%08x WPTR=%08x\n",
           hw.sdma_state,hw.sdma_control,hw.sdma_rptr,hw.sdma_wptr);
    printf("MMHUB_CTX0=%08x GFXHUB_CTX0=%08x\n",
           hw.mmhub_context0,hw.gfxhub_context0);
    puts("GPU Copy submitted: NO; this executable is a diagnostic only.");
    return hw.reason==N33_HW_NO_OWNED_GPU_VM ||
           hw.reason==N33_HW_SHARED_GPU_STATE ? 0 : 7;
}
