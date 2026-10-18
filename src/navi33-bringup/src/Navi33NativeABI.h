/* SPDX-License-Identifier: MIT
 * Separate RX 7600 user/kernel ABI: never reinterpret Navi48 N48N requests.
 * Diagnostic queries, explicitly gated OS-IOMMU staging, and one write:
 * the boot-arg gated PSP13 sOS bootloader load (N33_SEL_PSP_LOAD).
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
#define N33_ABI_VERSION 11u /* 9: PSP_LOAD; 10: ring modes; 11: IP firmware modes */
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
  N33_SEL_VRAM_GEOMETRY=15, N33_SEL_QUEUE_CENSUS=16,
  N33_SEL_MMIO_PEEK=17, N33_SEL_PSP_LOAD=18,
  N33_SEL_COUNT=19
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
// Physical SDMA6 Queue0 observation. A quiescent register image
// NEVER grants PCI/firmware/queue ownership or a right to submit work.
enum n33_queue_census_state {
  N33_QUEUE_DISABLED=0, N33_QUEUE_NO_INVENTORY=1,
  N33_QUEUE_BAD_MMIO_PLAN=2, N33_QUEUE_READ_ERROR=3,
  N33_QUEUE_INVALID_READ=4, N33_QUEUE_UNSTABLE=5,
  N33_QUEUE_OBSERVED=6
};
#define N33_QUEUE_FLAG_ENABLED (1u<<0)
#define N33_QUEUE_FLAG_DOORBELL (1u<<1)
#define N33_QUEUE_FLAG_POINTERS (1u<<2)
#define N33_QUEUE_FLAG_RING_BASE (1u<<3)
#define N33_QUEUE_FLAG_APPARENTLY_PARKED (1u<<4)
struct n33_queue_census_status {
  uint32_t abi_version, size, state, flags;
  uint32_t ring_control, doorbell_control, ring_rptr_lo, ring_rptr_hi;
  uint32_t ring_wptr_lo, ring_wptr_hi, ring_base_lo, ring_base_hi;
  uint32_t read_count, reserved0;
  uint64_t reserved1;
};
N33_STATIC_ASSERT(sizeof(struct n33_queue_census_status)==64,
                  "N33 read-only Queue0 status must be 64 bytes");
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
// Root-only read of an allowlisted BAR5 register window (see
// Navi33PeekWindows.h). New read-only checks need a new tool, not a new
// Kext. The request is followed by rom_bytes of IP Discovery; the window
// table is per IP version and never grants a write.
#define N33_PEEK_MAX_DWORDS 64u
enum n33_peek_state {
  N33_PEEK_DISABLED=0, N33_PEEK_BAD_REQUEST=1, N33_PEEK_NOT_ALLOWED=2,
  N33_PEEK_NO_PROVIDER=3, N33_PEEK_BAD_ROM=4, N33_PEEK_NO_BAR5=5,
  N33_PEEK_MAP_FAILED=6, N33_PEEK_READ_FAILED=7, N33_PEEK_DONE=8
};
struct n33_peek_request {
  uint32_t abi_version, size, window, first;
  uint32_t count, rom_bytes;
  uint64_t reserved0;
};
N33_STATIC_ASSERT(sizeof(struct n33_peek_request)==32,
                  "N33 MMIO peek request must be 32 bytes");
struct n33_peek_result {
  uint32_t abi_version, size, state, window;
  uint32_t first, count, reads, reserved0;
  uint64_t bar5_offset, reserved1;
  uint32_t values[N33_PEEK_MAX_DWORDS];
};
N33_STATIC_ASSERT(sizeof(struct n33_peek_result)==48+4*N33_PEEK_MAX_DWORDS,
                  "N33 MMIO peek result must be 304 bytes");
/* PSP13 sOS load (Linux psp_v13_0 bootloader chain). PLAN reads and checks
 * everything and writes nothing; LOAD also needs navi33-psp-load=1 with no
 * navi33-psp-off, and runs at most once per boot. The request is followed
 * by rom_bytes of IP Discovery, then fw_bytes of psp_13_0_7_sos.bin. */
#define N33_PSP_LOAD_PLAN 1u
#define N33_PSP_LOAD_RUN 2u
/* Ring stage (after the sOS is alive): KM ring, then LOAD_TOC, then SETUP_TMR.
 * RING_PLAN reads and checks only. RING_RUN also needs navi33-psp-ring=1 with
 * no navi33-psp-off, runs at most once per boot, and stops after the step in
 * the request's stop_after (1 ring, 2 +TOC, 3 +TMR). */
#define N33_PSP_LOAD_RING_PLAN 3u
#define N33_PSP_LOAD_RING_RUN 4u
#define N33_PSP_RING_STOP_RING 1u
#define N33_PSP_RING_STOP_TOC 2u
#define N33_PSP_RING_STOP_TMR 3u
/* IP firmware stage (after the ring and TMR exist): LOAD_IP_FW for the records
 * [start_at, stop_after) of the 26-record list of Navi33Psp13IpFw.h; the input
 * after the IP Discovery blob is the eight pinned firmware files concatenated
 * in File order (their sizes are pinned, so no lengths are sent). stop_after
 * 27 also sends AUTOLOAD_RLC, which needs its own boot-arg. The SMU image is
 * never sent. FW_PLAN reads only; FW_RUN needs navi33-psp-fw=1. */
