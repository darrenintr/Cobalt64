/* SPDX-License-Identifier: MIT
 * Small, host-checkable admission gate for the Navi33 Metal plug-in.
 *
 * This consumes the canonical Navi33/N33N ABI. It deliberately has no
 * Navi48 dependencies and never turns a diagnostic observation into a
 * capability. `backend_ready` must come from a concrete, running GFX11
 * allocator/VM/context/queue/submit/wait implementation.
 */
#ifndef COBALT_NAVI33_METAL_GATE_H
#define COBALT_NAVI33_METAL_GATE_H

#include "../../../src/navi33-bringup/src/Navi33NativeABI.h"

#ifdef __cplusplus
extern "C" {
#endif

enum n33m_gate_result {
    N33M_GATE_ALLOW = 0,
    N33M_GATE_NO_PROVIDER = 1,
    N33M_GATE_NO_BACKEND = 2,
    N33M_GATE_BAD_CAPS = 3,
    N33M_GATE_WRONG_DEVICE = 4,
    N33M_GATE_MISSING_HW_VM = 5,
    N33M_GATE_MISSING_HW_SDMA = 6,
    N33M_GATE_MISSING_HW_GFX = 7,
    N33M_GATE_MISSING_METAL = 8
};

static inline enum n33m_gate_result
n33m_gate_device(const struct n33_capabilities *caps,
                 int provider_ready,
                 int backend_ready) {
    if (!provider_ready) return N33M_GATE_NO_PROVIDER;
    if (!caps || !n33_is_reserved_clean(caps) ||
        caps->abi_version != N33_ABI_VERSION ||
        caps->size != sizeof(*caps))
        return N33M_GATE_BAD_CAPS;
    if (caps->vendor_id != N33_PCI_VENDOR ||
        caps->device_id != N33_PCI_DEVICE ||
        caps->gfx_ip_version != N33_GFX_IP_VERSION ||
        caps->family != N33_FAMILY_GFX1102)
        return N33M_GATE_WRONG_DEVICE;
    if (!(caps->capabilities & N33_CAP_HW_VM))
        return N33M_GATE_MISSING_HW_VM;
    if (!(caps->capabilities & N33_CAP_HW_SDMA))
        return N33M_GATE_MISSING_HW_SDMA;
    if (!(caps->capabilities & N33_CAP_HW_GFX))
        return N33M_GATE_MISSING_HW_GFX;
    if (!(caps->capabilities & N33_CAP_METAL))
        return N33M_GATE_MISSING_METAL;
    if (!backend_ready) return N33M_GATE_NO_BACKEND;
    return N33M_GATE_ALLOW;
}

static inline const char *n33m_gate_result_name(enum n33m_gate_result result) {
    switch (result) {
        case N33M_GATE_ALLOW: return "ALLOW";
        case N33M_GATE_NO_PROVIDER: return "NO_PROVIDER";
        case N33M_GATE_NO_BACKEND: return "NO_BACKEND";
        case N33M_GATE_BAD_CAPS: return "BAD_CAPS";
        case N33M_GATE_WRONG_DEVICE: return "WRONG_DEVICE";
        case N33M_GATE_MISSING_HW_VM: return "MISSING_HW_VM";
        case N33M_GATE_MISSING_HW_SDMA: return "MISSING_HW_SDMA";
        case N33M_GATE_MISSING_HW_GFX: return "MISSING_HW_GFX";
        case N33M_GATE_MISSING_METAL: return "MISSING_METAL";
    }
    return "UNKNOWN";
}

#ifdef __cplusplus
}
#endif

#endif
