// Host-only N33_SEL_MMIO_PEEK allowlist and read tests. No GPU access:
// reads go to a mock, resolution uses the physical RX 7600 IP Discovery.
#include "../src/Navi33MmioPeek.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace navi33;
using namespace navi33::peek;
using rx7600_fixture::kBar5;

namespace {
bool covered(uint16_t hwid,uint8_t seg,uint32_t reg) {
    for(uint32_t i=0;i<N33_PEEK_WINDOW_COUNT;++i) {
        const n33_peek_window &w=n33_peek_windows[i];
        if(w.hwid==hwid && w.segment==seg &&
           reg>=w.first && reg-w.first<w.count) return true;
    }
    return false;
}
uint32_t find(const char *name) {
    for(uint32_t i=0;i<N33_PEEK_WINDOW_COUNT;++i)
        if(!std::strcmp(n33_peek_windows[i].name,name)) return i;
    assert(!"unknown window");
    return 0u;
}
struct Mock {
    std::vector<uint64_t> offsets;
    unsigned failAt=0u;
};
bool mockRead(void *ctx,uint64_t off,uint32_t *out) {
    auto *m=static_cast<Mock *>(ctx);
    m->offsets.push_back(off);
    if(m->offsets.size()==m->failAt)return false;
    *out=static_cast<uint32_t>(off^0xa5a50000u);
    return true;
}
} // namespace

