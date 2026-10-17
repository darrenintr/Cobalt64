#include "../src/Navi33Discovery.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

using navi33::IPDiscovery;
using navi33::IPEntry;

static void w16(std::vector<uint8_t>&v,size_t p,uint16_t x){v[p]=x&255u;v[p+1]=x>>8;}
static void w32(std::vector<uint8_t>&v,size_t p,uint32_t x){w16(v,p,x&65535u);w16(v,p+2,x>>16);}
static void w64(std::vector<uint8_t>&v,size_t p,uint64_t x){w32(v,p,x&0xffffffffu);w32(v,p+4,x>>32);}
static uint16_t sum(const std::vector<uint8_t>&v,size_t a,size_t b){
    uint16_t s=0;for(size_t i=a;i<b;++i)s=static_cast<uint16_t>(s+v[i]);return s;
}
struct Config {bool v2=false,wide=false,duplicate=false,wrongGC=false,emptyBases=false;size_t prefix=0;};
static std::vector<uint8_t> make(Config c={}){
    const size_t st=c.prefix,hdr=c.v2?16:12,ipd=128,die=208;
    std::vector<uint8_t> v(st+1024,0u);
    w32(v,st,0x28211407u);w16(v,st+4,c.v2?2:1);
    if(c.v2)w16(v,st+12,1u);
    const size_t info=st+hdr;
    w16(v,info,static_cast<uint16_t>(ipd));
    const size_t th=st+ipd;
    w32(v,th,0x53445049u);w16(v,th+4,c.wide?4:3);
    w32(v,th+8,0x12345678u);w16(v,th+12,1u);
    w16(v,th+14,0u);w16(v,th+16,static_cast<uint16_t>(die));
    if(c.wide)v[th+78]=1;
    const size_t dh=st+die;
    w16(v,dh,0);
    const uint16_t n=static_cast<uint16_t>(5+(c.duplicate?1:0));
    w16(v,dh+2,n);
    const struct {uint16_t id;uint8_t major,minor,rev;} entries[]={
        {11,11,0,2},{42,6,0,2},{255,13,0,7},{1,13,0,7},{271,3,2,1},{11,11,0,2}
    };
    size_t p=dh+4;
    for(uint16_t i=0;i<n;++i){
        auto e=entries[i];if(c.wrongGC&&e.id==11)e.rev=3;
        w16(v,p,e.id);v[p+2]=0;v[p+3]=c.emptyBases?0:2;
        v[p+4]=e.major;v[p+5]=e.minor;v[p+6]=e.rev;v[p+7]=0;
        p+=8;
        if(!c.emptyBases){
            const uint64_t address=c.wide?0x0000000500001234ull:0x1234ull;
            if(c.wide){w64(v,p,address);w64(v,p+8,address+1);p+=16;}
            else{w32(v,p,0x1234);w32(v,p+4,0x1235);p+=8;}
        }
    }
    const uint16_t tsize=static_cast<uint16_t>(p-th);
    w16(v,th+6,tsize);w16(v,info+4,tsize);w16(v,info+2,sum(v,th,p));
    const uint16_t bsize=static_cast<uint16_t>(p-st);
    w16(v,st+10,bsize);w16(v,st+8,sum(v,st+10,st+bsize));
    v.resize(st+bsize);
    return v;
}
int main(){
    IPDiscovery d;IPEntry e;
    for(const bool v2:{false,true})for(const bool wide:{false,true})
        for(const size_t prefix:{size_t(0),size_t(128)}){
            auto bin=make(Config{v2,wide,false,false,false,prefix});
            assert(d.parse(bin.data(),bin.size()));
            assert(d.valid()&&d.count()==5&&d.binaryOffset()==prefix);
            assert(d.is64BitBase()==wide&&d.isNavi33Candidate());
            assert(d.find(navi33::ipid::GC,0,e)&&e.major==11&&e.minor==0&&e.revision==2);
            assert(e.numBases==2&&e.bases[0]==(wide?0x0000000500001234ull:0x1234ull));
            assert(!d.find(999,0,e)&&!d.entryAt(5,e));
            bin[prefix+208+4+7]^=1;
            assert(!d.parse(bin.data(),bin.size()));
        }
    {auto b=make(Config{false,false,true});assert(d.parse(b.data(),b.size()));
        assert(!d.find(navi33::ipid::GC,0,e)&&!d.isNavi33Candidate());}
    {auto b=make(Config{false,false,false,true});assert(d.parse(b.data(),b.size()));
        assert(!d.isNavi33Candidate());}
    {auto b=make(Config{false,false,false,false,true});assert(d.parse(b.data(),b.size()));
        assert(!d.isNavi33Candidate());}
    {const auto b=make();
        for(size_t end=0;end<b.size();++end)assert(!d.parse(b.data(),end));
        assert(!d.parse(nullptr,0));
        auto x=b;x[8]^=1;assert(!d.parse(x.data(),x.size()));
        x=b;x[12]^=1;assert(!d.parse(x.data(),x.size()));
        x=b;x[128]^=1;assert(!d.parse(x.data(),x.size()));
        x=b;x[208+3]=255;assert(!d.parse(x.data(),x.size()));
        x=b;w16(x,12,0xfffe);assert(!d.parse(x.data(),x.size()));
        x=b;w16(x,16,0xfffe);assert(!d.parse(x.data(),x.size()));
        x=b;w16(x,128+16,0xfffe);assert(!d.parse(x.data(),x.size()));
        x=b;w16(x,208+2,0xffff);assert(!d.parse(x.data(),x.size()));
        x=b;w16(x,208,1u);assert(!d.parse(x.data(),x.size()));
        x=b;w16(x,128+12,2u);assert(!d.parse(x.data(),x.size()));
    }
    {const auto b=make();uint32_t rng=0x97a3bfe5u;
        for(int i=0;i<4000;++i){
            auto x=b;rng=rng*1664525u+1013904223u;
            const size_t p=20u+(rng%(x.size()-20u));
            x[p]^=static_cast<uint8_t>(1u+(rng%255u));
            assert(!d.parse(x.data(),x.size()));
        }
    }
    std::puts("Navi33 IP discovery: 32/64-bit, v1/v2, v3/v4, bounds/CRC/4K mutations passed; offline only");
}
