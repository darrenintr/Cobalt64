#include "../src/Navi33LiveMmio.h"
#include <cassert>
#include <cstdio>
using namespace navi33;
using namespace navi33::live_mmio;
struct Mock { unsigned reads=0; bool fail=false; bool allOnes=false; };
static bool fake(void* ctx,uint64_t off,uint32_t* out){
 auto *m=static_cast<Mock*>(ctx); ++m->reads;
 if(m->fail)return false;
 if(m->allOnes){*out=UINT32_MAX;return true;}
 *out=(off % 12u == 0u)?1u:0u;
 return true;
}
int main(){
 IPDiscovery empty{};
 auto bad=buildPlan(empty,65536u);
 assert(bad.code==Code::InvalidInventory);
 Mock m{};
 assert(readSnapshot(bad,true,fake,&m).code==Code::InvalidInventory);
 assert(m.reads==0);
 Plan p{};p.code=Code::Pass;
 for(unsigned i=0;i<kPipeCount;++i)
  for(unsigned j=0;j<3;++j) p.byteOffsets[i][j]=4096u+i*32u+j*4u;
 assert(readSnapshot(p,false,fake,&m).code==Code::Disabled && m.reads==0);
 assert(readSnapshot(p,true,nullptr,&m).code==Code::NoReader);
 auto sample=readSnapshot(p,true,fake,&m);
 assert(sample.code==Code::Pass && sample.reads==12u && m.reads==12u);
 m.fail=true;
 assert(readSnapshot(p,true,fake,&m).code==Code::MmioReadFailed);
 m.fail=false;m.allOnes=true;
 assert(readSnapshot(p,true,fake,&m).code==Code::UnmappedAllOnes);
 m.allOnes=false;
 p.byteOffsets[2][1]=p.byteOffsets[0][0];
 unsigned n=m.reads;
 assert(readSnapshot(p,true,fake,&m).code==Code::InvalidAddress && m.reads==n);
 p.byteOffsets[2][1]=1u;
 assert(readSnapshot(p,true,fake,&m).code==Code::InvalidAddress && m.reads==n);
 static_assert(!kHasGpuWritePath && !kHasGfx11Submission,"read only");
 std::puts("Navi33 bounded read-only live MMIO plan tests passed (mock only)");
}
