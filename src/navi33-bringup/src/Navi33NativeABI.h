/* SPDX-License-Identifier: MIT
 * Separate RX 7600 user/kernel ABI: never reinterpret Navi48 N48N requests.
 * All currently accepted operations are QUERY ONLY; GPU submission is denied.
 */
#ifndef COBALT_NAVI33_NATIVE_ABI_H
#define COBALT_NAVI33_NATIVE_ABI_H
#include <stdint.h>
#ifdef __cplusplus
#define N33_STATIC_ASSERT(x,m) static_assert(x,m)
#else
#define N33_STATIC_ASSERT(x,m) _Static_assert(x,m)
#endif
#define N33_ABI_VERSION 1u
#define N33_UC_TYPE 0x4e33334eu /* N33N */
#define N33_PCI_VENDOR 0x1002u
#define N33_PCI_DEVICE 0x7480u
#define N33_GFX_IP_VERSION 0x000b0002u
#define N33_FAMILY_GFX1102 1102u
#define N33_CAP_PCI_INVENTORY (1u<<0)
#define N33_CAP_ROM_DISCOVERY (1u<<1)
#define N33_CAP_DMA_PREFLIGHT (1u<<2)
#define N33_CAP_HW_VM (1u<<16)
#define N33_CAP_HW_SDMA (1u<<17)
#define N33_CAP_HW_GFX (1u<<18)
#define N33_CAP_DCN (1u<<19)
#define N33_CAP_METAL (1u<<20)
#define N33_CAP_ACCELERATION (N33_CAP_HW_VM | N33_CAP_HW_GFX)
enum n33_selector {
  N33_SEL_HELLO=0, N33_SEL_QUERY_CAPS=1,
  N33_SEL_BO_CREATE=2, N33_SEL_BO_FREE=3, N33_SEL_VM_MAP=4,
  N33_SEL_CTX_CREATE=5, N33_SEL_SUBMIT=6, N33_SEL_WAIT=7,
  N33_SEL_PSP_STATUS=8, N33_SEL_SDMA_STATUS=9, N33_SEL_COUNT=10
};
// PSP status is deliberately read-only, never a readiness flag for GFX.
enum n33_psp_state {
  N33_PSP_DISABLED=0, N33_PSP_NO_INVENTORY=1, N33_PSP_BAD_MMIO_PLAN=2,
  N33_PSP_READ_ERROR=3, N33_PSP_INVALID_READ=4, N33_PSP_STOPPED=5,
  N33_PSP_RUNNING=6, N33_PSP_UNSTABLE=7
};
struct n33_psp_status {
  uint32_t abi_version, size, state, flags;
  uint32_t sign_of_life, sos_version, read_count, reserved0;
  uint64_t reserved1, reserved2, reserved3, reserved4;
};
N33_STATIC_ASSERT(sizeof(struct n33_psp_status)==64,"N33 PSP ABI must be 64B");
// Live SDMA6 state is an observation, never evidence of an executable ring.
enum n33_sdma_state {
  N33_SDMA_DISABLED=0, N33_SDMA_NO_INVENTORY=1,
  N33_SDMA_BAD_MMIO_PLAN=2, N33_SDMA_READ_ERROR=3,
  N33_SDMA_INVALID_READ=4, N33_SDMA_OBSERVED=5
};
struct n33_sdma_status {
  uint32_t abi_version, size, state, read_count;
  uint32_t status_register, ucode_checksum, ring_control;
  uint32_t ring_rptr, ring_wptr, reserved0;
  uint64_t reserved1, reserved2, reserved3;
};
N33_STATIC_ASSERT(sizeof(struct n33_sdma_status)==64,"N33 SDMA ABI must be 64B");
struct n33_capabilities {
  uint32_t abi_version, size, vendor_id, device_id;
  uint32_t gfx_ip_version, family, capabilities, reserved0;
  uint64_t vram_bytes, gtt_bytes, reserved1, reserved2;
};
N33_STATIC_ASSERT(sizeof(struct n33_capabilities)==64,"N33 capabilities ABI must be 64B");
static inline int n33_can_submit(const struct n33_capabilities *c) {
  return c && c->abi_version==N33_ABI_VERSION &&
    c->size==sizeof(*c) && c->vendor_id==N33_PCI_VENDOR &&
    c->device_id==N33_PCI_DEVICE &&
    c->gfx_ip_version==N33_GFX_IP_VERSION &&
    c->family==N33_FAMILY_GFX1102 &&
    (c->capabilities & N33_CAP_ACCELERATION)==N33_CAP_ACCELERATION;
}
static inline int n33_is_reserved_clean(const struct n33_capabilities *c){
  return c && !c->reserved0 && !c->reserved1 && !c->reserved2;
}
#endif
