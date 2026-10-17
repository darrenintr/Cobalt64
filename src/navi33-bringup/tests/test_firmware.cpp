#include "../src/Navi33FirmwareGuard.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace navi33::firmware;
static void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at]=static_cast<uint8_t>(x);v[at+1]=static_cast<uint8_t>(x>>8);
}
static void put32(std::vector<uint8_t>& v,size_t at,uint32_t x){
    put16(v,at,static_cast<uint16_t>(x));
    put16(v,at+2,static_cast<uint16_t>(x>>16));
}
static std::vector<uint8_t> make(Family family) {
    const auto &spec=kFamilies[static_cast<size_t>(family)];
    std::vector<uint8_t> fw(48u,0);
    put32(fw,0u,static_cast<uint32_t>(fw.size()));
    put32(fw,4u,32u);
    put16(fw,8u,1u);put16(fw,10u,0u);
    put16(fw,12u,spec.ipMajor);put16(fw,14u,spec.ipMinor);
    put32(fw,16u,0x01020304u);
    put32(fw,20u,16u);
    put32(fw,24u,32u);
    put32(fw,28u,0xdeadbeefu);
    return fw;
}
int main(){
    static_assert(!kMayLoadFirmware, "firmware parsing is metadata-only");
    for(const auto &spec:kFamilies) {
        auto b=make(spec.family);
        const Header h=inspect(b.data(),b.size(),spec.family);
        assert(h.status==Check::Pass);
        assert(h.advertisedCrc32==0xdeadbeefu);
        assert(h.ucodeVersion==0x01020304u);
        assert(h.ucodeOffset==32u&&h.ucodeSize==16u);
        assert(inspect(b.data(),b.size(),static_cast<Family>(255)).status==
               Check::UnknownFamily);
        b=make(spec.family);put16(b,12,12u);
        assert(inspect(b.data(),b.size(),spec.family).status==Check::WrongEngine);
        b=make(spec.family);put32(b,0,49u);
        assert(inspect(b.data(),b.size(),spec.family).status==Check::BadSize);
        b=make(spec.family);put32(b,4,64u);
        assert(inspect(b.data(),b.size(),spec.family).status==Check::BadHeader);
        b=make(spec.family);put32(b,20,0u);
        assert(inspect(b.data(),b.size(),spec.family).status==Check::EmptyPayload);
        b=make(spec.family);put32(b,24,47u);
        assert(inspect(b.data(),b.size(),spec.family).status==Check::OutOfRange);
        b=make(spec.family);put32(b,24,0u);
        assert(inspect(b.data(),b.size(),spec.family).status==Check::OutOfRange);
        b=make(spec.family);
        assert(inspect(b.data(),31,spec.family).status==Check::MissingBytes);
    }
    assert(inspect(nullptr,0u,Family::Gfx11).status==Check::MissingBytes);
    std::puts("Navi33 firmware headers: four families with rejection cases PASS; no firmware loaded");
}
