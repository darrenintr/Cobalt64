// SPDX-License-Identifier: MIT
// PSP13 IP firmware records for the RX 7600 (GC 11.0.2), pure and host-tested.
// Turns the eight linux-firmware images into the PSP LOAD_IP_FW records Linux
// would send, in Linux's order, with every offset and size bounds-checked.
// No I/O: nothing here can touch the card, and nothing here is wired into the
// kext yet (design: FIRMWARE_LOAD_DESIGN.md; the golden list is
// tests/fixtures/rx7600-fw-plan.tsv, produced by tools/fwplan.py).
//
// Rules copied from amdgpu_ucode_init_single_fw(), amdgpu_gfx_cp_init_microcode(),
// amdgpu_rlc.c, amdgpu_sdma.c, amdgpu_mes.c and imu_v11_0.c; the order is the
// AMDGPU_UCODE_ID enum order (psp_load_non_psp_fw). The SMU image is left out
// on the user's decision (2026-10-18), and so is AUTOLOAD_RLC: that command is
// not a record and has its own gate.
#pragma once
#include "Navi33Sha256.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace psp13ipfw {
enum class File : uint8_t { Pfp, Me, Mec, Rlc, Imu, Sdma, Mes, Mes1, Count };
constexpr size_t kFileCount=static_cast<size_t>(File::Count);
// Names of the linux-firmware files, decompressed (amdgpu/<name>.bin).
constexpr const char *kFileNames[kFileCount]={
    "gc_11_0_2_pfp","gc_11_0_2_me","gc_11_0_2_mec","gc_11_0_2_rlc",
    "gc_11_0_2_imu","sdma_6_0_2","gc_11_0_2_mes_2","gc_11_0_2_mes1"};
// The exact files this Kext accepts (linux-firmware 20260915, Ubuntu).
constexpr size_t kPinnedBytes[kFileCount]={
    234944u, 324080u, 406528u, 178848u, 132352u, 34560u, 263696u, 221408u};
constexpr sha256::Digest kPins[kFileCount]={
    {{
        0x90,0x20,0xf5,0x37,0x88,0xad,0x88,0x1f,
        0xa0,0x1a,0xa6,0x56,0xfc,0x08,0x2f,0x9f,
        0x8d,0x3c,0xdf,0xc8,0x1f,0x70,0xaa,0xac,
        0x0b,0xed,0x6e,0x60,0x01,0x49,0x11,0x28}},
    {{
        0xf5,0xfe,0x48,0xf9,0x7a,0xcb,0xd3,0xce,
        0x13,0xb3,0x59,0x29,0x29,0x0b,0xfb,0xac,
        0x01,0xce,0x52,0x26,0x31,0xcc,0x91,0xdc,
        0xef,0x1f,0xde,0xb3,0xff,0x35,0xc8,0xed}},
    {{
        0x19,0xbf,0x08,0x0d,0x6e,0x67,0x2d,0xe5,
        0xed,0x3f,0xb8,0x6e,0x3f,0xdb,0xdd,0xa4,
        0xd7,0x00,0xd8,0xe3,0xbd,0xa2,0xdb,0xdc,
        0xc9,0x23,0x10,0x14,0x84,0xad,0x64,0x5b}},
    {{
        0xb4,0x3e,0xb2,0xfd,0x06,0x00,0xf5,0x0a,
        0x1a,0x57,0x96,0xbc,0x99,0x83,0xd6,0xb3,
        0x9b,0x5c,0x20,0x96,0x02,0x34,0x92,0x0f,
        0x5e,0x89,0xcb,0x36,0x21,0x93,0xe0,0xb8}},
    {{
        0x9b,0xef,0xca,0x62,0xb0,0xb0,0xcf,0xd2,
        0x52,0xc3,0xdf,0x4a,0x9e,0xdc,0xa2,0x95,
        0x52,0x6f,0x4d,0x43,0x82,0x1c,0xd9,0x9a,
        0x63,0x26,0x45,0x49,0x95,0xa6,0xca,0x2d}},
    {{
        0x16,0xc3,0x74,0x34,0x4d,0x28,0x94,0xda,
        0x75,0x1f,0x70,0x28,0xf9,0xec,0x1f,0x75,
        0x20,0x03,0x5f,0xa9,0x54,0x8d,0x8c,0x75,
        0x0d,0x99,0xa0,0x0a,0x4a,0xfa,0x86,0xc7}},
    {{
        0x71,0x9f,0x44,0x68,0x30,0xec,0x87,0x66,
        0xf4,0x7b,0x9c,0xfc,0xf1,0xdc,0x60,0x6b,
        0x77,0xdb,0xf7,0x20,0xa9,0xe8,0x68,0xc4,
        0xcf,0xcd,0xc6,0x0e,0x07,0x67,0xd1,0xa7}},
    {{
        0x5f,0x8f,0x0b,0x50,0xb7,0xa9,0xf8,0xc3,
        0x4f,0x68,0xee,0x51,0x6a,0x51,0x96,0x8e,
        0xd1,0x90,0xcb,0x0d,0x0f,0xb3,0x6c,0x99,
        0xe5,0x30,0x17,0x99,0xdd,0x65,0x09,0x7c}}
};

