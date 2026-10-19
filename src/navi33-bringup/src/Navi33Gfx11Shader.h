// SPDX-License-Identifier: MIT
// Pure parser for a gfx1102 AMDHSA code object (ELF64): finds one kernel's
// descriptor (".kd", LLVM AMDGPUUsage "Kernel Descriptor") and its code, and
// checks that a bare PM4 dispatch can run it: wave32, no scratch, no LDS, no
// HSA dispatch packet, the kernarg pointer as the only user SGPR input.
// Also the CPU reference of shaders/gradient.cl for checking a readback.
// No hardware access.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace navi33 { namespace gfx11_shader {

constexpr uint16_t kEmAmdgpu=224u;           // EM_AMDGPU
constexpr uint32_t kMachGfx1102=0x047u;      // EF_AMDGPU_MACH_AMDGCN_GFX1102
constexpr uint32_t kMachMask=0x0ffu;
constexpr uint32_t kSEndpgm=0xbfb00000u;     // s_endpgm (SOPP)

enum class Code : uint8_t {
    Ok=0, NotElf, NotGfx1102, NoKernel, BadDescriptor, NeedsScratch,
    NeedsLds, NeedsHsaInputs, NotWave32, NoEndpgm
};

struct Kernel {
    Code code=Code::NotElf;
    uint64_t descriptorOffset=0u, codeOffset=0u, codeBytes=0u; // file offsets
    uint32_t groupSegment=0u, privateSegment=0u, kernargBytes=0u;
    uint32_t rsrc1=0u, rsrc2=0u, rsrc3=0u;
    uint16_t properties=0u;
    uint32_t userSgprs=0u;     // COMPUTE_PGM_RSRC2.USER_SGPR
    bool tgidX=false, tgidY=false, tgidZ=false;
    uint32_t tidigComponents=0u;
};

inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0]|(p[1]<<8)); }
inline uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0])|(static_cast<uint32_t>(p[1])<<8)|
           (static_cast<uint32_t>(p[2])<<16)|(static_cast<uint32_t>(p[3])<<24);
}
inline uint64_t rd64(const uint8_t *p) {
    return static_cast<uint64_t>(rd32(p))|(static_cast<uint64_t>(rd32(p+4))<<32);
}
inline bool inside(size_t size,uint64_t off,uint64_t len) {
    return off<=size && len<=size-off;
}

