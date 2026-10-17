/* SPDX-License-Identifier: MIT
 * Allowlisted read-only RX 7600 register windows for N33_SEL_MMIO_PEEK,
 * shared by the Kext and tools/n33peek.c (C and C++).
 *
 * Every dword of every window is a register defined in Linux amdgpu
 * gc_11_0_0, mp_13_0_2, mmhub_3_0_2 or nbio_4_3_0 offset headers at the
 * listed BASE_IDX, with no holes. Excluded on purpose because reading
 * them is not side-effect free: *_INVALIDATE_ENG*_SEM (a read acquires a
 * VM invalidation semaphore), *_ACK, DATA/INDEX pairs, SDMA0 SCRATCH_RAM,
 * INT_STATUS, EDC counters and QUEUE_RESET_REQ. Offsets are specific to
 * the IP versions below; the Kext refuses any other version.
 */
#ifndef COBALT_NAVI33_PEEK_WINDOWS_H
#define COBALT_NAVI33_PEEK_WINDOWS_H
#include <stdint.h>

#define N33_HWID_MP1 1u
#define N33_HWID_GC 11u
#define N33_HWID_MMHUB 34u
#define N33_HWID_NBIF 108u
#define N33_HWID_MP0 255u

struct n33_peek_window {
  const char *name;
  uint16_t hwid;
  uint8_t major, minor, revision, segment;
  uint32_t first, count; /* amdgpu register dword offset, dword count */
};

#define N33_PEEK_MP0(n,f,c) {n,N33_HWID_MP0,13u,0u,7u,0u,f,c}
#define N33_PEEK_MP1(n,f,c) {n,N33_HWID_MP1,13u,0u,7u,0u,f,c}
#define N33_PEEK_GC(n,s,f,c) {n,N33_HWID_GC,11u,0u,2u,s,f,c}
#define N33_PEEK_MM(n,f,c) {n,N33_HWID_MMHUB,3u,0u,2u,0u,f,c}
#define N33_PEEK_NBIF(n,s,f,c) {n,N33_HWID_NBIF,4u,3u,1u,s,f,c}

static const struct n33_peek_window n33_peek_windows[] = {
  /* PSP and SMU mailboxes: C2PMSG_32..127 / C2PMSG_32..103 */
  N33_PEEK_MP0("PSP_C2PMSG",0x0060u,96u),
  N33_PEEK_MP1("SMU_C2PMSG",0x0260u,72u),
  /* SDMA0 (GC BASE_IDX 0) */
  N33_PEEK_GC("SDMA0_STATUS",0u,0x0025u,5u),
  N33_PEEK_GC("SDMA0_ID",0u,0x0034u,2u),
  N33_PEEK_GC("SDMA0_STATUS2",0u,0x0038u,1u),
  N33_PEEK_GC("SDMA0_Q0_RB",0u,0x0080u,7u),
  N33_PEEK_GC("SDMA0_Q0_CTL",0u,0x0088u,11u),
  N33_PEEK_GC("SDMA0_Q1_RB",0u,0x00d8u,7u),
  N33_PEEK_GC("SDMA0_Q1_CTL",0u,0x00e0u,11u),
  /* GRBM and CP status */
  N33_PEEK_GC("GRBM_STATUS2",0u,0x0da2u,1u),
  N33_PEEK_GC("GRBM_STATUS",0u,0x0da4u,4u),
  N33_PEEK_GC("CP_CPC_STATUS",0u,0x0e24u,1u),
  N33_PEEK_GC("CP_CPF_STATUS",0u,0x0e27u,1u),
  N33_PEEK_GC("CP_STAT",0u,0x0f3fu,2u),
  /* GFXHUB VM and framebuffer location */
  N33_PEEK_GC("GCMC_FB_OFFSET",0u,0x15a7u,1u),
  N33_PEEK_GC("GCVM_L2_CNTL",0u,0x15bcu,1u),
  N33_PEEK_GC("GCMC_FB_LOCATION",0u,0x1678u,2u),
  N33_PEEK_GC("GCMC_SYS_APERTURE",0u,0x167du,2u),
  N33_PEEK_GC("GCVM_CONTEXT_CNTL",0u,0x1688u,16u),
  N33_PEEK_GC("GCVM_CONTEXT_PT",0u,0x16f3u,96u),
  /* CP, IMU and RLC (GC BASE_IDX 1) */
  N33_PEEK_GC("CP_MEC_ME_CNTL",1u,0x0802u,2u),
  N33_PEEK_GC("CP_MEC_RS64_CNTL",1u,0x2904u,1u),
  N33_PEEK_GC("IMU_RLC_STATUS",1u,0x4054u,1u),
  N33_PEEK_GC("IMU_SCRATCH",1u,0x4068u,16u),
  N33_PEEK_GC("IMU_CORE_STATUS",1u,0x40b7u,1u),
  N33_PEEK_GC("IMU_GFX_RESET_CTRL",1u,0x40bcu,1u),
  N33_PEEK_GC("RLC_CNTL",1u,0x4c00u,1u),
  N33_PEEK_GC("RLC_STAT",1u,0x4c04u,1u),
  N33_PEEK_GC("RLC_GPM_STAT",1u,0x4e6bu,1u),
  N33_PEEK_GC("RLC_BOOTLOAD_STATUS",1u,0x4e82u,1u),
  /* MMHUB 3.0.2 VM and framebuffer location */
  N33_PEEK_MM("MMVM_L2_CNTL",0x0680u,1u),
  N33_PEEK_MM("MMVM_L2_FAULT_STATUS",0x068cu,1u),
  N33_PEEK_MM("MMVM_CONTEXT_CNTL",0x06c0u,16u),
  N33_PEEK_MM("MMVM_CONTEXT_PT",0x072bu,96u),
  N33_PEEK_MM("MMMC_FB_OFFSET",0x0857u,1u),
  N33_PEEK_MM("MMMC_FB_LOCATION",0x086cu,2u),
  N33_PEEK_MM("MMMC_SYS_APERTURE",0x0871u,2u),
  /* NBIO 4.3.1 RCC_CONFIG_MEMSIZE (BASE_IDX 2) */
  N33_PEEK_NBIF("NBIO_MEMSIZE",2u,0x00c3u,1u)
};
#define N33_PEEK_WINDOW_COUNT \
  ((uint32_t)(sizeof(n33_peek_windows)/sizeof(n33_peek_windows[0])))

#endif