struct Image { const uint8_t *data=nullptr; size_t size=0u; };
struct Images { Image f[kFileCount]; };

// psp_gfx_if.h enum psp_gfx_fw_type.
constexpr uint32_t kTypeSdmaTh0=71u, kTypeSdmaTh1=72u;
constexpr uint32_t kTypeRs64Pfp=87u, kTypeRs64Me=88u, kTypeRs64Mec=89u;
constexpr uint32_t kTypePfpP0=90u, kTypePfpP1=91u, kTypeMeP0=92u, kTypeMeP1=93u;
constexpr uint32_t kTypeMecP0=94u, kTypeMecP1=95u, kTypeMecP2=96u, kTypeMecP3=97u;
constexpr uint32_t kTypeMes=33u, kTypeMesStack=34u, kTypeMesKiq=81u, kTypeMesKiqStack=82u;
constexpr uint32_t kTypeImuI=68u, kTypeImuD=69u;
constexpr uint32_t kTypeRlcCntl=22u, kTypeRlcGpm=20u, kTypeRlcSrm=21u;
constexpr uint32_t kTypeRlcIram=26u, kTypeRlcDram=48u, kTypeRlcP=25u, kTypeRlcV=7u, kTypeRlcG=8u;
constexpr uint32_t kCmdLoadIpFw=0x6u, kCmdAutoloadRlc=0x21u; // GFX_CMD_ID_*
constexpr uint32_t kPage=4096u;

struct Record {
    uint8_t order=0u;          // AMDGPU_UCODE_ID enum value (the sort key)
    const char *name=nullptr;
    uint32_t type=0u;          // GFX_FW_TYPE_*
    File file=File::Pfp;
    uint32_t offset=0u, size=0u; // inside that file
};
constexpr uint32_t kMaxRecords=32u;
struct List {
    Record rec[kMaxRecords];
    uint32_t count=0u;
};

namespace detail {
struct Hdr {                       // common_firmware_header, 32 bytes
    uint32_t size, headerSize; uint16_t major, minor, ipMajor, ipMinor;
    uint32_t version, ucodeSize, arrayOff, crc;
};
inline bool rd32(const Image &im,uint64_t off,uint32_t *out) {
    if(!im.data || off+4u>im.size)return false;
    *out=static_cast<uint32_t>(im.data[off])|static_cast<uint32_t>(im.data[off+1])<<8u|
         static_cast<uint32_t>(im.data[off+2])<<16u|static_cast<uint32_t>(im.data[off+3])<<24u;
    return true;
}
inline bool hdr(const Image &im,Hdr &h,uint16_t major,uint16_t minor,
                uint16_t ipMajor,uint32_t minHeader) {
    uint32_t a[8];
    for(unsigned i=0;i<8u;++i)if(!rd32(im,4u*i,&a[i]))return false;
    h.size=a[0];h.headerSize=a[1];
    h.major=static_cast<uint16_t>(a[2]);h.minor=static_cast<uint16_t>(a[2]>>16u);
    h.ipMajor=static_cast<uint16_t>(a[3]);h.ipMinor=static_cast<uint16_t>(a[3]>>16u);
    h.version=a[4];h.ucodeSize=a[5];h.arrayOff=a[6];h.crc=a[7];
    return h.size==im.size && h.major==major && h.minor==minor &&
           h.ipMajor==ipMajor && h.headerSize>=minHeader && h.headerSize<=im.size &&
           h.arrayOff>=h.headerSize;
}
// An image [off, off+size) must lie after the header and inside the file.
inline bool inside(const Image &im,const Hdr &h,uint32_t off,uint32_t size) {
    return size && off>=h.headerSize && static_cast<uint64_t>(off)+size<=im.size;
}
inline bool add(List &l,uint8_t order,const char *name,uint32_t type,File f,
                const Image &im,const Hdr &h,uint32_t off,uint32_t size) {
    if(l.count>=kMaxRecords || !inside(im,h,off,size))return false;
    l.rec[l.count++]={order,name,type,f,off,size};
    return true;
}
} // namespace detail

