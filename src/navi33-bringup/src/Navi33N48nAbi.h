/* SPDX-License-Identifier: MIT
 * The N48N user-client ABI v1.0 subset (selectors 0..8) as the Navi33 kext serves it, so
 * RADV's Darwin backend (mesa-patches/0002) can reach the RX 7600. A byte-identical copy
 * of the definitions in src/navi48-bringup/src/Navi48NativeABI.h (Navi33 shares no Navi48
 * source); the layout asserts below pin every offset Mesa uses. Each struct equals its
 * amdgpu DRM twin. Plain C11 / C++17, stdint only. */
#ifndef NAVI33_N48N_ABI_H
#define NAVI33_N48N_ABI_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
#define N33_N48N_ASSERT(c,m) static_assert(c,m)
#else
#define N33_N48N_ASSERT(c,m) _Static_assert(c,m)
#endif
#define N48N_UC_TYPE 0x4E34384Eu          /* 'N48N' */
#define N48N_ABI_VERSION 1u
#define N48N_MAX_BOS 4096u
#define N48N_WAIT_CAP_NS 2000000000ull
#define N48N_SEQ_LAST 0xFFFFFFFFFFFFFFFFull
enum {
    N48N_SEL_HELLO=0,      /* in [0] ABI (1) [1] flags (0)   out [0] ABI [1] build [2] max IBs [3] VMID */
    N48N_SEL_QUERYINFO=1,  /* struct out n48n_info (192 B) */
    N48N_SEL_READREGS=2,   /* in [0] absolute dword [1] count 1..16 [2] instance   struct out uint32_t[count] */
    N48N_SEL_BOCREATE=3,   /* struct in n48n_gem_create_in   out [0] handle [1] size [2] domain [3] placed bits */
    N48N_SEL_BOFREE=4,     /* in [0] handle */
    N48N_SEL_GEMVA=5,      /* struct in n48n_gem_va */
    N48N_SEL_CTX=6,        /* struct in/out n48n_ctx */
    N48N_SEL_SUBMIT=7,     /* struct in n48n_cs_in + IBs   out [0] seqno */
    N48N_SEL_WAITSEQ=8,    /* in [0] seqno [1] timeout ns [2] ctx   out [0] busy [1] retired [2] emitted */
    N48N_SEL_COUNT=9
};
#define N48N_GEM_DOMAIN_CPU 0x1u
#define N48N_GEM_DOMAIN_GTT 0x2u
#define N48N_GEM_DOMAIN_VRAM 0x4u
#define N48N_GEM_CPU_ACCESS_REQUIRED (1ull<<0)
#define N48N_GEM_NO_CPU_ACCESS (1ull<<1)
#define N48N_GEM_CPU_GTT_USWC (1ull<<2)
#define N48N_GEM_VRAM_CLEARED (1ull<<3)
#define N48N_GEM_VRAM_CONTIGUOUS (1ull<<5)
#define N48N_GEM_VM_ALWAYS_VALID (1ull<<6)
#define N48N_GEM_EXPLICIT_SYNC (1ull<<7)
#define N48N_GEM_CP_MQD_GFX9 (1ull<<8)
#define N48N_GEM_ENCRYPTED (1ull<<10)
#define N48N_GEM_PREEMPTIBLE (1ull<<11)
#define N48N_GEM_DISCARDABLE (1ull<<12)
#define N48N_GEM_UNCACHED (1ull<<14)
#define N48N_GEM_GFX12_DCC (1ull<<16)
#define N48N_GEM_VIRTIO_SHARED (1ull<<31)
#define N48N_VA_OP_MAP 1u
#define N48N_VA_OP_UNMAP 2u
#define N48N_VM_DELAY_UPDATE (1u<<0)
#define N48N_VM_PAGE_READABLE (1u<<1)
#define N48N_VM_PAGE_WRITEABLE (1u<<2)
#define N48N_VM_PAGE_EXECUTABLE (1u<<3)
#define N48N_VM_PAGE_PRT (1u<<4)
#define N48N_VM_MTYPE_MASK (0xfu<<5)
#define N48N_VM_PAGE_NOALLOC (1u<<9)
#define N48N_CTX_OP_ALLOC 1u
#define N48N_CTX_OP_FREE 2u
#define N48N_CTX_OP_QUERY_STATE2 4u
#define N48N_CTX_QUERY2_RESET (1ull<<0)
#define N48N_CTX_QUERY2_GUILTY (1ull<<2)
#define N48N_HW_IP_GFX 0u
#define N48N_HW_IP_COMPUTE 1u              /* amdgpu_drm.h AMDGPU_HW_IP_COMPUTE */
#define N48N_IB_FLAG_CE (1u<<0)
#define N48N_IB_FLAG_PREAMBLE (1u<<1)
#define N48N_IB_FLAG_PREEMPT (1u<<2)
#define N48N_IB_FLAG_TC_WB_NOT_INVALIDATE (1u<<3)
#define N48N_CS_HAS_FENCE (1u<<0)
#define N48N_PLACED_CPU_MAPPABLE (1u<<0)
#define N48N_PLACED_ZEROED (1u<<1)
#define N48N_INFO_HUNG (1u<<0)
struct n48n_gem_create_in { uint64_t bo_size, alignment, domains, domain_flags; };
struct n48n_gem_va {
    uint32_t handle, _pad, operation, flags;
    uint64_t va_address, offset_in_bo, map_size;
    uint64_t vm_timeline_point;
    uint32_t vm_timeline_syncobj_out, num_syncobj_handles;
    uint64_t input_fence_syncobj_handles;
};
struct n48n_ctx { uint32_t op, flags, ctx_id; int32_t priority; };
struct n48n_cs_ib { uint32_t _pad, flags; uint64_t va_start; uint32_t ib_bytes, ip_type, ip_instance, ring; };
struct n48n_cs_in { uint32_t abi, ctx_id, num_ibs, flags, fence_handle, fence_offset; uint64_t reserved; };
struct n48n_info {
    uint32_t abi_version, kext_build, flags, vmid;
    uint32_t gb_addr_config, gc_version, ring_size_dw, max_ibs;
    uint64_t vram_vis_total, vram_vis_free;
    uint64_t vram_hi_total, vram_hi_free;
    uint64_t gtt_cap, gtt_used, gtt_max_bo;
    uint64_t pt_cap, pt_used;
    uint64_t seq_emitted, seq_retired;
    uint64_t va_low_first, va_low_last;
    uint64_t va_high_first, va_high_last;
    uint32_t max_bos, fence_slots;
    uint32_t reserved[8];
};
N33_N48N_ASSERT(sizeof(struct n48n_gem_create_in)==32,"gem_create_in 32 B");
N33_N48N_ASSERT(sizeof(struct n48n_gem_va)==64 && offsetof(struct n48n_gem_va,va_address)==16 &&
                offsetof(struct n48n_gem_va,map_size)==32 && offsetof(struct n48n_gem_va,input_fence_syncobj_handles)==56,
                "gem_va 64 B, drm_amdgpu_gem_va layout");
N33_N48N_ASSERT(sizeof(struct n48n_ctx)==16,"ctx 16 B");
N33_N48N_ASSERT(sizeof(struct n48n_cs_ib)==32 && offsetof(struct n48n_cs_ib,va_start)==8 &&
                offsetof(struct n48n_cs_ib,ip_type)==20,"cs_ib 32 B, drm_amdgpu_cs_chunk_ib layout");
N33_N48N_ASSERT(sizeof(struct n48n_cs_in)==32 && offsetof(struct n48n_cs_in,fence_offset)==20,"cs_in header 32 B");
N33_N48N_ASSERT(sizeof(struct n48n_info)==192 && offsetof(struct n48n_info,vram_vis_total)==32 &&
                offsetof(struct n48n_info,gtt_cap)==64 && offsetof(struct n48n_info,va_low_first)==120 &&
                offsetof(struct n48n_info,max_bos)==152,"n48n_info 192 B");
#endif
