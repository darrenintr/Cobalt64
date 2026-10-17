#include "../src/Navi33Psp13Firmware.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using namespace navi33::psp13fw;
static void w16(std::vector<uint8_t> &v,size_t p,uint16_t x) {
    v[p]=static_cast<uint8_t>(x);v[p+1]=static_cast<uint8_t>(x>>8u);
}
static void w32(std::vector<uint8_t> &v,size_t p,uint32_t x) {
    w16(v,p,static_cast<uint16_t>(x));w16(v,p+2u,static_cast<uint16_t>(x>>16u));
}
static std::vector<uint8_t> make(unsigned minor=0u) {
    std::vector<uint8_t> data(144u,0u);
    const unsigned first=minor ? 40u:36u;
    w32(data,0u,static_cast<uint32_t>(data.size())); // full image
    w32(data,4u,static_cast<uint32_t>(first+32u));
    w16(data,8u,2u);w16(data,10u,static_cast<uint16_t>(minor));
    w16(data,12u,13u);w16(data,14u,0u);
    w32(data,16u,0x01020304u);
    w32(data,20u,16u);w32(data,24u,128u);
    w32(data,32u,2u);if(minor)w32(data,36u,1u);
    // Descriptor offsets count from ucode_array_offset (128), as upstream.
    w32(data,first,1u);w32(data,first+4u,0x77u);
    w32(data,first+8u,0u);w32(data,first+12u,8u);
    w32(data,first+16u,8u);w32(data,first+20u,0x88u);
    w32(data,first+24u,8u);w32(data,first+28u,8u);
    for(unsigned i=128u;i<144u;++i)data[i]=static_cast<uint8_t>(i);
    w32(data,28u,crc32(data.data()+128u,16u));
    return data;
}
int main() {
    const auto base=make();
    const auto good=inspect(base.data(),base.size());
    assert(good.code==Code::Pass);
    assert(good.descriptorCount==2u && good.auxiliaryIndex==UINT32_MAX);
    assert(good.component[0].type==1u && good.component[0].offset==128u);
    assert(good.component[1].offset==136u); // reported from file start
    assert(find(good,1u)==&good.component[0] && !find(good,kKdb));
    auto newer=make(1u);
    assert(inspect(newer.data(),newer.size()).code==Code::Pass);
    w32(newer,36u,2u);
    assert(inspect(newer.data(),newer.size()).code==Code::OutOfBounds);
    // Linux never checks the header crc32; it is reported, not a gate.
    auto v=base;v[128u]^=0x42u;
    const auto bent=inspect(v.data(),v.size());
    assert(bent.code==Code::Pass && bent.computedCrc32!=bent.advertisedCrc32);
    // The old absolute-offset reading (128) now runs past the ucode array.
    v=base;w32(v,36u+8u,128u);
    assert(inspect(v.data(),v.size()).code==Code::OutOfBounds);
    v=base;w16(v,8u,3u);
    assert(inspect(v.data(),v.size()).code==Code::UnsupportedVersion);
    v=base;w16(v,12u,14u);
    assert(inspect(v.data(),v.size()).code==Code::CommonHeader);
    v=base;w32(v,32u,999u);
    assert(inspect(v.data(),v.size()).code==Code::DescriptorCount);
    v=base;w32(v,36u,0u); // descriptor zero type
    assert(inspect(v.data(),v.size()).code==Code::OutOfBounds);
    v=base;w32(v,36u+16u,1u); // duplicate component kind
    assert(inspect(v.data(),v.size()).code==Code::DuplicateType);
    v=base;w32(v,36u+16u+8u,4u); // overlapping byte ranges
    assert(inspect(v.data(),v.size()).code==Code::OverlappingComponents);
    v=base;w32(v,36u+8u,0xfffffff0u); // overrun
    assert(inspect(v.data(),v.size()).code==Code::OutOfBounds);
    // Real containers give the fixed header size only (36 for v2.0) and
    // put the descriptors after it; that must parse.
    v=base;w32(v,4u,36u);
    assert(inspect(v.data(),v.size()).code==Code::Pass);
    v=base;w32(v,4u,36u);w32(v,24u,60u); // descriptors run into the ucode
    assert(inspect(v.data(),v.size()).code==Code::OutOfBounds);
    v=base;v.resize(130u);
    assert(inspect(v.data(),v.size()).code==Code::CommonHeader);
    assert(inspect(nullptr,0u).code==Code::CommonHeader);
    // Known CRC32-IEEE vector
    const uint8_t nine[]={'1','2','3','4','5','6','7','8','9'};
    assert(crc32(nine,9u)==0xcbf43926u);
    static_assert(!kMayBootPsp,"parser only");
    std::puts("PSP13 v2.0/v2.1 component manifest and corruption tests passed");
    return 0;
}