inline bool pinned(File f,const Image &im) {
    const size_t i=static_cast<size_t>(f);
    return i<kFileCount && im.data && im.size==kPinnedBytes[i] &&
           sha256::equal(sha256::hash(im.data,im.size),kPins[i]);
}
inline bool allPinned(const Images &in) {
    for(size_t i=0;i<kFileCount;++i)
        if(!pinned(static_cast<File>(i),in.f[i]))return false;
    return true;
}

// The records, sorted by AMDGPU_UCODE_ID. False if any header, offset or size
// is out of bounds or any header version differs from the one this code knows.
inline bool build(const Images &in,List &out) {
    using namespace detail;
    out=List{};
    Hdr h{};
    uint32_t a=0u,b=0u,c=0u,d=0u,e=0u;
    // SDMA 6.0.2: header v2.0 (64 B). TH0 = ctx_ucode_size at the array offset;
    // TH1 = ctl_ucode_size at ctl_ucode_offset.
    const Image &sd=in.f[static_cast<size_t>(File::Sdma)];
    if(!hdr(sd,h,2,0,6,64u) || !rd32(sd,36u,&a) || !rd32(sd,48u,&b) || !rd32(sd,52u,&c) ||
       !add(out,9,"SDMA_UCODE_TH0",kTypeSdmaTh0,File::Sdma,sd,h,h.arrayOff,a) ||
       !add(out,10,"SDMA_UCODE_TH1",kTypeSdmaTh1,File::Sdma,sd,h,b,c))return false;
    // CP (RS64), gfx_firmware_header_v2_0: ucode size/offset @36/@40, data
    // size/offset @44/@48. The stacks are the same data image, sent per stack.
    struct Cp { File f; uint8_t order; const char *name; uint32_t type;
                uint8_t nStacks; uint8_t stackOrder; const char *stackName[4];
                uint32_t stackType[4]; };
    const Cp cps[3]={
        {File::Pfp,15,"CP_RS64_PFP",kTypeRs64Pfp,2,18,
         {"CP_RS64_PFP_P0_STACK","CP_RS64_PFP_P1_STACK",nullptr,nullptr},
         {kTypePfpP0,kTypePfpP1,0u,0u}},
        {File::Me,16,"CP_RS64_ME",kTypeRs64Me,2,20,
         {"CP_RS64_ME_P0_STACK","CP_RS64_ME_P1_STACK",nullptr,nullptr},
         {kTypeMeP0,kTypeMeP1,0u,0u}},
        {File::Mec,17,"CP_RS64_MEC",kTypeRs64Mec,4,22,
         {"CP_RS64_MEC_P0_STACK","CP_RS64_MEC_P1_STACK","CP_RS64_MEC_P2_STACK","CP_RS64_MEC_P3_STACK"},
         {kTypeMecP0,kTypeMecP1,kTypeMecP2,kTypeMecP3}}};
    for(const Cp &cp:cps) {
        const Image &im=in.f[static_cast<size_t>(cp.f)];
        if(!hdr(im,h,2,0,11,60u) || !rd32(im,36u,&a) || !rd32(im,44u,&c) ||
           !rd32(im,48u,&d) ||
           !add(out,cp.order,cp.name,cp.type,cp.f,im,h,h.arrayOff,a))return false;
        for(uint8_t s=0;s<cp.nStacks;++s)
            if(!add(out,static_cast<uint8_t>(cp.stackOrder+s),cp.stackName[s],
                    cp.stackType[s],cp.f,im,h,d,c))return false;
    }
    // MES v1.0: the scheduler pipe image is gc_11_0_2_mes_2, the KIQ pipe mes1.
    // ucode size/offset @36/@40, data size/offset @48/@52.
    struct Mes { File f; uint8_t order; const char *n0,*n1; uint32_t t0,t1; };
    const Mes mes[2]={{File::Mes,30,"CP_MES","CP_MES_DATA",kTypeMes,kTypeMesStack},
                      {File::Mes1,32,"CP_MES1","CP_MES1_DATA",kTypeMesKiq,kTypeMesKiqStack}};
    for(const Mes &m:mes) {
        const Image &im=in.f[static_cast<size_t>(m.f)];
        if(!hdr(im,h,1,0,11,72u) || !rd32(im,36u,&a) || !rd32(im,40u,&b) ||
           !rd32(im,48u,&c) || !rd32(im,52u,&d) ||
           !add(out,m.order,m.n0,m.t0,m.f,im,h,b,a) ||
           !add(out,static_cast<uint8_t>(m.order+1u),m.n1,m.t1,m.f,im,h,d,c))return false;
    }
    // IMU v1.0: IRAM size @32 then DRAM size @40; IRAM at the array offset, DRAM after it.
    const Image &imu=in.f[static_cast<size_t>(File::Imu)];
    if(!hdr(imu,h,1,0,11,48u) || !rd32(imu,32u,&a) || !rd32(imu,40u,&b) ||
       !add(out,34,"IMU_I",kTypeImuI,File::Imu,imu,h,h.arrayOff,a) ||
       h.arrayOff>UINT32_MAX-a ||
       !add(out,35,"IMU_D",kTypeImuD,File::Imu,imu,h,h.arrayOff+a,b))return false;
    // RLC v2.3 (204 B). v2_0 is 32 + 18 dwords; v2_1 adds direct_reg_list_length
    // and {ucode_ver, feature_ver, size, offset} for CNTL, GPM, SRM; v2_2 IRAM/DRAM
    // {size, offset}; v2_3 RLC_P / RLC_V {ver, feature_ver, size, offset}. All
    // offsets are from the file start. An empty restore list is skipped.
    const Image &rlc=in.f[static_cast<size_t>(File::Rlc)];
    if(!hdr(rlc,h,2,3,11,204u))return false;
    const uint32_t b21=32u+18u*4u+4u;
    const struct { uint8_t order; const char *name; uint32_t type; } sr[3]={
        {41,"RLC_RESTORE_LIST_CNTL",kTypeRlcCntl},
        {42,"RLC_RESTORE_LIST_GPM_MEM",kTypeRlcGpm},
        {43,"RLC_RESTORE_LIST_SRM_MEM",kTypeRlcSrm}};
    for(uint32_t n=0;n<3u;++n) {
        if(!rd32(rlc,b21+16u*n+8u,&a) || !rd32(rlc,b21+16u*n+12u,&b))return false;
        if(a && !add(out,sr[n].order,sr[n].name,sr[n].type,File::Rlc,rlc,h,b,a))return false;
    }
    const uint32_t b22=b21+48u, b23=b22+16u;
    if(!rd32(rlc,b22,&a) || !rd32(rlc,b22+4u,&b) || !rd32(rlc,b22+8u,&c) ||
       !rd32(rlc,b22+12u,&d) ||
       !add(out,44,"RLC_IRAM",kTypeRlcIram,File::Rlc,rlc,h,b,a) ||
       !add(out,45,"RLC_DRAM",kTypeRlcDram,File::Rlc,rlc,h,d,c))return false;
    if(!rd32(rlc,b23+8u,&a) || !rd32(rlc,b23+12u,&b) || !rd32(rlc,b23+24u,&c) ||
       !rd32(rlc,b23+28u,&d) ||
       !add(out,48,"RLC_P",kTypeRlcP,File::Rlc,rlc,h,b,a) ||
       !add(out,49,"RLC_V",kTypeRlcV,File::Rlc,rlc,h,d,c) ||
       !add(out,50,"RLC_G",kTypeRlcG,File::Rlc,rlc,h,h.arrayOff,h.ucodeSize))return false;
    (void)e;
    // Insertion sort by enum order (stable; at most 32 records).
    for(uint32_t i=1;i<out.count;++i)
        for(uint32_t j=i;j>0u && out.rec[j-1u].order>out.rec[j].order;--j) {
            const Record t=out.rec[j];out.rec[j]=out.rec[j-1u];out.rec[j-1u]=t;
        }
    return true;
}
// Page-aligned size of a record in the staging buffer (Linux ALIGNs to PAGE_SIZE).
inline uint32_t stagedBytes(const Record &r) {
    return (r.size+kPage-1u)/kPage*kPage;
}
inline uint64_t stagingTotal(const List &l) {
    uint64_t t=0u;
    for(uint32_t i=0;i<l.count;++i)t+=stagedBytes(l.rec[i]);
    return t;
}
// Byte offset of record i inside the staging buffer, in list order.
inline uint64_t stagingOffset(const List &l,uint32_t i) {
    uint64_t t=0u;
    for(uint32_t k=0;k<i && k<l.count;++k)t+=stagedBytes(l.rec[k]);
    return t;
}
} } // namespace navi33::psp13ipfw
