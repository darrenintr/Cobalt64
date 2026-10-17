// Offline, synthetic AMD IP Discovery fixtures. No GPU or PCI access.
#include "../src/Navi33HardwarePlan.h"
#include "../src/Navi33Dcn321.h"
#include "../src/Navi33Psp13Status.h"
#include "../src/Navi33Sdma6Status.h"
#include "../src/Navi33Gmc11Mmhub.h"
#include "../src/Navi33Sdma6BarAdapter.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstdint>

using namespace navi33;

static void put16(std::vector<uint8_t> &v, size_t p, uint16_t x) {
    v[p]=static_cast<uint8_t>(x);
    v[p+1]=static_cast<uint8_t>(x>>8);
}
static void put32(std::vector<uint8_t> &v, size_t p, uint32_t x) {
    put16(v,p,static_cast<uint16_t>(x));
    put16(v,p+2,static_cast<uint16_t>(x>>16));
}
static void put64(std::vector<uint8_t> &v, size_t p, uint64_t x) {
    put32(v,p,static_cast<uint32_t>(x));
    put32(v,p+4,static_cast<uint32_t>(x>>32));
}
static uint16_t sum(const std::vector<uint8_t> &v, size_t a, size_t b) {
    uint16_t c=0;
    for (size_t i=a;i<b;++i) c=static_cast<uint16_t>(c+v[i]);
    return c;
}

struct Fixture {
    std::vector<uint8_t> bytes;
    size_t gcVersion=0, pspVersion=0, smuVersion=0, sdmaVersion=0, dcnVersion=0;
    size_t dcnBase2=0;
    bool wide=false;
    size_t binaryEnd=0;
    void checksums() {
        // Table checksum covers table from signature to final IP record.
        put16(bytes,14,sum(bytes,128,binaryEnd));
        // Binary checksum covers the size word and every following byte.
        put16(bytes,8,sum(bytes,10,binaryEnd));
    }
};
static Fixture make(bool wide=false, bool duplicateGc=false, bool zeroDcnBase2=false, bool withMmhub=false) {
    Fixture f{};
    f.wide=wide;
    auto &v=f.bytes;
    v.resize(1024,0);
    put32(v,0,0x28211407u);
    put16(v,4,1);
    put16(v,12,128);
    put32(v,128,0x53445049u);
    put16(v,132,wide?4:3);
    put32(v,136,0x12345678u);
    put16(v,140,1);
    put16(v,144,208);
    if (wide) v[206]=1;
    put16(v,208,0);
    put16(v,210,(duplicateGc || withMmhub)?6:5);

    // All five exact Navi33 engine fingerprints, per amdgpu_discovery.c.
    struct Entry { uint16_t id;uint8_t maj,min,rev;uint64_t base; };
    const Entry entries[]={
        {ipid::GC,11,0,2,0x100u},
        {ipid::MP0,13,0,7,0x200u},
        {ipid::MP1,13,0,7,0x300u},
        {ipid::SDMA0,6,0,2,0x400u},
        {ipid::DMU,3,2,1,0x1800u},
        {ipid::GC,11,0,2,0x100u},
        {ipid::MMHUB,3,0,2,0x500u}
    };
    size_t p=212;
    for(int i=0;i<((duplicateGc || withMmhub)?6:5);++i) {
        const auto &e=entries[(i==5 && withMmhub)?6:i];
        put16(v,p,e.id);
        v[p+2]=0;v[p+3]=3; // >= 3 bases for DCN BASE_IDX 2.
        v[p+4]=e.maj;v[p+5]=e.min;v[p+6]=e.rev;v[p+7]=0;
        if(i==0)f.gcVersion=p+6;
        if(i==1)f.pspVersion=p+6;
        if(i==2)f.smuVersion=p+6;
        if(i==3)f.sdmaVersion=p+6;
        if(i==4){f.dcnVersion=p+6; f.dcnBase2=p+8+(wide?16:8);}
        p+=8;
        for (uint8_t j=0;j<3;++j) {
            uint64_t addr=e.base;
            if(i==4 && j==2 && zeroDcnBase2)addr=0;
            if(wide){put64(v,p,addr);p+=8;}
            else {put32(v,p,static_cast<uint32_t>(addr));p+=4;}
        }
    }
    f.binaryEnd=p;
    put16(v,134,static_cast<uint16_t>(p-128));
    put16(v,16,static_cast<uint16_t>(p-128));
    put16(v,10,static_cast<uint16_t>(p));
    f.checksums();
    v.resize(p);
    return f;
}

