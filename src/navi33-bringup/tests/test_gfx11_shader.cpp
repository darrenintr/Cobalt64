// Host-only checks of the committed gfx1102 code object shaders/gradient.co
// (rebuilt by shaders/build.sh). No GPU access.
#include "../src/Navi33Gfx11Shader.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace navi33::gfx11_shader;

namespace {
std::vector<uint8_t> load(const char *path) {
    std::vector<uint8_t> b;
    FILE *f=std::fopen(path,"rb");
    assert(f && "run from src/navi33-bringup");
    for(int c;(c=std::fgetc(f))!=EOF;) b.push_back(static_cast<uint8_t>(c));
    std::fclose(f);
    return b;
}
void put32(std::vector<uint8_t> &b,size_t off,uint32_t v) {
    for(int i=0;i<4;++i) b[off+i]=static_cast<uint8_t>(v>>(8*i));
}
} // namespace

int main() {
    const std::vector<uint8_t> co=load("shaders/gradient.co");
    const Kernel k=parse(co.data(),co.size(),"cobalt_gradient");
    assert(k.code==Code::Ok);
    // What a bare PM4 dispatch has to program, straight from the descriptor.
    assert(k.kernargBytes==24u && !k.groupSegment && !k.privateSegment);
    assert(k.rsrc1==0xe0af0001u && k.rsrc2==0x0000099cu && k.rsrc3==0x40u);
    assert(k.properties==0x0408u);           // kernarg ptr + wave32 only
    assert(k.userSgprs==14u && k.tgidX && k.tgidY && !k.tgidZ);
    assert(k.tidigComponents==1u);           // x and y work-item IDs
    assert(k.descriptorOffset==0x640u && k.codeOffset==0x700u && k.codeBytes==456u);
    assert(parse(co.data(),co.size(),"nope").code==Code::NoKernel);
    // Refusals on edited copies.
    std::vector<uint8_t> b=co;
    put32(b,48,0x046u);                      // e_flags: gfx1101
    assert(parse(b.data(),b.size(),"cobalt_gradient").code==Code::NotGfx1102);
    b=co; put32(b,k.descriptorOffset+4,256u); // private segment
    assert(parse(b.data(),b.size(),"cobalt_gradient").code==Code::NeedsScratch);
    b=co; put32(b,k.descriptorOffset,1024u);  // LDS
    assert(parse(b.data(),b.size(),"cobalt_gradient").code==Code::NeedsLds);
    b=co; b[k.descriptorOffset+56]|=0x02u;    // dispatch ptr
    assert(parse(b.data(),b.size(),"cobalt_gradient").code==Code::NeedsHsaInputs);
    b=co; b[k.descriptorOffset+57]&=~0x04u;   // wave64
    assert(parse(b.data(),b.size(),"cobalt_gradient").code==Code::NotWave32);
    b=co; put32(b,k.descriptorOffset+16,0x10c4u); // entry off by 4
    assert(parse(b.data(),b.size(),"cobalt_gradient").code==Code::BadDescriptor);
    b=co;
    for(uint64_t i=0;i<k.codeBytes;i+=4)
        if(b[k.codeOffset+i+3]==0xbfu && b[k.codeOffset+i+2]==0xb0u) put32(b,k.codeOffset+i,0xbf800000u);
    assert(parse(b.data(),b.size(),"cobalt_gradient").code==Code::NoEndpgm);
    assert(parse(co.data(),20u,"cobalt_gradient").code==Code::NotElf);
    // CPU reference for the readback: corners of a 1024x768 frame 0.
    assert(gradientPixel(0,0,1024,768,0)==0x000000u);
    assert(gradientPixel(1023,767,1024,768,0)==((254u<<16)|(254u<<8)|((1023u^767u)&0xffu)));
    assert(gradientPixel(512,384,1024,768,7)==((127u<<16)|(127u<<8)|(((512u^384u)+7u)&0xffu)));
    std::printf("gfx1102 shader: gradient.co parses, PM4-ready (wave32, no scratch/LDS/HSA "
                "inputs, %u user SGPRs, %u-byte kernargs), refusals checked\n",
                k.userSgprs,k.kernargBytes);
}
