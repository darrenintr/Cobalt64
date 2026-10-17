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
    IOServiceClose(conn);
    if (ret != KERN_SUCCESS || size != sizeof(caps) ||
        !n33_is_reserved_clean(&caps) || caps.abi_version != N33_ABI_VERSION) {
        fprintf(stderr, "N33N QUERY_CAPS failed or malformed: 0x%x\n", ret);
        return 5;
    }
    printf("N33N: %04x:%04x gfx%u; flags=0x%08x; VRAM=%llu; GTT=%llu\n",
        caps.vendor_id, caps.device_id, caps.family, caps.capabilities,
        (unsigned long long)caps.vram_bytes,
        (unsigned long long)caps.gtt_bytes);
    printf("GFX11 hardware command submission: %s; Metal: %s\n",
        n33_can_submit(&caps) ? "reported READY (still verify on GPU)" : "DISABLED",
        (caps.capabilities & N33_CAP_METAL) ? "reported enabled" : "DISABLED");
    return 0;
}