int main() {
    // Table sanity: unique names, no overlap within one IP segment.
    for(uint32_t i=0;i<N33_PEEK_WINDOW_COUNT;++i) {
        const n33_peek_window &a=n33_peek_windows[i];
        assert(a.name && a.count>=1u && a.first+a.count>a.first);
        for(uint32_t j=i+1u;j<N33_PEEK_WINDOW_COUNT;++j) {
            const n33_peek_window &b=n33_peek_windows[j];
            assert(std::strcmp(a.name,b.name)!=0);
            if(a.hwid==b.hwid && a.segment==b.segment)
                assert(a.first+a.count<=b.first || b.first+b.count<=a.first);
        }
    }
    // Registers whose read has a side effect must never be covered.
    for(uint32_t r=0x1698u;r<=0x16a9u;++r)   // GCVM_CONTEXTS_DISABLE, ENG*_SEM
        assert(!covered(N33_HWID_GC,0u,r));
    for(uint32_t r=0x06d0u;r<=0x0706u;++r)   // MMVM ENG*_SEM / ENG*_ACK
        assert(!covered(N33_HWID_MMHUB,0u,r));
    const uint32_t sdmaHazards[]={0x0036u,0x0037u,0x0060u,0x0061u,
                                  0x0070u,0x0077u,0x0078u,0x007bu};
    for(uint32_t r:sdmaHazards)              // EDC, EA_DBIT, INT, SCRATCH_RAM
        assert(!covered(N33_HWID_GC,0u,r));
    const uint32_t sdmaControlHazards[]={0x0024u,0x0042u,0x0043u,0x0044u,0x0050u};
    for(uint32_t r:sdmaControlHazards)       // PROGRAM, UTCL1_INV0..2, ERROR_LOG
        assert(!covered(N33_HWID_GC,0u,r));
    assert(!covered(N33_HWID_GC,1u,0x5880u) && // SDMA0_UCODE_ADDR/DATA
           !covered(N33_HWID_GC,1u,0x5881u));

    const std::vector<uint8_t> bytes=rx7600_fixture::blob();
    IPDiscovery disc{};
    assert(disc.parse(bytes.data(),bytes.size()));

    // Request validation happens before the ROM is trusted.
    assert(plan(disc,N33_PEEK_WINDOW_COUNT,0u,1u,kBar5).state==N33_PEEK_BAD_REQUEST);
    assert(plan(disc,0u,0u,0u,kBar5).state==N33_PEEK_BAD_REQUEST);
    assert(plan(disc,0u,0u,N33_PEEK_MAX_DWORDS+1u,kBar5).state==N33_PEEK_BAD_REQUEST);
    const uint32_t psp=find("PSP_C2PMSG");
    assert(plan(disc,psp,96u,1u,kBar5).state==N33_PEEK_NOT_ALLOWED);
    assert(plan(disc,psp,90u,7u,kBar5).state==N33_PEEK_NOT_ALLOWED);
    assert(plan(IPDiscovery{},psp,0u,1u,kBar5).state==N33_PEEK_BAD_ROM);
    assert(plan(disc,psp,0u,1u,0x1000u).state==N33_PEEK_NO_BAR5);

    // Offsets Linux amdgpu would use on the physical RX 7600.
    auto at=[&](const char *name,uint32_t first){
        const Plan p=plan(disc,find(name),first,1u,kBar5);
        assert(p.state==N33_PEEK_DONE);
        return p.offset;
    };
    assert(at("PSP_C2PMSG",3u)==0x5818cu);          // MP0_SMN_C2PMSG_35
    assert(at("PSP_C2PMSG",49u)==0x58244u);         // MP0_SMN_C2PMSG_81
    assert(at("SMU_C2PMSG",58u)==0x58a68u);         // MP1_SMN_C2PMSG_90
    assert(at("SDMA0_STATUS",0u)==0x4a14u);         // SDMA0_STATUS_REG
    assert(at("SDMA0_Q0_RB",0u)==0x4b80u);          // SDMA0_QUEUE0_RB_CNTL
    assert(at("SDMA0_Q1_RB",0u)==0x4ce0u);          // SDMA0_QUEUE1_RB_CNTL
    assert(at("GRBM_STATUS",0u)==0x8010u);          // GRBM_STATUS
    assert(at("GCVM_CONTEXT_CNTL",0u)==0xa3a0u);    // GCVM_CONTEXT0_CNTL
    assert(at("RLC_BOOTLOAD_STATUS",0u)==0x3ba08u); // GC BASE_IDX 1
    assert(at("MMVM_CONTEXT_CNTL",0u)==0x69b00u);   // MMVM_CONTEXT0_CNTL
    assert(at("NBIO_MEMSIZE",0u)==0x378cu);         // RCC_CONFIG_MEMSIZE
    assert(at("MMVM_L2_CNTL2",0u)==0x69a04u);       // MMVM_L2_CNTL2
    assert(at("MMMC_VM_MX_L1_TLB_CNTL",0u)==0x6a1ccu);
    assert(at("GCVM_L2_CNTL2",0u)==0xa074u);        // GCVM_L2_CNTL2
    assert(at("GCMC_VM_MX_L1_TLB_CNTL",0u)==0xa37cu);
    assert(at("SDMA0_F32_CNTL",0u)==0x3e268u);      // GC BASE_IDX 1
    // Every window resolves inside BAR5 and away from MM_INDEX/MM_DATA.
    for(uint32_t i=0;i<N33_PEEK_WINDOW_COUNT;++i) {
        const n33_peek_window &w=n33_peek_windows[i];
        for(uint32_t f=0;f<w.count;f+=N33_PEEK_MAX_DWORDS) {
            const uint32_t n=w.count-f<N33_PEEK_MAX_DWORDS ?
                w.count-f : N33_PEEK_MAX_DWORDS;
            const Plan p=plan(disc,i,f,n,kBar5);
            assert(p.state==N33_PEEK_DONE && p.offset>=0x20u);
            assert(p.offset+4ull*n<=kBar5);
        }
    }
    // MMHUB offsets are version specific: 3.0.0 must be refused.
    const std::vector<uint8_t> mm300=rx7600_fixture::blob(0u);
    IPDiscovery other{};
    assert(other.parse(mm300.data(),mm300.size()));
    assert(plan(other,find("MMVM_CONTEXT_CNTL"),0u,1u,kBar5).state==N33_PEEK_BAD_ROM);
    assert(plan(other,psp,0u,1u,kBar5).state==N33_PEEK_DONE);

    // Reads: sequential dwords, values copied, failures stop and report.
    const Plan grbm=plan(disc,find("GRBM_STATUS"),0u,4u,kBar5);
    Mock m{};
    n33_peek_result r=read(grbm,find("GRBM_STATUS"),0u,4u,mockRead,&m);
    assert(r.state==N33_PEEK_DONE && r.reads==4u && r.count==4u);
    assert(r.abi_version==N33_ABI_VERSION && r.size==sizeof(r));
    assert(r.bar5_offset==0x8010u && m.offsets.size()==4u);
    for(uint32_t i=0;i<4u;++i) {
        assert(m.offsets[i]==0x8010u+4u*i);
        assert(r.values[i]==((0x8010u+4u*i)^0xa5a50000u));
    }
    assert(!r.reserved0 && !r.reserved1);
    m=Mock{};m.failAt=3u;
    r=read(grbm,find("GRBM_STATUS"),0u,4u,mockRead,&m);
    assert(r.state==N33_PEEK_READ_FAILED && r.reads==2u);
    m=Mock{};
    r=read(plan(disc,psp,96u,1u,kBar5),psp,96u,1u,mockRead,&m);
    assert(r.state==N33_PEEK_NOT_ALLOWED && m.offsets.empty());
    r=read(grbm,0u,0u,4u,nullptr,&m);
    assert(r.state==N33_PEEK_READ_FAILED && m.offsets.empty());
    static_assert(!kMayWriteMmio,"read-only");
    std::printf("N33N MMIO peek: %u allowlisted windows, hazards excluded, RX 7600 offsets match\n",
                static_cast<unsigned>(N33_PEEK_WINDOW_COUNT));
}
