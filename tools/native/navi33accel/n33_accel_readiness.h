// SPDX-License-Identifier: MIT
// Navi33-only readiness contract for a future Metal-facing accelerator.
// This header is pure policy: it neither publishes an IOAccelerator service
// nor touches hardware, memory mappings, queues, or Metal capabilities.
#pragma once

#include "../../../src/navi33-bringup/src/Navi33NativeABI.h"
#include <stdint.h>

#define N33_ACCEL_BACKEND_ABI 1u

// Registry metadata agreed with Navi33Metal. These keys are not currently
// published by Navi33Probe or by this contract.
#define N33_ACCEL_PROP_ABI          "Navi33,ABI"
#define N33_ACCEL_PROP_VENDOR_ID    "Navi33,VendorID"
#define N33_ACCEL_PROP_DEVICE_ID    "Navi33,DeviceID"
#define N33_ACCEL_PROP_GFX_IP       "Navi33,GfxIP"
#define N33_ACCEL_PROP_FAMILY       "Navi33,Family"
#define N33_ACCEL_PROP_CAPABILITIES "Navi33,Capabilities"
#define N33_ACCEL_PROP_READY        "Navi33,Ready"

#define N33_ACCEL_REQUIRED_CAPS \
    (N33_CAP_HW_VM | N33_CAP_HW_SDMA | N33_CAP_HW_GFX | N33_CAP_METAL)

// Every bit must describe a live, kernel-owned object or a verified hardware
// result from this same device epoch. Offline plans, user-set properties,
// DMA address preflights, PSP/RLC status and queue censuses cannot set bits.
enum n33_accel_evidence_bit {
    N33_ACCEL_EVID_DEVICE_OWNER       = 1u << 0,
    N33_ACCEL_EVID_NO_OTHER_CLIENTS   = 1u << 1,
    N33_ACCEL_EVID_PINNED_VM_ROOT     = 1u << 2,
    N33_ACCEL_EVID_VM_ROOT_INSTALLED  = 1u << 3,
    N33_ACCEL_EVID_VM_ROOT_READBACK   = 1u << 4,
    N33_ACCEL_EVID_TLB_ACK            = 1u << 5,
    N33_ACCEL_EVID_SDMA6_FIRMWARE     = 1u << 6,
    N33_ACCEL_EVID_SDMA_QUEUE_OWNER   = 1u << 7,
    N33_ACCEL_EVID_SDMA_FENCE         = 1u << 8,
    N33_ACCEL_EVID_COPY_READBACK      = 1u << 9,
    N33_ACCEL_EVID_QUEUE_PARKED       = 1u << 10,
    N33_ACCEL_EVID_MEMORY_RETAINED    = 1u << 11,
    N33_ACCEL_EVID_GFX_CONTEXT        = 1u << 12,
    N33_ACCEL_EVID_GFX_QUEUE_OWNER    = 1u << 13,
    N33_ACCEL_EVID_GFX_FENCE          = 1u << 14,
    N33_ACCEL_EVID_GFX_READBACK       = 1u << 15,
    N33_ACCEL_EVID_RECOVERY           = 1u << 16,
    N33_ACCEL_EVID_CLIENT_TRANSPORT   = 1u << 17,
    N33_ACCEL_EVID_METAL_TRANSLATION  = 1u << 18,
    N33_ACCEL_EVID_METAL_COMMANDS     = 1u << 19,
    N33_ACCEL_EVID_METAL_DRAW_READBACK = 1u << 20
};

#define N33_ACCEL_REQUIRED_EVIDENCE ((1u << 21) - 1u)

// Generation tokens bind all retained kernel-owned resources to one device
// owner without exposing physical addresses in the registry. Every epoch
// must equal device_owner_epoch; a nonzero stale epoch is not sufficient.
// reserved must remain zero for forward compatibility.
struct n33_accel_backend_evidence {
    uint32_t abi_version;
    uint32_t size;
    uint32_t evidence_bits;
    uint32_t reserved;
    uint64_t device_owner_epoch;
    uint64_t vm_root_epoch;
    uint64_t gmc_context_epoch;
    uint64_t sdma_queue_epoch;
    uint64_t gfx_queue_epoch;
    uint64_t recovery_epoch;
    uint64_t client_transport_epoch;
};

#ifdef __cplusplus
static_assert(sizeof(n33_accel_backend_evidence) == 72,
              "Navi33 accelerator evidence layout is fixed");
#else
_Static_assert(sizeof(struct n33_accel_backend_evidence) == 72,
               "Navi33 accelerator evidence layout is fixed");
#endif

static inline int n33_accel_backend_ready(
    const struct n33_capabilities *caps,
    const struct n33_accel_backend_evidence *evidence) {
    if (!caps || !evidence) return 0;
    if (!n33_can_submit(caps) || !n33_is_reserved_clean(caps)) return 0;
    if ((caps->capabilities & N33_ACCEL_REQUIRED_CAPS) !=
        N33_ACCEL_REQUIRED_CAPS) return 0;
    if (caps->vram_bytes == 0u || caps->gtt_bytes == 0u) return 0;
    if (evidence->abi_version != N33_ACCEL_BACKEND_ABI ||
        evidence->size != sizeof(*evidence) || evidence->reserved != 0u)
        return 0;
    if ((evidence->evidence_bits & N33_ACCEL_REQUIRED_EVIDENCE) !=
        N33_ACCEL_REQUIRED_EVIDENCE) return 0;
    if ((evidence->evidence_bits & ~N33_ACCEL_REQUIRED_EVIDENCE) != 0u)
        return 0;
    const uint64_t owner = evidence->device_owner_epoch;
    return owner != 0u &&
           evidence->vm_root_epoch == owner &&
           evidence->gmc_context_epoch == owner &&
           evidence->sdma_queue_epoch == owner &&
           evidence->gfx_queue_epoch == owner &&
           evidence->recovery_epoch == owner &&
           evidence->client_transport_epoch == owner;
}
