/* SPDX-License-Identifier: MIT
 * Separate RX 7600 user/kernel ABI: never reinterpret Navi48 N48N requests.
 * Only diagnostic queries and explicitly gated OS-IOMMU staging exist.
 * Genuine GPU execution and GPU-VM programming remain disabled.
 */
#ifndef COBALT_NAVI33_NATIVE_ABI_H
#define COBALT_NAVI33_NATIVE_ABI_H
#include <stdint.h>
#ifdef __cplusplus
#define N33_STATIC_ASSERT(x,m) static_assert(x,m)
#else
#define N33_STATIC_ASSERT(x,m) _Static_assert(x,m)
#endif
#define N33_ABI_VERSION 6u /* adds verified read-only NBIO 4.3 VRAM capacity */
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
  N33_SEL_PSP_STATUS=8, N33_SEL_SDMA_STATUS=9,
  N33_SEL_DMA_STAGE=10, N33_SEL_DMA_STATUS=11,
  N33_SEL_DMA_RELEASE=12, N33_SEL_HW_PREFLIGHT=13, N33_SEL_HW_ROM_PREFLIGHT=14,
  N33_SEL_VRAM_GEOMETRY=15,
  N33_SEL_COUNT=16
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
// Stateful N33N OS-IOMMU staging is NOT a GPU command submission.
// HW_VM, HW_SDMA and HW_GFX are never set by this operation.
enum n33_dma_stage_state {
  N33_DMA_NOT_PREPARED=0, N33_DMA_DISABLED=1, N33_DMA_STAGED=2,
  N33_DMA_BAD_PROVIDER=3, N33_DMA_NO_MAPPER=4,
  N33_DMA_NO_MEMORY=5, N33_DMA_PREPARE_FAILED=6,
  N33_DMA_COMMAND_FAILED=7, N33_DMA_SCATTER_FAILED=8,
  N33_DMA_IMAGE_FAILED=9, N33_DMA_CACHE_SYNC_FAILED=10,
  N33_DMA_BUSY=11
};
#define N33_DMA_FLAG_IOMMU_MAPPED (1u<<0)
#define N33_DMA_FLAG_CPU_IMAGE_READY (1u<<1)
#define N33_DMA_FLAG_DEVICE_SYNCED (1u<<2)
struct n33_dma_stage_status {
  uint32_t abi_version, size, state, flags;
  uint32_t page_count, segment_count, ring_dwords, reserved0;
  uint64_t source_hash, destination_hash, fence_cookie, reserved1;
};
N33_STATIC_ASSERT(sizeof(struct n33_dma_stage_status)==64,
                  "N33 DMA staging ABI must be 64B");
// Physical RX7600 state diagnostics. This never asserts 3D/Metal support.
enum n33_hw_reason {
  N33_HW_DISABLED=0, N33_HW_NO_PROVIDER=1, N33_HW_NO_ROM=2,
  N33_HW_BAD_ROM=3, N33_HW_NO_BAR5=4, N33_HW_BAD_OFFSETS=5,
  N33_HW_MAP_FAILED=6, N33_HW_MMIO_READ_FAILED=7,
  N33_HW_PSP_NOT_RUNNING=8, N33_HW_SDMA_READ_FAILED=9,
  N33_HW_SHARED_GPU_STATE=10, N33_HW_NO_OWNED_GPU_VM=11,
  N33_HW_COMPUTE_COMPLETE=12
};
#define N33_HW_FLAG_ROM_VERIFIED (1u<<0)
#define N33_HW_FLAG_BAR5_MAPPED (1u<<1)
#define N33_HW_FLAG_PSP_ALIVE (1u<<2)
#define N33_HW_FLAG_SDMA_READ (1u<<3)
#define N33_HW_FLAG_HUBS_READ (1u<<4)
#define N33_HW_FLAG_SDMA_BUSY (1u<<5)
#define N33_HW_FLAG_HUBS_ACTIVE (1u<<6)
struct n33_hw_preflight {
  uint32_t abi_version,size,reason,flags;
  uint32_t psp_state,sdma_state,psp_alive,sos_version;
  uint32_t sdma_control,sdma_rptr,sdma_wptr,mmhub_context0;
  uint32_t gfxhub_context0,bar5_kib,mmio_reads,reserved0;
};
N33_STATIC_ASSERT(sizeof(struct n33_hw_preflight)==64,
                  "N33 hardware preflight ABI must be 64 bytes");
// MMHUB+GFXHUB frame buffer registers: READ-ONLY measurements, never
// evidence of free VRAM, BO ownership, GPU DMA or Metal acceleration.
enum n33_vram_state {
  N33_VRAM_DISABLED=0, N33_VRAM_NO_ROM=1, N33_VRAM_BAD_DISCOVERY=2,
  N33_VRAM_NO_BAR5=3, N33_VRAM_BAD_OFFSETS=4, N33_VRAM_MAP_FAILED=5,
  N33_VRAM_MMIO_FAILED=6, N33_VRAM_UNSTABLE=7,
  N33_VRAM_DISAGREE=8, N33_VRAM_PAIRED_READ=9
};
#define N33_VRAM_FLAG_PAIRED (1u<<0)
#define N33_VRAM_FLAG_BAR0_ASSIGNED (1u<<1)
#define N33_VRAM_FLAG_CAPACITY_VALID (1u<<2)
#define N33_VRAM_FLAG_MC_GEOMETRY_VALID (1u<<3)
enum n33_nbio_capacity_state {
  N33_NBIO_NO_INVENTORY=0, N33_NBIO_UNSUPPORTED=1,
  N33_NBIO_MISSING_BASE=2, N33_NBIO_BAD_OFFSET=3,
  N33_NBIO_READ_FAILED=4, N33_NBIO_UNSTABLE=5,
  N33_NBIO_INVALID_CAPACITY=6, N33_NBIO_STABLE=7,
  N33_NBIO_GEOMETRY_MISMATCH=8
};
struct n33_vram_geometry_status {
  uint32_t abi_version,size,state,flags;
  uint32_t mm_fb_base,mm_fb_top,mm_fb_offset;
  uint32_t gfx_fb_base,gfx_fb_top,gfx_fb_offset;
  uint32_t bar0_mib,bar5_kib,read_count,mmhub_revision;
  uint64_t candidate_physical_fb_base;
  uint32_t nbio_state,nbio_revision,nbio_memsize_mib,nbio_reads;
  uint64_t verified_vram_bytes,verified_mc_start;
};
N33_STATIC_ASSERT(sizeof(struct n33_vram_geometry_status)==96,
                  "N33 read-only VRAM geometry status must be 96 bytes");
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
