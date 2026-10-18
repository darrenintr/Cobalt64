// PSP13 IP firmware records (Navi33Psp13IpFw.h): synthetic images with
// hand-computed expectations, bounds and version failures, and, when given
//   argv[1] = directory with the eight decompressed gc_11_0_2_*.bin/sdma files
//   argv[2] = tests/fixtures/rx7600-fw-plan.tsv
// a record-for-record comparison with tools/fwplan.py's output on the real files.
#include "../src/Navi33Psp13IpFw.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace navi33;
namespace fw=navi33::psp13ipfw;

static void w32(std::vector<uint8_t> &v,size_t p,uint32_t x) {
    for(unsigned i=0;i<4u;++i)v[p+i]=static_cast<uint8_t>(x>>(8u*i));
}
// common_firmware_header: size, header_size, version maj/min, ip maj/min, ...
static std::vector<uint8_t> image(size_t total,uint32_t hdrSize,uint16_t maj,uint16_t min,
                                  uint16_t ipMaj,uint32_t ucodeSize) {
    std::vector<uint8_t> v(total,0u);
    w32(v,0,static_cast<uint32_t>(total));w32(v,4,hdrSize);
    w32(v,8,static_cast<uint32_t>(maj)|static_cast<uint32_t>(min)<<16u);
    w32(v,12,static_cast<uint32_t>(ipMaj));
    w32(v,16,0x11u);w32(v,20,ucodeSize);w32(v,24,256u);
    for(size_t i=256;i<total;++i)v[i]=static_cast<uint8_t>(i*7u);
    return v;
}
struct Set { std::vector<uint8_t> f[fw::kFileCount]; };
static Set synthetic() {
    Set s;
    auto &sd=s.f[static_cast<size_t>(fw::File::Sdma)];
    sd=image(416,64,2,0,6,160);
    w32(sd,36,100);w32(sd,48,356);w32(sd,52,60);
    for(fw::File f:{fw::File::Pfp,fw::File::Me,fw::File::Mec}) {
        auto &c=s.f[static_cast<size_t>(f)];
        c=image(536,60,2,0,11,280);
        w32(c,32,0x77u);w32(c,36,200);w32(c,40,256);w32(c,44,80);w32(c,48,456);
    }
    for(fw::File f:{fw::File::Mes,fw::File::Mes1}) {
        auto &m=s.f[static_cast<size_t>(f)];
        m=image(476,72,1,0,11,220);
        w32(m,36,150);w32(m,40,256);w32(m,48,70);w32(m,52,406);
    }
    auto &u=s.f[static_cast<size_t>(fw::File::Smu)];
    u=image(416,44,2,1,13,100);
    auto &i=s.f[static_cast<size_t>(fw::File::Imu)];
    i=image(352,48,1,0,11,96);
    w32(i,32,64);w32(i,36,256);w32(i,40,32);w32(i,44,320);
    auto &r=s.f[static_cast<size_t>(fw::File::Rlc)];
    r=image(580,204,2,3,11,100);
    // v2_1: direct_len @104, CNTL @108.. (size @116 off @120), GPM size @132 off @136,
    // SRM size @148 off @152; v2_2 @156: iram size/off, dram size/off; v2_3 @172: RLC_P
    // size @180 off @184, RLC_V size @196 off @200
    w32(r,116,0);w32(r,120,0);
    w32(r,132,40);w32(r,136,356);w32(r,148,48);w32(r,152,396);
    w32(r,156,64);w32(r,160,444);w32(r,164,32);w32(r,168,508);
    w32(r,180,16);w32(r,184,540);w32(r,196,24);w32(r,200,556);
    return s;
}
static fw::Images view(const Set &s) {
    fw::Images in;
    for(size_t k=0;k<fw::kFileCount;++k)in.f[k]={s.f[k].data(),s.f[k].size()};
    return in;
}
static const fw::Record *find(const fw::List &l,const char *name) {
    for(uint32_t i=0;i<l.count;++i)if(!strcmp(l.rec[i].name,name))return &l.rec[i];
    return nullptr;
}
static bool has(const fw::List &l,const char *name,uint32_t type,fw::File f,
                uint32_t off,uint32_t size) {
    const fw::Record *r=find(l,name);
    return r && r->type==type && r->file==f && r->offset==off && r->size==size;
}