// Section and symbol tables only; no relocation is needed for one kernel.
inline Kernel parse(const uint8_t *elf,size_t size,const char *name) {
    Kernel k{};
    if(!elf || size<64u || memcmp(elf,"\x7f" "ELF\x02\x01",6)!=0) return k;
    if(rd16(elf+18)!=kEmAmdgpu || (rd32(elf+48)&kMachMask)!=kMachGfx1102) {
        k.code=Code::NotGfx1102; return k;
    }
    const uint64_t shoff=rd64(elf+40);
    const uint16_t shentsize=rd16(elf+58), shnum=rd16(elf+60);
    if(shentsize!=64u || !inside(size,shoff,static_cast<uint64_t>(shnum)*64u)) return k;
    char kdName[128];
    const size_t nameLen=strlen(name);
    if(nameLen+4u>=sizeof(kdName)) { k.code=Code::NoKernel; return k; }
    memcpy(kdName,name,nameLen); memcpy(kdName+nameLen,".kd",4);
    uint64_t kdValue=0u,codeValue=0u,codeSize=0u; uint16_t kdShndx=0u,codeShndx=0u;
    for(uint16_t i=0;i<shnum;++i) {
        const uint8_t *sh=elf+shoff+static_cast<uint64_t>(i)*64u;
        if(rd32(sh+4)!=2u) continue;                    // SHT_SYMTAB
        const uint64_t off=rd64(sh+24), bytes=rd64(sh+32);
        const uint32_t link=rd32(sh+40);
        if(rd64(sh+56)!=24u || link>=shnum || !inside(size,off,bytes)) return k;
        const uint8_t *str=elf+shoff+static_cast<uint64_t>(link)*64u;
        const uint64_t strOff=rd64(str+24), strBytes=rd64(str+32);
        if(!inside(size,strOff,strBytes)) return k;
        for(uint64_t s=0;s+24u<=bytes;s+=24u) {
            const uint8_t *sym=elf+off+s;
            const uint32_t n=rd32(sym);
            if(n>=strBytes) continue;
            const char *symName=reinterpret_cast<const char *>(elf+strOff+n);
            const size_t room=static_cast<size_t>(strBytes-n);
            if(strnlen(symName,room)==room) continue;
            if(!strcmp(symName,kdName)) { kdValue=rd64(sym+8); kdShndx=rd16(sym+6); }
            if(!strcmp(symName,name)) {
                codeValue=rd64(sym+8); codeSize=rd64(sym+16); codeShndx=rd16(sym+6);
            }
        }
    }
    if(!kdShndx || !codeShndx || kdShndx>=shnum || codeShndx>=shnum) {
        k.code=Code::NoKernel; return k;
    }
    auto fileOffset=[&](uint16_t shndx,uint64_t value,uint64_t len,uint64_t &out) {
        const uint8_t *sh=elf+shoff+static_cast<uint64_t>(shndx)*64u;
        const uint64_t addr=rd64(sh+16), off=rd64(sh+24), bytes=rd64(sh+32);
        if(value<addr || value-addr>bytes || len>bytes-(value-addr)) return false;
        out=off+(value-addr);
        return inside(size,out,len);
    };
    if(!fileOffset(kdShndx,kdValue,64u,k.descriptorOffset)) {
        k.code=Code::BadDescriptor; return k;
    }
    const uint8_t *kd=elf+k.descriptorOffset;
    k.groupSegment=rd32(kd); k.privateSegment=rd32(kd+4); k.kernargBytes=rd32(kd+8);
    const int64_t entry=static_cast<int64_t>(rd64(kd+16));
    k.rsrc3=rd32(kd+44); k.rsrc1=rd32(kd+48); k.rsrc2=rd32(kd+52);
    k.properties=rd16(kd+56);
    k.userSgprs=(k.rsrc2>>1)&0x1fu;
    k.tgidX=(k.rsrc2>>7)&1u; k.tgidY=(k.rsrc2>>8)&1u; k.tgidZ=(k.rsrc2>>9)&1u;
    k.tidigComponents=(k.rsrc2>>11)&3u;
    // The descriptor's entry offset is relative to the descriptor itself.
    if(static_cast<uint64_t>(static_cast<int64_t>(kdValue)+entry)!=codeValue ||
       !codeSize || !fileOffset(codeShndx,codeValue,codeSize,k.codeOffset)) {
        k.code=Code::BadDescriptor; return k;
    }
    k.codeBytes=codeSize;
    if(k.privateSegment || (k.rsrc2&1u) || (k.properties&((1u<<0)|(1u<<5)|(1u<<6)))) {
        k.code=Code::NeedsScratch; return k;   // SCRATCH_EN, priv seg buf/size, flat scratch
    }
    if(k.groupSegment) { k.code=Code::NeedsLds; return k; }
    if(k.properties&((1u<<1)|(1u<<2)|(1u<<4))) {
        k.code=Code::NeedsHsaInputs; return k; // dispatch ptr, queue ptr, dispatch id
    }
    if(!(k.properties&(1u<<10))) { k.code=Code::NotWave32; return k; }
    bool endpgm=false;
    for(uint64_t i=0;i+4u<=codeSize;i+=4u)
        if(rd32(elf+k.codeOffset+i)==kSEndpgm) endpgm=true;
    k.code=endpgm ? Code::Ok : Code::NoEndpgm;
    return k;
}

// CPU reference of shaders/gradient.cl, for comparing a readback.
inline uint32_t gradientPixel(uint32_t x,uint32_t y,uint32_t width,
                              uint32_t height,uint32_t frame) {
    const uint32_t r=(x*255u)/width, g=(y*255u)/height, b=((x^y)+frame)&0xffu;
    return (r<<16)|(g<<8)|b;
}

} } // namespace navi33::gfx11_shader