#define N33_PSP_LOAD_FW_PLAN 5u
#define N33_PSP_LOAD_FW_RUN 6u
#define N33_PSP_FW_RECORDS 26u
#define N33_PSP_FW_STOP_AUTOLOAD 27u
#define N33_PSP_LOAD_MAX_INPUT (4u*1024u*1024u)
enum n33_psp_load_state {
  N33_PSPL_DISABLED=0, N33_PSPL_BAD_REQUEST=1, N33_PSPL_NO_PROVIDER=2,
  N33_PSPL_BAD_ROM=3, N33_PSPL_BAD_FIRMWARE=4, N33_PSPL_NO_BARS=5,
  N33_PSPL_MAP_FAILED=6, N33_PSPL_READ_FAILED=7,
  N33_PSPL_SOS_ALREADY_ALIVE=8, N33_PSPL_BOOTLOADER_BUSY=9,
  N33_PSPL_BAD_FB_LOCATION=10, N33_PSPL_NO_CONSOLE=11,
  N33_PSPL_APERTURE_MISMATCH=12, N33_PSPL_NO_WINDOW=13,
  N33_PSPL_PLAN_OK=14, N33_PSPL_WRITE_DISABLED=15,
  N33_PSPL_ALREADY_ATTEMPTED=16, N33_PSPL_STAGE_VERIFY_FAILED=17,
  N33_PSPL_BOOTLOADER_TIMEOUT=18, N33_PSPL_SOS_TIMEOUT=19,
  N33_PSPL_SOS_ALIVE=20,
  /* ring stage */
  N33_PSPL_SOS_NOT_ALIVE=21, N33_PSPL_RING_NOT_READY=22,
  N33_PSPL_RING_REGS_UNEXPECTED=23, N33_PSPL_RING_CREATE_TIMEOUT=24,
  N33_PSPL_CMD_TIMEOUT=25, N33_PSPL_CMD_FAILED=26, N33_PSPL_TOC_BAD=27,
  N33_PSPL_RING_DONE=28,
  /* IP firmware stage */
  N33_PSPL_RING_MISSING=29, N33_PSPL_FW_BAD_IMAGES=30,
  N33_PSPL_AUTOLOAD_DISABLED=31, N33_PSPL_FW_DONE=32
};
/* Last step reached: KDB, then SPL, SYS_DRV, sOS (psp_hw_start order). */
enum n33_psp_load_step {
  N33_PSPL_STEP_NONE=0, N33_PSPL_STEP_KDB=1, N33_PSPL_STEP_SPL=2,
  N33_PSPL_STEP_SYS_DRV=3, N33_PSPL_STEP_SOS=4,
  N33_PSPL_STEP_RING_CREATE=5, N33_PSPL_STEP_LOAD_TOC=6,
  N33_PSPL_STEP_SETUP_TMR=7, N33_PSPL_STEP_FW_LOAD=8,
  N33_PSPL_STEP_AUTOLOAD=9
};
#define N33_PSPL_FLAG_FW_PINNED (1u<<0)
#define N33_PSPL_FLAG_CONSOLE_FOUND (1u<<1)
#define N33_PSPL_FLAG_APERTURE_MATCH (1u<<2)
#define N33_PSPL_FLAG_DCN_FB_MATCH (1u<<3)
#define N33_PSPL_FLAG_WRITE_GATE_OPEN (1u<<4)
#define N33_PSPL_FLAG_WROTE_VRAM (1u<<5)
#define N33_PSPL_FLAG_WROTE_REGISTERS (1u<<6)
struct n33_psp_load_request {
  uint32_t abi_version, size, mode, rom_bytes;
  uint32_t fw_bytes, stop_after; /* stop_after: ring and fw modes only, else 0 */
  uint64_t start_at;             /* fw modes: first record; else 0 */
};
N33_STATIC_ASSERT(sizeof(struct n33_psp_load_request)==32,
                  "N33 PSP load request must be 32 bytes");
struct n33_psp_load_result {
  uint32_t abi_version, size, state, step;
  uint32_t flags, register_writes, c2pmsg35_before, c2pmsg81_before;
  uint32_t c2pmsg35_last, c2pmsg81_after, sos_version_after, steps_done;
  uint32_t mm_fb_base, mm_fb_top, dcn_fb_base, active_pipes;
  uint64_t surface_mc[4];
  uint64_t console_phys, console_bytes, bar0_phys, bar0_bytes;
  uint64_t console_vram_offset, fw_pri_vram_offset, fw_pri_mc;
  uint64_t fw_tmr_addr;          /* fw modes: TMR address the PSP gave the last record */
  uint32_t fw_sos_version, fw_kdb_bytes, fw_spl_bytes, fw_sys_bytes;
  uint32_t fw_sos_bytes, elapsed_ms;
  uint32_t fw_last_record, fw_record_count; /* fw modes: last record sent, list size */
  /* ring stage (zero for the sOS load modes) */
  uint64_t ring_mc, cmd_mc, fence_mc, tmr_mc;
  uint32_t tmr_bytes, toc_bytes, c2pmsg64_last, c2pmsg67_last;
  uint32_t cmd_status, fence_seen, ring_flags, reserved3;
};
N33_STATIC_ASSERT(sizeof(struct n33_psp_load_result)==256,
                  "N33 PSP load result must be 256 bytes");
#define N33_PSPR_FLAG_RESUMED (1u<<0) /* ring already existed, not recreated */
#define N33_PSPR_FLAG_RING_CREATED (1u<<1)
#define N33_PSPR_FLAG_TOC_LOADED (1u<<2)
#define N33_PSPR_FLAG_TMR_SET (1u<<3)
#define N33_PSPR_FLAG_GATE_OPEN (1u<<4)
#define N33_PSPR_FLAG_FW_LOADED (1u<<5)     /* at least one record sent */
#define N33_PSPR_FLAG_AUTOLOAD_SENT (1u<<6)
#define N33_PSPR_FLAG_AUTOLOAD_GATE (1u<<7) /* navi33-psp-autoload=1 */
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