int main() {
    static_assert(!kFirmwareLoadingImplemented, "only the PSP13 bootloader loads firmware");
    static_assert(!kRegisterWritesImplemented, "no GPU register writes beyond C2PMSG_35/36");
    static_assert(!kGpuCommandSubmissionImplemented, "no GPU queues in this stage");
    static_assert(kPspBootloaderLoadImplemented, "the gated PSP13 sOS load exists");

    IPDiscovery disc{};
    // No snapshot: fail closed.
    assert(inventory(disc).code==InventoryCode::NoDiscovery);
    assert(resolveDword(disc,Engine::DCN321,2,0x1b4c,65536).code==
           AddressCode::InventoryRejected);

    {auto f=make();assert(disc.parse(f.bytes.data(),f.bytes.size()));
        const InventoryReport id=inventory(disc);
        assert(id.code==InventoryCode::Pass);
        assert(id.matchedEngines==5);
        assert(disc.isNavi33Candidate());
        const auto m=dcn321::buildReadPlan(disc,65536);
        assert(m.code==AddressCode::Pass);
        assert(m.resolved==dcn321::kReadOnlyCount);
        assert(m.offsets[static_cast<size_t>(dcn321::Register::Pipe0FrameCount)]==
               (0x1800ull+0x1b4cull)*4ull);
        assert(m.offsets[static_cast<size_t>(dcn321::Register::Pipe3FrameCount)]==
               (0x1800ull+0x1cccull)*4ull);
        assert(m.offsets[static_cast<size_t>(dcn321::Register::Pipe0SurfaceLo)]==
               (0x1800ull+0x060aull)*4ull);
        assert(m.offsets[static_cast<size_t>(dcn321::Register::VmFbBase)]==
               (0x1800ull+0x0475ull)*4ull);
        const auto psp=navi33::psp13::buildPlan(disc,65536u);
        assert(psp.state==N33_PSP_STOPPED);
        assert(psp.aliveOffset==(0x200ull+0x0091ull)*4ull);
        assert(psp.versionOffset==(0x200ull+0x007aull)*4ull);
        const auto sdma=navi33::sdma6live::buildPlan(disc,65536u);
        assert(sdma.state==N33_SDMA_OBSERVED);
        assert(sdma.offsets[0]==(0x100ull+0x25ull)*4ull);
        assert(sdma.offsets[4]==(0x100ull+0x85ull)*4ull);
        const auto qmap=sdma6_bar::buildPlan(disc,65536u);
        assert(qmap.code==AddressCode::Pass);
        uint64_t rbBaseBytes=0u;
        assert(sdma6_bar::translate(qmap,sdma6_mmio::kRbBase,&rbBaseBytes));
        assert(rbBaseBytes==(0x100ull+0x81ull)*4ull);
        assert(qmap.byteOffset[0]==(0x100ull+0x80ull)*4ull);
        assert(qmap.byteOffset[5]==(0x100ull+0x85ull)*4ull);
        assert(!sdma6_bar::translate(qmap,UINT32_MAX,&rbBaseBytes));
        assert(sdma6_bar::buildPlan(disc,256u).code==
               AddressCode::OutsideBar5);
        assert(dcn321::combineSurfaceAddress(0xabcd1234u,0x00000002u)==
               0x00000002abcd1234ull);
        assert(resolveDword(disc,Engine::DCN321,2,UINT32_MAX,65536).code==
               AddressCode::OutsideBar5);
        assert(resolveDword(disc,Engine::DCN321,3,0,65536).code==
               AddressCode::MissingSegment);
        assert(resolveDword(disc,Engine::DCN321,2,0,0).code==
               AddressCode::NoBar5);
        assert(dcn321::buildReadPlan(disc,16384).code==AddressCode::OutsideBar5);
        assert(dcn321::resolveReadOnly(disc,
                  static_cast<dcn321::Register>(255),65536).code==
               AddressCode::MissingSegment);
    }
    {auto f=make(false,false,false,true);
        assert(disc.parse(f.bytes.data(),f.bytes.size()));
        assert(inventory(disc).code==InventoryCode::Pass);
        const auto gfxhub=mmhub11::planGfxFromDiscovery(disc,65536u);
        assert(gfxhub.code==mmhub11::Code::Prepared);
        assert(gfxhub.hub==mmhub11::Hub::Gfxhub);
        assert(gfxhub.offsets[0u]==(0x100ull+0x1688ull)*4ull);
        assert(gfxhub.offsets[8u]==(0x100ull+0x16bcull)*4ull);
        const auto mmhub=mmhub11::planFromDiscovery(disc,65536u);
        assert(mmhub.code==mmhub11::Code::Prepared);
        assert(mmhub.revision==2u);
        assert(mmhub.offsets[0u]==(0x500ull+0x06c0ull)*4ull);
        assert(mmhub.offsets[8u]==(0x500ull+0x06f4ull)*4ull);
        assert(mmhub11::planFromDiscovery(disc,64u).code==
               mmhub11::Code::InvalidBar);
    }
    {auto f=make();
        assert(disc.parse(f.bytes.data(),f.bytes.size()));
        assert(mmhub11::planFromDiscovery(disc,65536u).code==
               mmhub11::Code::NoDiscovery);
    }
    // Reject RDNA4/Phoenix IP versions instead of reusing Navi48 registers.
    for (int index=0;index<5;++index) {
        auto f=make();
        const size_t offsets[]={f.gcVersion,f.pspVersion,f.smuVersion,
                                f.sdmaVersion,f.dcnVersion};
        f.bytes[offsets[index]]=(index==0)?12u:((index==4)?4u:14u);
        f.checksums();
        assert(disc.parse(f.bytes.data(),f.bytes.size()));
        const auto report=inventory(disc);
        assert(report.code==InventoryCode::UnsupportedEngineVersion);
        assert(report.matchedEngines==static_cast<unsigned>(index));
        assert(dcn321::buildReadPlan(disc,65536).code==
               AddressCode::InventoryRejected);
    }
    {auto f=make(false,true);
        assert(disc.parse(f.bytes.data(),f.bytes.size()));
        assert(inventory(disc).code==InventoryCode::MissingOrDuplicateEngine);
    }
    {auto f=make(true);
        assert(disc.parse(f.bytes.data(),f.bytes.size()));
        assert(inventory(disc).code==InventoryCode::Pass);
        assert(dcn321::buildReadPlan(disc,65536).code==AddressCode::Pass);
        put64(f.bytes,f.dcnBase2,0x0000000100001800ull);
        f.checksums();
        assert(disc.parse(f.bytes.data(),f.bytes.size()));
        assert(dcn321::buildReadPlan(disc,65536).code==AddressCode::OutsideBar5);
    }
    {auto f=make(false,false,true);
        assert(disc.parse(f.bytes.data(),f.bytes.size()));
        assert(inventory(disc).code==InventoryCode::Pass);
        assert(dcn321::buildReadPlan(disc,65536).code==AddressCode::UnmappedSegment);
    }
    // Rejection of invalid/reused metadata must not depend on asserts alone.
    std::puts("Navi33 GC11/PSP13/SMU13/SDMA6/DCN3.2.1 and 18 DCN read-offset plans: PASS; no MMIO");
    return 0;
}
