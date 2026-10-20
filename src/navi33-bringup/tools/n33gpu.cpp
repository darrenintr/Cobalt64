// SPDX-License-Identifier: MIT
// n33gpu: the first user program that drives the RX 7600 itself (Phase B4). Through the
// N33N client GPU selectors it opens a context (its own VMID, queue and page tables),
// creates buffers, maps them at GPU addresses, writes the gradient kernel and its
// arguments through CPU mappings, builds the PM4 command buffer in user space, submits
// it, waits for the fence and checks every pixel against the CPU reference.
// Needs navi33-client-gpu=1 and a card the autostart brought up.
//   sudo n33gpu --fw gradient.co [--frame N] [--repeat N]
#include "../src/Navi33Gfx11Gradient.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
using namespace navi33;
namespace gd=gfx11_dispatch;
namespace gr=gradient;

static io_connect_t conn=IO_OBJECT_NULL;
static kern_return_t call(uint32_t sel,std::vector<uint64_t> in,uint32_t outCount,uint64_t *out) {
    uint32_t n=outCount;
    return IOConnectCallScalarMethod(conn,sel,in.data(),static_cast<uint32_t>(in.size()),out,out ? &n : nullptr);
}
static void die(const char *what,kern_return_t kr) {
    fprintf(stderr,"%s failed: 0x%x\n",what,kr);
    if(conn!=IO_OBJECT_NULL){uint64_t v=0;call(N33_SEL_CTX_CREATE,{N33_CTX_OP_CLOSE},1,&v);IOServiceClose(conn);}
    exit(9);
}
struct Buffer { uint32_t handle=0; uint64_t size=0, va=0; volatile uint32_t *cpu=nullptr; };
static Buffer buffer(uint64_t bytes,uint64_t va,uint32_t flags) {
    Buffer b{};uint64_t out[2]={0,0};
    kern_return_t kr=call(N33_SEL_BO_CREATE,{bytes,0u},2,out);
    if(kr!=KERN_SUCCESS)die("BO_CREATE",kr);
    b.handle=static_cast<uint32_t>(out[0]);b.size=out[1];b.va=va;
    kr=call(N33_SEL_VM_MAP,{b.handle,va,flags,N33_VM_OP_MAP},0,nullptr);
    if(kr!=KERN_SUCCESS)die("VM_MAP",kr);
    mach_vm_address_t addr=0;mach_vm_size_t size=0;
    kr=IOConnectMapMemory64(conn,b.handle,mach_task_self(),&addr,&size,kIOMapAnywhere|kIOMapInhibitCache);
    if(kr!=KERN_SUCCESS || size<b.size)die("IOConnectMapMemory64",kr);
    b.cpu=reinterpret_cast<volatile uint32_t *>(addr);
    return b;
}
int main(int argc,char **argv) {
    const char *fw=nullptr;uint32_t frame=11u,repeat=1u;bool nop=false;
    for(int a=1;a<argc;++a) {
        if(!strcmp(argv[a],"--fw") && a+1<argc)fw=argv[++a];
        else if(!strcmp(argv[a],"--frame") && a+1<argc)frame=static_cast<uint32_t>(strtoul(argv[++a],nullptr,0))&0xffu;
        else if(!strcmp(argv[a],"--repeat") && a+1<argc)repeat=static_cast<uint32_t>(strtoul(argv[++a],nullptr,0));
        else if(!strcmp(argv[a],"--nop"))nop=true;
        else {fw=nullptr;break;}
    }
    if(!fw || !repeat || repeat>100000u) {
        fputs("Usage: sudo n33gpu --fw gradient.co [--frame N] [--repeat N] [--nop]\n"
              "--nop submits an IB of NOPs only (queue fetch + fence, no dispatch).\n"
              "Opens a GPU context (navi33-client-gpu=1), runs the gradient from user space\n"
              "and checks every pixel. Writes only its own buffers and its own queue.\n",stderr);
        return 1;
    }
    if(geteuid()!=0){fputs("N33N requires root/admin.\n",stderr);return 2;}
    FILE *f=fopen(fw,"rb");
    std::vector<uint8_t> co;
    if(f){uint8_t buf[4096];size_t n;while((n=fread(buf,1,sizeof(buf),f))>0)co.insert(co.end(),buf,buf+n);fclose(f);}
    const gfx11_shader::Kernel k=gr::kernelOf(co.data(),co.size());
    if(k.code!=gfx11_shader::Code::Ok){fputs("gradient.co is not the pinned kernel.\n",stderr);return 8;}
    io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!svc){fputs("Navi33Probe service not loaded.\n",stderr);return 3;}
    kern_return_t kr=IOServiceOpen(svc,mach_task_self(),N33_UC_TYPE,&conn);
    IOObjectRelease(svc);
    if(kr!=KERN_SUCCESS){fprintf(stderr,"N33N open refused: 0x%x\n",kr);return 4;}
    uint64_t hello[3]={0,0,0};
    kr=call(N33_SEL_HELLO,{N33_ABI_VERSION,0u},3,hello);
    if(kr!=KERN_SUCCESS || hello[0]!=N33_ABI_VERSION){fputs("N33N ABI version mismatch.\n",stderr);IOServiceClose(conn);return 5;}
    n33_capabilities caps{};size_t capsBytes=sizeof(caps);
    kr=IOConnectCallStructMethod(conn,N33_SEL_QUERY_CAPS,nullptr,0,&caps,&capsBytes);
    if(kr!=KERN_SUCCESS || !(caps.capabilities&N33_CAP_CLIENT_GPU)) {
        fputs("Client GPU contexts are off: boot with navi33-client-gpu=1 (and no navi33-psp-off).\n",stderr);
        IOServiceClose(conn);return 6;
    }
    uint64_t vmid=0;
    kr=call(N33_SEL_CTX_CREATE,{N33_CTX_OP_OPEN},1,&vmid);
    if(kr!=KERN_SUCCESS)die("CTX_CREATE (needs the autostart state; see the kernel log)",kr);
    printf("context: VMID %llu\n",static_cast<unsigned long long>(vmid));
    const uint64_t base=0x100000000ull;
    Buffer shader=buffer(gr::kShaderMax,base,N33_VM_MAP_EXEC);
    Buffer args=buffer(4096,base+0x10000,N33_VM_MAP_WRITE);
    Buffer image=buffer(gr::kImageBytes,base+0x20000,N33_VM_MAP_WRITE);
    Buffer ib=buffer(4096,base+0x30000,0u);
    // The kernel code (zero tail), the arguments, a cleared image.
    for(uint32_t i=0;i<gr::kShaderMax/4u;++i)shader.cpu[i]=0u;
    for(uint64_t i=0;i<k.codeBytes;i+=4) {
        uint32_t w=0;for(uint64_t b=0;b<4 && i+b<k.codeBytes;++b)w|=uint32_t(co[k.codeOffset+i+b])<<(8u*b);
        shader.cpu[i/4u]=w;
    }
    uint32_t argv6[6];gr::args(argv6,image.va,gr::kTestSize,gr::kTestSize,gr::kTestSize,frame);
    for(int i=0;i<6;++i)args.cpu[i]=argv6[i];
    // The command buffer, built here: thread masks and the dispatch (the kernel adds the fence).
    gd::Request rq{};rq.kernel=&k;rq.shaderMc=shader.va;rq.kernargMc=args.va;
    rq.groupsX=gr::kTestSize/8u;rq.groupsY=gr::kTestSize/8u;rq.fenceMc=args.va+0x800u;rq.fenceValue=1u;
    const gd::Stream s=gd::encode(rq);
    if(s.code!=gd::Code::Ok){fputs("PM4 encoding failed.\n",stderr);return 8;}
    std::vector<uint32_t> pm4={gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe0),~0u,~0u,
                               gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe2),~0u,~0u};
    for(size_t i=0;i+8<s.count;++i)pm4.push_back(s.dwords[i]);       // without the encoder's RELEASE_MEM
    if(nop)pm4.assign(8,gd::packet3(0x10u,0x3fffu));                    // NOPs: fetch and fence only
    for(size_t i=0;i<pm4.size();++i)ib.cpu[i]=pm4[i];
    // Read back once so the posted BAR writes reach VRAM before the GPU fetches them.
    if(ib.cpu[pm4.size()-1]!=pm4.back() || shader.cpu[0]==0u)die("CPU write readback",0);
    uint32_t bad=0;uint64_t seq=0;
    const auto t0=std::chrono::steady_clock::now();
    for(uint32_t r=0;r<repeat;++r) {
        for(uint32_t i=0;i<gr::kImageBytes/4u;++i)image.cpu[i]=0u;
        kr=call(N33_SEL_SUBMIT,{1u,ib.va,pm4.size()},1,&seq);
        if(kr!=KERN_SUCCESS)die("SUBMIT",kr);
        uint64_t w[2]={0,0};
        kr=call(N33_SEL_WAIT,{seq,1000000u},2,w);
        if(kr!=KERN_SUCCESS)die("WAIT",kr);
        if(w[1]!=N33_GPU_OK) {
            fprintf(stderr,"WAIT state %llu (1 timeout, 2 GPU fault, 3 quarantined), retired %llu of %llu\n",
                    static_cast<unsigned long long>(w[1]),static_cast<unsigned long long>(w[0]),
                    static_cast<unsigned long long>(seq));
            die("the submission",0);
        }
        for(uint32_t y=0;y<gr::kTestSize && !nop;++y)for(uint32_t x=0;x<gr::kTestSize;++x)
            if(image.cpu[y*gr::kTestSize+x]!=gfx11_shader::gradientPixel(x,y,gr::kTestSize,gr::kTestSize,frame))++bad;
        if(bad)break;
    }
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
    printf("submitted %u, last sequence %llu, %u wrong pixels, pixel(1,1) %08x, %.3f ms per submit+wait+check\n",
           repeat,static_cast<unsigned long long>(seq),bad,image.cpu[gr::kTestSize+1u],ms/repeat);
    uint64_t v=0;
    kr=call(N33_SEL_CTX_CREATE,{N33_CTX_OP_CLOSE},1,&v);
    printf("context closed: 0x%x\n",kr);
    IOServiceClose(conn);
    if(nop){puts("CLIENT GPU NOP OK: the queue fetched the IB and wrote the fence");return 0;}
    puts(bad ? "CLIENT GPU: FAILED (pixels differ)" :
               "CLIENT GPU OK: a user program ran the gradient on its own context and every pixel matched");
    return bad ? 9 : 0;
}
