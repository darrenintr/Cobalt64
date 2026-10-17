/* SPDX-License-Identifier: MIT
 * RX7600 N33N device-bound DMA STAGING smoke tool (not GPU execution).
 * This program is compile-only in GitHub Actions and never installed/run
 * automatically. It only maps 64KiB ordinary RAM via OS IOMMU and releases it.
 *
 * Command (on a RECOVERABLE macOS installation):
 *   sudo ./n33dma --status
 *   sudo ./n33dma --stage    # also needs opt-in boot argument
 * No Kext loading, no GPU writes, no SDMA submission, no Metal.
 */
#include "../src/Navi33NativeABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int callStatus(io_connect_t conn,uint32_t selector,
                      struct n33_dma_stage_status *status) {
    if (!status) return -1;
    memset(status,0,sizeof(*status));
    size_t out=sizeof(*status);
    const kern_return_t k=IOConnectCallStructMethod(conn,selector,
                                                    NULL,0u,status,&out);
    if(k!=KERN_SUCCESS || out!=sizeof(*status) ||
       status->size!=sizeof(*status) ||
       status->abi_version!=N33_ABI_VERSION ||
       status->reserved0 || status->reserved1) {
        fprintf(stderr,"N33N selector %u failed or ABI mismatch (0x%x)\n",
                selector,k);
        return -1;
    }
    return 0;
}
int main(int argc,char **argv) {
    if(argc!=2 || (strcmp(argv[1],"--status") &&
                   strcmp(argv[1],"--stage"))) {
        fprintf(stderr,"usage: n33dma --status|--stage\n");
        return 1;
    }
    if(geteuid()!=0) {
        fprintf(stderr,"N33N diagnostic client requires administrator/root\n");
        return 2;
    }
    io_service_t svc=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!svc) {fputs("No Navi33Probe Kext service\n",stderr);return 3;}
    io_connect_t conn=IO_OBJECT_NULL;
    kern_return_t k=IOServiceOpen(svc,mach_task_self(),N33_UC_TYPE,&conn);
    IOObjectRelease(svc);
    if(k!=KERN_SUCCESS) {
        fprintf(stderr,"IOServiceOpen N33N rejected: 0x%x\n",k);return 4;
    }
    uint64_t request[2]={N33_ABI_VERSION,0u},hello[3]={0,0,0};
    uint32_t count=3u;
    k=IOConnectCallScalarMethod(conn,N33_SEL_HELLO,
                                 request,2u,hello,&count);
    if(k!=KERN_SUCCESS || count!=3u ||
       hello[0]!=N33_ABI_VERSION || hello[1]!=N33_UC_TYPE ||
       hello[2]!=N33_SEL_COUNT) {
        fputs("N33N handshake or selector count mismatch\n",stderr);
        IOServiceClose(conn);return 5;
    }
    struct n33_dma_stage_status s={0};
    int rc=callStatus(conn,N33_SEL_DMA_STATUS,&s);
    if(rc!=0) {IOServiceClose(conn);return 6;}
    printf("Initial: state=%u flags=0x%x pages=%u; no GPU execution\n",
           s.state,s.flags,s.page_count);
    if(strcmp(argv[1],"--stage")==0) {
        // This requests an OS-IOMMU mapping ONLY, never an SDMA GPU command.
        if(callStatus(conn,N33_SEL_DMA_STAGE,&s)!=0)
            rc=7;
        else {
            printf("Stage: state=%u flags=0x%x pages=%u segments=%u "
                   "ring_dwords=%u source_fnv=%016llx target_fnv=%016llx\n",
                   s.state,s.flags,s.page_count,s.segment_count,s.ring_dwords,
                   (unsigned long long)s.source_hash,
                   (unsigned long long)s.destination_hash);
            if(s.state!=N33_DMA_STAGED ||
               (s.flags&(N33_DMA_FLAG_IOMMU_MAPPED|
                         N33_DMA_FLAG_CPU_IMAGE_READY)) !=
                         (N33_DMA_FLAG_IOMMU_MAPPED|
                          N33_DMA_FLAG_CPU_IMAGE_READY))
                rc=8;
        }
        // ALWAYS revoke pinning and the mapper before exit (also on failure).
        k=IOConnectCallScalarMethod(conn,N33_SEL_DMA_RELEASE,
                                     NULL,0u,NULL,NULL);
        if(k!=KERN_SUCCESS) {fprintf(stderr,"DMA release failed 0x%x\n",k);rc=9;}
        if(callStatus(conn,N33_SEL_DMA_STATUS,&s)!=0 ||
           s.state!=N33_DMA_NOT_PREPARED) {
            fputs("DMA status after release incorrect\n",stderr);rc=10;
        }
    }
    IOServiceClose(conn); // clientClose also revokes mappings
    return rc;
}
