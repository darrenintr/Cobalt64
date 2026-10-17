/* SPDX-License-Identifier: MIT
 * Navi33 real MMHUB3 framebuffer window diagnostic (READ ONLY).
 * Requires Navi33Probe.kext, admin/root and OpenCore boot arguments:
 *   navi33-probe=1 navi33-client=1 navi33-vram-read=1
 *
 * The output is NOT a VRAM allocator/free-memory estimate and MUST
 * never be used to program BAR0 or a GMC11 page table directly.
 */
#include "../src/Navi33NativeABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
static const char *why(uint32_t status) {
    switch(status) {
    case N33_VRAM_DISABLED:return "VRAM read-only opt-in disabled";
    case N33_VRAM_NO_INVENTORY:return "no checksum-valid Navi33 IP Discovery ROM";
    case N33_VRAM_UNSUPPORTED_IP:return "unsupported MMHUB IP version";
    case N33_VRAM_NO_BAR5:return "PCI BAR5 register aperture unavailable";
    case N33_VRAM_BAD_OFFSETS:return "unusable MMHUB register offsets";
    case N33_VRAM_READ_ERROR:return "cannot read BAR5 MMHUB registers";
    case N33_VRAM_UNSTABLE:return "MMHUB registers changed during read";
    case N33_VRAM_INVALID_WINDOW:return "invalid/implausible FB base/top/offset";
    case N33_VRAM_OBSERVED:return "six real MMHUB3 register reads succeeded";
    default:return "unknown state";
    }
}
int main(void) {
    if(geteuid()!=0) {
        fputs("Usage: sudo ./n33vram (root required)\n",stderr);
        return 2;
    }
    io_service_t service=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!service) {
        fputs("Navi33Probe.kext service not loaded\n",stderr);
        return 3;
    }
    io_connect_t connection=IO_OBJECT_NULL;
    kern_return_t kr=IOServiceOpen(
        service,mach_task_self(),N33_UC_TYPE,&connection);
    IOObjectRelease(service);
    if(kr!=KERN_SUCCESS) {
        fprintf(stderr,"N33N open refused: 0x%x\n",kr);
        return 4;
    }
    const uint64_t in[2]={N33_ABI_VERSION,0u};
    uint64_t hello[3]={0u,0u,0u};uint32_t n=3u;
    kr=IOConnectCallScalarMethod(connection,N33_SEL_HELLO,
                                 in,2u,hello,&n);
    if(kr!=KERN_SUCCESS || n!=3u || hello[0]!=N33_ABI_VERSION ||
       hello[1]!=N33_UC_TYPE || hello[2]!=N33_SEL_COUNT) {
        fputs("Wrong Navi33 Kext ABI; rebuild the matching CLI\n",stderr);
        IOServiceClose(connection);return 5;
    }
    struct n33_vram_geometry g={0};
    size_t length=sizeof(g);
    kr=IOConnectCallStructMethod(connection,N33_SEL_VRAM_GEOMETRY,
                                  NULL,0u,&g,&length);
    IOServiceClose(connection);
    if(kr!=KERN_SUCCESS || length!=sizeof(g) ||
       g.abi_version!=N33_ABI_VERSION || g.size!=sizeof(g) ||
       g.reserved!=0u) {
        fprintf(stderr,"N33N geometry call failed: 0x%x\n",kr);
        return 6;
    }
    printf("RX7600 MMHUB VRAM census: %s (state=%u)\n",why(g.state),g.state);
    if(g.state!=N33_VRAM_OBSERVED)return 7;
    printf("Framebuffer MC window: 0x%016" PRIx64 " .. 0x%016" PRIx64 "\n",
           g.fb_base_bytes,g.fb_top_inclusive_bytes);
    printf("FB window (NOT confirmed physical VRAM capacity): %" PRIu64 " MiB\n",
           g.fb_window_bytes/(1024u*1024u));
    printf("Separate MC FB offset: 0x%016" PRIx64 "\n",g.mc_fb_offset_bytes);
    printf("PCI BAR0 assigned aperture (NOT free VRAM): %" PRIu64 " MiB\n",
           g.assigned_bar0_bytes/(1024u*1024u));
    puts("VRAM allocated: NO; GPU queue owned: NO; MMIO writes: NO");
    return 0;
}
