/* SPDX-License-Identifier: MIT
 * macOS diagnostic client: IOServiceOpen(Navi33Probe, N33N).
 * Safe: sends only HELLO and QUERY_CAPS, never BO/VM/SUBMIT.
 * Build on macOS (not run by CI):
 *   clang -std=c11 -Wall -Wextra -Werror -I../src n33query.c \
 *     -framework IOKit -framework CoreFoundation -o n33query
 */
#include "../src/Navi33NativeABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>

int main(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                       IOServiceMatching("Navi33Probe"));
    if (!service) {
        fputs("Navi33Probe service not present; no driver is installed/loaded.\n", stderr);
        return 2;
    }
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t ret = IOServiceOpen(service, mach_task_self(), N33_UC_TYPE, &conn);
    IOObjectRelease(service);
    if (ret != KERN_SUCCESS) {
        fprintf(stderr, "N33N open failed: 0x%x (root and navi33-client=1 required)\n", ret);
        return 3;
    }
    uint64_t req[2] = { N33_ABI_VERSION, 0ull }, hello[3] = { 0,0,0 };
    uint32_t count = 3u;
    ret = IOConnectCallScalarMethod(conn, N33_SEL_HELLO, req, 2u, hello, &count);
    if (ret != KERN_SUCCESS || count != 3u || hello[0] != N33_ABI_VERSION ||
        hello[1] != N33_UC_TYPE || hello[2] != N33_SEL_COUNT) {
        fprintf(stderr, "N33N HELLO failed or protocol mismatch: 0x%x\n", ret);
        IOServiceClose(conn);
        return 4;
    }
    struct n33_capabilities caps = {0};
    size_t size = sizeof(caps);
    ret = IOConnectCallStructMethod(conn, N33_SEL_QUERY_CAPS, NULL, 0,
                                    &caps, &size);
    if (ret != KERN_SUCCESS || size != sizeof(caps) ||
        !n33_is_reserved_clean(&caps) || caps.abi_version != N33_ABI_VERSION) {
        fprintf(stderr, "N33N QUERY_CAPS failed or malformed: 0x%x\n", ret);
        IOServiceClose(conn);
        return 5;
    }
    printf("N33N: %04x:%04x gfx%u; flags=0x%08x; VRAM=%llu; GTT=%llu\n",
        caps.vendor_id, caps.device_id, caps.family, caps.capabilities,
        (unsigned long long)caps.vram_bytes,
        (unsigned long long)caps.gtt_bytes);
    struct n33_psp_status psp = {0};
    size_t psp_size=sizeof(psp);
    const kern_return_t psprc = IOConnectCallStructMethod(
        conn, N33_SEL_PSP_STATUS, NULL, 0u, &psp, &psp_size);
    if (psprc == KERN_SUCCESS && psp_size == sizeof(psp) &&
        psp.abi_version == N33_ABI_VERSION &&
        psp.size == sizeof(psp)) {
        printf("PSP13 mailbox state=%u (6=running; 0=disabled), sOS version=0x%08x, MMIO reads=%u\n",
               psp.state, psp.sos_version, psp.read_count);
    } else {
        fprintf(stderr,"N33N PSP status unavailable: 0x%x\n",psprc);
    }
    struct n33_sdma_status sdma={0};
    size_t sdma_size=sizeof(sdma);
    const kern_return_t sdmarc = IOConnectCallStructMethod(
        conn, N33_SEL_SDMA_STATUS, NULL, 0u, &sdma, &sdma_size);
    if(sdmarc==KERN_SUCCESS && sdma_size==sizeof(sdma) &&
       sdma.abi_version==N33_ABI_VERSION && sdma.size==sizeof(sdma)) {
        printf("SDMA0 state=%u (5=observed; 0=disabled), ring rptr=%u wptr=%u, checksum=0x%08x\n",
               sdma.state,sdma.ring_rptr,sdma.ring_wptr,sdma.ucode_checksum);
    } else {
        fprintf(stderr,"N33N SDMA read unavailable: 0x%x\n",sdmarc);
    }
    struct n33_dma_stage_status mapped={0};
    size_t mapped_size=sizeof(mapped);
    const kern_return_t maprc=IOConnectCallStructMethod(
        conn,N33_SEL_DMA_STATUS,NULL,0u,&mapped,&mapped_size);
    if(maprc==KERN_SUCCESS && mapped_size==sizeof(mapped) &&
       mapped.abi_version==N33_ABI_VERSION && mapped.size==sizeof(mapped)) {
        printf("N33N DMA stage state=%u (2=IOMMU STAGED only), pages=%u, GPU execution=NO\n",
               mapped.state,mapped.page_count);
    }
    IOServiceClose(conn);
    printf("GFX11 hardware command submission: %s; Metal: %s\n",
        n33_can_submit(&caps) ? "reported READY (still verify on GPU)" : "DISABLED",
        (caps.capabilities & N33_CAP_METAL) ? "reported enabled" : "DISABLED");
    return 0;
}