int main(int argc,char **argv) {
    static_assert(fw::kFileCount==9u,"nine images");
    // ---- synthetic images ----
    Set s=synthetic();
    fw::List l;
    assert(fw::build(view(s),l));
    // sdma 2 + cp 3x(1+stacks 2/2/4) + mes 4 + imu 2 + rlc (gpm, srm, iram, dram, p, v, g) 7
    assert(l.count==2u+3u+8u+4u+2u+7u+1u);
    for(uint32_t i=1;i<l.count;++i)assert(l.rec[i-1u].order<=l.rec[i].order);
    assert(has(l,"SMC",18,fw::File::Smu,256,100));                 // the SMU image, sent first by Linux
    assert(l.rec[l.count-1u].order==52u && l.rec[l.count-2u].order==50u);   // SMC sorts last, RLC_G before it
    assert(has(l,"SDMA_UCODE_TH0",71,fw::File::Sdma,256,100));
    assert(has(l,"SDMA_UCODE_TH1",72,fw::File::Sdma,356,60));
    assert(has(l,"CP_RS64_PFP",87,fw::File::Pfp,256,200));
    assert(has(l,"CP_RS64_PFP_P0_STACK",90,fw::File::Pfp,456,80));
    assert(has(l,"CP_RS64_PFP_P1_STACK",91,fw::File::Pfp,456,80));
    assert(has(l,"CP_RS64_ME_P1_STACK",93,fw::File::Me,456,80));
    assert(has(l,"CP_RS64_MEC_P3_STACK",97,fw::File::Mec,456,80));
    assert(has(l,"CP_MES",33,fw::File::Mes,256,150));
    assert(has(l,"CP_MES_DATA",34,fw::File::Mes,406,70));
    assert(has(l,"CP_MES1",81,fw::File::Mes1,256,150));
    assert(has(l,"CP_MES1_DATA",82,fw::File::Mes1,406,70));
    assert(has(l,"IMU_I",68,fw::File::Imu,256,64));
    assert(has(l,"IMU_D",69,fw::File::Imu,320,32));            // right after IRAM
    assert(!find(l,"RLC_RESTORE_LIST_CNTL"));                   // empty list skipped
    assert(has(l,"RLC_RESTORE_LIST_GPM_MEM",20,fw::File::Rlc,356,40));
    assert(has(l,"RLC_RESTORE_LIST_SRM_MEM",21,fw::File::Rlc,396,48));
    assert(has(l,"RLC_IRAM",26,fw::File::Rlc,444,64));
    assert(has(l,"RLC_DRAM",48,fw::File::Rlc,508,32));
    assert(has(l,"RLC_P",25,fw::File::Rlc,540,16));
    assert(has(l,"RLC_V",7,fw::File::Rlc,556,24));
    assert(has(l,"RLC_G",8,fw::File::Rlc,256,100));
    assert(!find(l,"AUTOLOAD_RLC"));                            // autoload is no record
    // staging: each record page-aligned, offsets in list order
    assert(fw::stagedBytes(l.rec[0])==4096u && fw::stagingOffset(l,3)==3u*4096u);
    assert(fw::stagingTotal(l)==static_cast<uint64_t>(l.count)*4096u); // all synthetic < 1 page
    assert(fw::stagingOffset(l,l.count)==fw::stagingTotal(l));

    // ---- structure failures: every one is refused ----
    auto mutate=[&](fw::File f,size_t at,uint32_t v) {
        Set m=synthetic();w32(m.f[static_cast<size_t>(f)],at,v);
        fw::List x;return fw::build(view(m),x);
    };
    assert(!mutate(fw::File::Smu,20,1000));             // SMU ucode runs past the file
    assert(!mutate(fw::File::Smu,12,11));               // SMU header says IP 11, not 13
    assert(!mutate(fw::File::Sdma,52,1000));            // TH1 runs past the file
    assert(!mutate(fw::File::Sdma,48,40));              // TH1 inside the header
    assert(!mutate(fw::File::Sdma,36,0));               // empty TH0
    assert(!mutate(fw::File::Pfp,48,100000));           // stack offset outside
    assert(!mutate(fw::File::Mec,44,0));                // no stack data
    assert(!mutate(fw::File::Mes,40,10));               // MES inside its header
    assert(!mutate(fw::File::Mes1,48,1u<<31u));         // huge data size
    assert(!mutate(fw::File::Imu,32,1u<<31u));          // IRAM huge (also overflow path)
    assert(!mutate(fw::File::Imu,40,500));              // DRAM past the end
    assert(!mutate(fw::File::Rlc,136,10));              // GPM list inside the header
    assert(!mutate(fw::File::Rlc,168,579));             // DRAM one byte from the end, size 32
    assert(!mutate(fw::File::Rlc,200,4294967295u));     // RLC_V offset overflow
    assert(!mutate(fw::File::Rlc,0,579));               // header size != file size
    assert(!mutate(fw::File::Rlc,8,3u|2u<<16u));        // header version 3.2, not 2.3
    assert(!mutate(fw::File::Pfp,8,1u<<16u));           // v0.1 instead of v2.0
    assert(!mutate(fw::File::Me,12,10));                // wrong IP major
    assert(!mutate(fw::File::Mec,24,40));               // ucode array inside the 60 B header
    {Set m=synthetic();m.f[static_cast<size_t>(fw::File::Imu)].resize(40);
     fw::List x;assert(!fw::build(view(m),x));}         // truncated file
    {fw::Images empty;fw::List x;assert(!fw::build(empty,x));}
    // an empty RLC restore list is fine, a non-empty one beyond the file is not
    assert(!mutate(fw::File::Rlc,116,50) );             // CNTL size 50 at offset 0 -> inside header

    // ---- pins: only the exact files ----
    assert(!fw::allPinned(view(s)));                    // synthetic is not the real set
    {std::vector<uint8_t> fake(fw::kPinnedBytes[0],0x5au);   // right size, wrong bytes
     assert(!fw::pinned(fw::File::Pfp,{fake.data(),fake.size()}));
     assert(!fw::pinned(fw::File::Pfp,{fake.data(),fake.size()-1u}));  // wrong size
     assert(!fw::pinned(fw::File::Pfp,{}));}

    // ---- the real files against the golden list ----
    if(argc>2) {
        std::vector<uint8_t> real[fw::kFileCount];
        for(size_t k=0;k<fw::kFileCount;++k) {
            const std::string path=std::string(argv[1])+"/"+fw::kFileNames[k]+".bin";
            FILE *fp=std::fopen(path.c_str(),"rb");
            assert(fp);
            real[k].resize(fw::kPinnedBytes[k]+1u);
            const size_t n=std::fread(real[k].data(),1,real[k].size(),fp);
            std::fclose(fp);
            real[k].resize(n);
        }
        Set rs;for(size_t k=0;k<fw::kFileCount;++k)rs.f[k]=real[k];
        const fw::Images in=view(rs);
        assert(fw::allPinned(in));                      // exact bytes, by SHA-256
        fw::List rl;
        assert(fw::build(in,rl));
        FILE *g=std::fopen(argv[2],"r");
        assert(g);
        char line[512];uint32_t idx=0u;
        while(std::fgets(line,sizeof(line),g)) {
            if(line[0]!='R')continue;
            unsigned order,type,off,size;char name[64],key[16],sha[80];
            assert(std::sscanf(line,"R %u %63s %u %15s %u %u %79s",&order,name,&type,key,&off,&size,sha)==7);
            assert(idx<rl.count);
            const fw::Record &r=rl.rec[idx++];
            assert(r.order==order && !strcmp(r.name,name) && r.type==type &&
                   r.offset==off && r.size==size);
            const char *keys[]={"pfp","me","mec","rlc","imu","sdma","mes","mes1","smu"};
            assert(!strcmp(keys[static_cast<size_t>(r.file)],key));
            // the record's own bytes hash to the golden digest
            const sha256::Digest d=sha256::hash(in.f[static_cast<size_t>(r.file)].data+r.offset,r.size);
            char hex[65];for(unsigned i=0;i<32u;++i)std::snprintf(hex+2u*i,3,"%02x",d.b[i]);
            assert(!strcmp(hex,sha));
        }
        std::fclose(g);
        assert(idx==rl.count && rl.count==27u);
        assert(rl.rec[26].type==18u && !strcmp(rl.rec[26].name,"SMC"));   // the SMU is the last record
        assert(fw::stagingTotal(rl)==2801664u);          // 2.42 MiB, as fwplan.py says
        std::puts("real firmware matches rx7600-fw-plan.tsv record for record");
    }
    std::puts("PSP13 IP firmware record tests passed");
    return 0;
}
