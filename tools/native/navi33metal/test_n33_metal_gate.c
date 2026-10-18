// SPDX-License-Identifier: MIT
// Host-only tests for the pure Navi33 Metal admission policy. No IOKit,
// Metal API, device discovery, GPU memory, or hardware access is performed.
#include "Navi33MetalGate.h"

#include <stdio.h>

static unsigned checks;

static struct n33_capabilities valid_caps(void) {
    struct n33_capabilities caps = {0};
    caps.abi_version = N33_ABI_VERSION;
    caps.size = sizeof(caps);
    caps.vendor_id = N33_PCI_VENDOR;
    caps.device_id = N33_PCI_DEVICE;
    caps.gfx_ip_version = N33_GFX_IP_VERSION;
    caps.family = N33_FAMILY_GFX1102;
    caps.capabilities = N33_CAP_HW_VM | N33_CAP_HW_SDMA |
                        N33_CAP_HW_GFX | N33_CAP_METAL;
    return caps;
}

static int expect_gate(const char *name,
                       const struct n33_capabilities *caps,
                       int provider_ready,
                       int backend_ready,
                       enum n33m_gate_result expected) {
    const enum n33m_gate_result actual =
        n33m_gate_device(caps, provider_ready, backend_ready);
    checks++;
    if (actual == expected) return 0;
    fprintf(stderr, "%s: got %s, expected %s\n", name,
            n33m_gate_result_name(actual),
            n33m_gate_result_name(expected));
    return 1;
}

#define EXPECT(label, caps, provider, backend, result) \
    do { if (expect_gate((label), (caps), (provider), (backend), (result))) \
             return 1; } while (0)

int main(void) {
    struct n33_capabilities caps = valid_caps();

    EXPECT("provider absent", &caps, 0, 1, N33M_GATE_NO_PROVIDER);
    EXPECT("caps absent", NULL, 1, 1, N33M_GATE_BAD_CAPS);

    caps = valid_caps(); caps.abi_version++;
    EXPECT("ABI mismatch", &caps, 1, 1, N33M_GATE_BAD_CAPS);
    caps = valid_caps(); caps.size--;
    EXPECT("size mismatch", &caps, 1, 1, N33M_GATE_BAD_CAPS);
    caps = valid_caps(); caps.reserved0 = 1;
    EXPECT("reserved0 nonzero", &caps, 1, 1, N33M_GATE_BAD_CAPS);
    caps = valid_caps(); caps.reserved1 = 1;
    EXPECT("reserved1 nonzero", &caps, 1, 1, N33M_GATE_BAD_CAPS);
    caps = valid_caps(); caps.reserved2 = 1;
    EXPECT("reserved2 nonzero", &caps, 1, 1, N33M_GATE_BAD_CAPS);

    caps = valid_caps(); caps.vendor_id++;
    EXPECT("vendor mismatch", &caps, 1, 1, N33M_GATE_WRONG_DEVICE);
    caps = valid_caps(); caps.device_id++;
    EXPECT("device mismatch", &caps, 1, 1, N33M_GATE_WRONG_DEVICE);
    caps = valid_caps(); caps.gfx_ip_version++;
    EXPECT("GFX IP mismatch", &caps, 1, 1, N33M_GATE_WRONG_DEVICE);
    caps = valid_caps(); caps.family++;
    EXPECT("family mismatch", &caps, 1, 1, N33M_GATE_WRONG_DEVICE);

    caps = valid_caps(); caps.capabilities &= ~N33_CAP_HW_VM;
    EXPECT("HW_VM missing", &caps, 1, 1, N33M_GATE_MISSING_HW_VM);
    caps = valid_caps(); caps.capabilities &= ~N33_CAP_HW_SDMA;
    EXPECT("HW_SDMA missing", &caps, 1, 1, N33M_GATE_MISSING_HW_SDMA);
    caps = valid_caps(); caps.capabilities &= ~N33_CAP_HW_GFX;
    EXPECT("HW_GFX missing", &caps, 1, 1, N33M_GATE_MISSING_HW_GFX);
    caps = valid_caps(); caps.capabilities &= ~N33_CAP_METAL;
    EXPECT("METAL missing", &caps, 1, 1, N33M_GATE_MISSING_METAL);

    caps = valid_caps();
    EXPECT("backend not ready", &caps, 1, 0, N33M_GATE_NO_BACKEND);
    EXPECT("fully allowed policy", &caps, 1, 1, N33M_GATE_ALLOW);

    printf("Navi33 Metal admission policy: %u host-only cases passed; "
           "ALLOW is a policy result, not hardware evidence.\n", checks);
    return 0;
}
