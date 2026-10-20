// SPDX-License-Identifier: MIT
// n33n48n: the gradient through the N48N ABI, the calls RADV's Darwin backend makes
// (mesa-patches/0002): HELLO, QUERYINFO, BOCREATE, GEMVA, CTX, SUBMIT with a fence
// chunk, WAITSEQ, with CPU access through IOConnectMapMemory64. Proves the RADV-facing
// path on the card before any Mesa build. Needs navi33-client-gpu=1.
//   sudo n33n48n --fw gradient.co [--frame N]
#include "../src/Navi33Gfx11Gradient.h"
#include "../src/Navi33N48nAbi.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
using namespace navi33;
namespace gd=gfx11_dispatch;
namespace gr=gradient;
static io_connect_t conn=IO_OBJECT_NULL;
static void die(const char *what,kern_return_t kr){fprintf(stderr,"%s failed: 0x%x\n",what,kr);if(conn)IOServiceClose(conn);exit(9);}
struct Bo { uint32_t handle=0; uint64_t size=0, va=0; volatile uint32_t *cpu=nullptr; };
static Bo bo(uint64_t bytes,uint64_t va,uint32_t vmFlags,uint64_t domain) {
    Bo b{};n48n_gem_create_in in{bytes,4096,domain,N48N_GEM_CPU_ACCESS_REQUIRED};
    uint64_t out[4]={0,0,0,0};uint32_t n=4;
    kern_return_t kr=IOConnectCallMethod(conn,N48N_SEL_BOCREATE,nullptr,0,&in,sizeof(in),out,&n,nullptr,nullptr);
    if(kr!=KERN_SUCCESS || n!=4)die("BOCREATE",kr);
    b.handle=uint32_t(out[0]);b.size=out[1];b.va=va;
    n48n_gem_va g{};g.handle=b.handle;g.operation=N48N_VA_OP_MAP;g.flags=N48N_VM_PAGE_READABLE|vmFlags;
    g.va_address=va;g.map_size=b.size;
    kr=IOConnectCallStructMethod(conn,N48N_SEL_GEMVA,&g,sizeof(g),nullptr,nullptr);
    if(kr!=KERN_SUCCESS)die("GEMVA",kr);
    mach_vm_address_t addr=0;mach_vm_size_t size=0;
    kr=IOConnectMapMemory64(conn,b.handle,mach_task_self(),&addr,&size,kIOMapAnywhere|kIOMapInhibitCache);
    if(kr!=KERN_SUCCESS || size<b.size)die("IOConnectMapMemory64",kr);
    b.cpu=reinterpret_cast<volatile uint32_t *>(addr);
    return b;
}
int main(int argc,char **argv) {
    const char *fw=nullptr;uint32_t frame=77u;
    for(int a=1;a<argc;++a) {
        if(!strcmp(argv[a],"--fw") && a+1<argc)fw=argv[++a];
        else if(!strcmp(argv[a],"--frame") && a+1<argc)frame=uint32_t(strtoul(argv[++a],nullptr,0))&0xffu;
        else {fw=nullptr;break;}
    }
    if(!fw){fputs("Usage: sudo n33n48n --fw gradient.co [--frame N]\n",stderr);return 1;}
    if(geteuid()!=0){fputs("N48N requires root.\n",stderr);return 2;}
    FILE *f=fopen(fw,"rb");std::vector<uint8_t> co;
    if(f){uint8_t buf[4096];size_t n;while((n=fread(buf,1,sizeof(buf),f))>0)co.insert(co.end(),buf,buf+n);fclose(f);}
    const gfx11_shader::Kernel k=gr::kernelOf(co.data(),co.size());
    if(k.code!=gfx11_shader::Code::Ok){fputs("gradient.co is not the pinned kernel.\n",stderr);return 8;}
    io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!svc){fputs("Navi33Probe service not loaded.\n",stderr);return 3;}
    kern_return_t kr=IOServiceOpen(svc,mach_task_self(),N48N_UC_TYPE,&conn);
    IOObjectRelease(svc);
    if(kr!=KERN_SUCCESS){fprintf(stderr,"N48N open refused (navi33-client-gpu=1?): 0x%x\n",kr);return 4;}
    const uint64_t hin[2]={N48N_ABI_VERSION,0};uint64_t hout[4]={0,0,0,0};uint32_t hn=4;
    kr=IOConnectCallScalarMethod(conn,N48N_SEL_HELLO,hin,2,hout,&hn);
    if(kr!=KERN_SUCCESS || hn!=4 || hout[0]!=N48N_ABI_VERSION)die("HELLO",kr);
    n48n_info info{};size_t isz=sizeof(info);
    kr=IOConnectCallStructMethod(conn,N48N_SEL_QUERYINFO,nullptr,0,&info,&isz);
    if(kr!=KERN_SUCCESS || isz!=sizeof(info))die("QUERYINFO",kr);
    printf("HELLO: build %llu, max IBs %llu, VMID %llu; QUERYINFO: gc %06x GB_ADDR_CONFIG %08x VRAM visible %llu/%llu MiB "
           "GTT cap %llu MiB, VA %llx..%llx, ring %u dwords\n",(unsigned long long)hout[1],(unsigned long long)hout[2],
           (unsigned long long)hout[3],info.gc_version,info.gb_addr_config,(unsigned long long)(info.vram_vis_free>>20),
           (unsigned long long)(info.vram_vis_total>>20),(unsigned long long)(info.gtt_cap>>20),
           (unsigned long long)info.va_low_first,(unsigned long long)info.va_low_last,info.ring_size_dw);
    const uint64_t base=0x400000000ull;    // where RADV's VA manager would put buffers
    Bo shader=bo(gr::kShaderMax,base,N48N_VM_PAGE_EXECUTABLE,N48N_GEM_DOMAIN_VRAM);
    Bo args=bo(4096,base+0x100000,N48N_VM_PAGE_WRITEABLE,N48N_GEM_DOMAIN_GTT);
    Bo image=bo(gr::kImageBytes,base+0x200000,N48N_VM_PAGE_WRITEABLE,N48N_GEM_DOMAIN_VRAM);
    Bo ib=bo(4096,base+0x300000,0u,N48N_GEM_DOMAIN_GTT);
    Bo fence=bo(4096,base+0x400000,N48N_VM_PAGE_WRITEABLE,N48N_GEM_DOMAIN_GTT);
    for(uint32_t i=0;i<gr::kShaderMax/4u;++i)shader.cpu[i]=0u;
    for(uint64_t i=0;i<k.codeBytes;i+=4){uint32_t w=0;for(uint64_t b=0;b<4 && i+b<k.codeBytes;++b)w|=uint32_t(co[k.codeOffset+i+b])<<(8u*b);shader.cpu[i/4u]=w;}
    uint32_t a6[6];gr::args(a6,image.va,gr::kTestSize,gr::kTestSize,gr::kTestSize,frame);
    for(int i=0;i<6;++i)args.cpu[i]=a6[i];
    for(uint32_t i=0;i<gr::kImageBytes/4u;++i)image.cpu[i]=0u;
    gd::Request rq{};rq.kernel=&k;rq.shaderMc=shader.va;rq.kernargMc=args.va;rq.groupsX=gr::kTestSize/8u;
    rq.groupsY=gr::kTestSize/8u;rq.fenceMc=args.va+0x800u;rq.fenceValue=1u;
    const gd::Stream s=gd::encode(rq);
    std::vector<uint32_t> pm4={gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe0),~0u,~0u,
                               gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe2),~0u,~0u};
    for(size_t i=0;i+8<s.count;++i)pm4.push_back(s.dwords[i]);
    for(size_t i=0;i<pm4.size();++i)ib.cpu[i]=pm4[i];
    if(ib.cpu[pm4.size()-1]!=pm4.back())die("CPU write readback",0);
    n48n_ctx cin{N48N_CTX_OP_ALLOC,0,0,0},cout{};size_t csz=sizeof(cout);
    kr=IOConnectCallStructMethod(conn,N48N_SEL_CTX,&cin,sizeof(cin),&cout,&csz);
    if(kr!=KERN_SUCCESS)die("CTX ALLOC",kr);
    struct { n48n_cs_in h; n48n_cs_ib ib; } cs{};
    cs.h={N48N_ABI_VERSION,cout.op,1,N48N_CS_HAS_FENCE,fence.handle,8,0};
    cs.ib={0,0,ib.va,uint32_t(4u*pm4.size()),N48N_HW_IP_COMPUTE,0,0};
    uint64_t seq=0;uint32_t sn=1;
    kr=IOConnectCallMethod(conn,N48N_SEL_SUBMIT,nullptr,0,&cs,sizeof(cs),&seq,&sn,nullptr,nullptr);
    if(kr!=KERN_SUCCESS)die("SUBMIT",kr);
    const uint64_t win[3]={seq,1000000000ull,cout.op};uint64_t wout[3]={0,0,0};uint32_t wn=3;
    kr=IOConnectCallScalarMethod(conn,N48N_SEL_WAITSEQ,win,3,wout,&wn);
    if(kr!=KERN_SUCCESS || wout[0])die("WAITSEQ (busy or error)",kr);
    uint32_t bad=0;
    for(uint32_t y=0;y<gr::kTestSize;++y)for(uint32_t x=0;x<gr::kTestSize;++x)
        if(image.cpu[y*gr::kTestSize+x]!=gfx11_shader::gradientPixel(x,y,gr::kTestSize,gr::kTestSize,frame))++bad;
    const uint64_t userFence=fence.cpu[2]|(uint64_t(fence.cpu[3])<<32);
    printf("seq %llu retired %llu emitted %llu; user fence %llu; %u wrong pixels; pixel(1,1) %08x\n",
           (unsigned long long)seq,(unsigned long long)wout[1],(unsigned long long)wout[2],(unsigned long long)userFence,
           bad,image.cpu[gr::kTestSize+1u]);
    IOServiceClose(conn);
    const bool ok=!bad && userFence==seq;
    puts(ok ? "N48N OK: the RADV-facing path ran the gradient on the RX 7600 (compute queue)" : "N48N: FAILED");
    return ok ? 0 : 9;
}
