#include "../src/Navi33EDID.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
using namespace navi33fb::edid;
static void checksum(uint8_t *p){
    uint8_t sum=0u;
    for(size_t i=0;i<127u;++i)sum=static_cast<uint8_t>(sum+p[i]);
    p[127]=static_cast<uint8_t>(0u-sum);
}
int main(){
    uint8_t edid[256]{0,255,255,255,255,255,255,0};
    edid[8]=0x10;edid[9]=0xac; // Dell
    edid[10]=0x34;edid[11]=0x12;
    edid[18]=1;edid[19]=4;
    checksum(edid);
    auto one=inspect(edid,128u);
    assert(one.valid()&&one.blocks==1u&&one.manufacturer==0x10acu&&one.product==0x1234u);
    assert(inspect(nullptr,128).code==Status::Missing);
    assert(inspect(edid,129).code==Status::UnsupportedLength);
    assert(inspect(edid,257).code==Status::UnsupportedLength);
    edid[0]=1;
    assert(inspect(edid,128).code==Status::BadHeader);
    edid[0]=0;edid[19]=7;
    assert(inspect(edid,128).code==Status::WrongVersion);
    edid[19]=4;edid[42]=11;
    assert(inspect(edid,128).code==Status::BadChecksum);
    edid[42]=0;
    edid[126]=1;checksum(edid);
    edid[128]=0x02u;edid[129]=0x03u;
    checksum(edid+128u);
    auto two=inspect(edid,256u);
    assert(two.valid()&&two.blocks==2u);
    edid[255]^=1u;
    assert(inspect(edid,256).code==Status::BadChecksum);
    edid[255]^=1u;
    edid[126]=0;checksum(edid);
    assert(inspect(edid,256).code==Status::WrongExtensionCount);
    std::puts("Navi33 passive OpenCore EDID/DDC validation: PASS (CPU-only)");
}
