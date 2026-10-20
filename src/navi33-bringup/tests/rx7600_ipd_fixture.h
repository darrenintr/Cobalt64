// SPDX-License-Identifier: MIT
// IP Discovery table Linux amdgpu parsed from a physical RX 7600
// (1002:7480 rev cf, 1 MiB BAR5), rebuilt as a binary blob for host tests;
// see tests/fixtures/rx7600-linux-ipd.txt.
#pragma once
#include "../src/Navi33Discovery.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace navi33 { namespace rx7600_fixture {
constexpr uint64_t kBar5=0x100000u;
struct Ip { uint16_t id; uint8_t maj,min,rev; std::vector<uint32_t> bases; };
const Ip kRx7600[]={
    {ipid::GC,    11,0,2,{0x00001260u,0x0000A000u,0x0001C000u,0x02402C00u}},
    {ipid::MP0,   13,0,7,{0x00016000u,0x00DC0000u,0x00E00000u,0x00E40000u,0x0243FC00u}},
    {ipid::MP1,   13,0,7,{0x00016000u,0x00DC0000u,0x00E00000u,0x00E40000u,0x0243FC00u}},
    {ipid::SDMA0,  6,0,2,{0x00001260u,0x0000A000u,0x0001C000u,0x02402C00u}},
    {ipid::SDMA1,  6,0,2,{0x00001260u,0x0000A000u,0x0001C000u,0x02402C00u}},
    {ipid::DMU,    3,2,1,{0x00000012u,0x000000C0u,0x000034C0u,0x00009000u,0x02403C00u}},
    {ipid::MMHUB,  3,0,2,{0x0001A000u,0x02408800u}},
    {ipid::NBIF,   4,3,1,{0x00000000u,0x00000014u,0x00000D20u,0x00010400u,0x0241B000u,0x04040000u}},
    {ipid::HDP,    6,0,1,{0x00000F20u,0x0240A400u}},
    {ipid::OSSSYS, 6,0,2,{0x000010A0u,0x0240A000u}},
};

inline void put16(std::vector<uint8_t> &v,size_t p,uint16_t x) {
    v[p]=static_cast<uint8_t>(x);
    v[p+1]=static_cast<uint8_t>(x>>8);
}
inline void put32(std::vector<uint8_t> &v,size_t p,uint32_t x) {
    put16(v,p,static_cast<uint16_t>(x));
    put16(v,p+2,static_cast<uint16_t>(x>>16));
}
inline uint16_t sum(const std::vector<uint8_t> &v,size_t a,size_t b) {
    uint16_t c=0;
    for(size_t i=a;i<b;++i)c=static_cast<uint16_t>(c+v[i]);
    return c;
}
// Same v1 binary header / v3 IPDS layout as test_hardware_plan.cpp.
// Optional revisions let focused plans exercise exact IP-version refusals.
inline std::vector<uint8_t> blob(uint8_t mmhubRevision=2u,uint8_t nbifRevision=1u,uint8_t hdpRevision=1u) {
    std::vector<uint8_t> v(1024,0);
    put32(v,0,0x28211407u);
    put16(v,4,1);
    put16(v,12,128);
    put32(v,128,0x53445049u);
    put16(v,132,3);
    put16(v,140,1);
    put16(v,144,208);
    put16(v,208,0);
    put16(v,210,static_cast<uint16_t>(sizeof(kRx7600)/sizeof(kRx7600[0])));
    size_t p=212;
    for(const Ip &ip:kRx7600) {
        put16(v,p,ip.id);
        v[p+2]=0;
        v[p+3]=static_cast<uint8_t>(ip.bases.size());
        v[p+4]=ip.maj;v[p+5]=ip.min;
        v[p+6]=ip.id==ipid::MMHUB ? mmhubRevision : ip.id==ipid::NBIF ? nbifRevision : ip.id==ipid::HDP ? hdpRevision : ip.rev;v[p+7]=0;
        p+=8;
        for(uint32_t b:ip.bases){put32(v,p,b);p+=4;}
    }
    put16(v,134,static_cast<uint16_t>(p-128));
    put16(v,16,static_cast<uint16_t>(p-128));
    put16(v,10,static_cast<uint16_t>(p));
    put16(v,14,sum(v,128,p));
    put16(v,8,sum(v,10,p));
    v.resize(p);
    return v;
}
} } // namespace navi33::rx7600_fixture
