/* SPDX-License-Identifier: MIT
 * Root-only reads of allowlisted RX 7600 register windows (N33_SEL_MMIO_PEEK).
 * New read-only checks only need this tool, not a new Kext: the Kext
 * refuses anything outside src/Navi33PeekWindows.h and never writes.
 *   n33peek --list
 *   sudo n33peek --rom rx7600-ipd.bin --all
 *   sudo n33peek --rom rx7600-ipd.bin PSP_C2PMSG:3:1 GRBM_STATUS
 * NAME[:first[:count]] selects dwords within one window.
 */
#include "../src/Navi33NativeABI.h"
#include "../src/Navi33PeekWindows.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *stateText(uint32_t s) {
    switch(s) {
    case N33_PEEK_DISABLED: return "disabled (needs navi33-hw-probe=1, no navi33-hw-off)";
    case N33_PEEK_BAD_REQUEST: return "bad request";
    case N33_PEEK_NOT_ALLOWED: return "outside the allowlisted window";
    case N33_PEEK_NO_PROVIDER: return "RX7600 PCI provider unavailable";
    case N33_PEEK_BAD_ROM: return "IP Discovery invalid or IP version differs";
    case N33_PEEK_NO_BAR5: return "register outside BAR5";
    case N33_PEEK_MAP_FAILED: return "cannot map BAR5";
    case N33_PEEK_READ_FAILED: return "BAR5 read failed";
    case N33_PEEK_DONE: return "read";
    default: return "unknown state";
    }
}
static const char *ipName(uint16_t hwid) {
    switch(hwid) {
    case N33_HWID_MP0: return "MP0";
    case N33_HWID_MP1: return "MP1";
    case N33_HWID_GC: return "GC";
    case N33_HWID_MMHUB: return "MMHUB";
    case N33_HWID_NBIF: return "NBIF";
    default: return "?";
    }
}
static void list(void) {
    for(uint32_t i=0;i<N33_PEEK_WINDOW_COUNT;++i) {
        const struct n33_peek_window *w=&n33_peek_windows[i];
        printf("%-22s %-5s %u.%u.%u seg%u reg 0x%04x x%u\n",w->name,
               ipName(w->hwid),w->major,w->minor,w->revision,w->segment,
               w->first,w->count);
    }
}
/* req holds the 32-byte header followed by the IP Discovery bytes. */
static int peekWindow(io_connect_t conn,uint8_t *req,size_t reqLen,
                      uint32_t index,uint32_t first,uint32_t count) {
    const struct n33_peek_window *w=&n33_peek_windows[index];
    int zero=1, ones=1;
    printf("== %s  %s seg%u reg 0x%04x x%u\n",w->name,ipName(w->hwid),
           w->segment,w->first+first,count);
    for(uint32_t done=0;done<count;) {
        const uint32_t n=count-done<N33_PEEK_MAX_DWORDS ?
            count-done : N33_PEEK_MAX_DWORDS;
        struct n33_peek_request h={N33_ABI_VERSION,sizeof(h),index,
                                   first+done,n,(uint32_t)(reqLen-sizeof(h)),0u};
        memcpy(req,&h,sizeof(h));
        struct n33_peek_result r;
        size_t size=sizeof(r);
        memset(&r,0,sizeof(r));
        const kern_return_t kr=IOConnectCallStructMethod(
            conn,N33_SEL_MMIO_PEEK,req,reqLen,&r,&size);
        if(kr!=KERN_SUCCESS || size!=sizeof(r) ||
           r.abi_version!=N33_ABI_VERSION || r.size!=sizeof(r)) {
            printf("  call failed: 0x%x\n",kr);
            return 0;
        }
        if(r.state!=N33_PEEK_DONE || r.reads!=n) {
            printf("  state %u (%s), %u of %u read\n",r.state,
                   stateText(r.state),r.reads,n);
            return 0;
        }
        for(uint32_t i=0;i<n;++i) {
            if(i%8u==0u)printf("  0x%04x:",w->first+first+done+i);
            printf(" %08x",r.values[i]);
            if(i%8u==7u || i+1u==n)putchar('\n');
            zero&=r.values[i]==0u;
            ones&=r.values[i]==UINT32_MAX;
        }
        done+=n;
    }
    if(zero)puts("  (all zero)");
    if(ones)puts("  (all ones: block not answering)");
    return 1;
}
static int parseSpec(const char *spec,uint32_t *index,uint32_t *first,
                     uint32_t *count) {
    const char *colon=strchr(spec,':');
    const size_t len=colon ? (size_t)(colon-spec) : strlen(spec);
    for(uint32_t i=0;i<N33_PEEK_WINDOW_COUNT;++i) {
        const struct n33_peek_window *w=&n33_peek_windows[i];
        if(strlen(w->name)!=len || strncmp(w->name,spec,len))continue;
        char *end=NULL;
        unsigned long f=0ul, c=0ul;
        if(colon) {
            f=strtoul(colon+1,&end,0);
            if(*end==':')c=strtoul(end+1,&end,0);
            else c=1ul;
            if(*end)return 0;
        }
        if(f>=w->count)return 0;
        if(!colon)c=w->count;
        if(!c || c>w->count-f)return 0;
        *index=i;*first=(uint32_t)f;*count=(uint32_t)c;
        return 1;
    }
    return 0;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--list")) {list();return 0;}
    if(argc<4 || strcmp(argv[1],"--rom")) {
        fputs("Usage: n33peek --list\n"
              "       sudo n33peek --rom rx7600-ipd.bin --all | NAME[:first[:count]] ...\n"
              "Reads allowlisted registers only; never writes the GPU.\n",stderr);
        return 1;
    }
    uint32_t specs[3*64];
    uint32_t nspec=0u;
    const int all=argc==4 && !strcmp(argv[3],"--all");
    for(int a=3;!all && a<argc;++a) {
        if(nspec>=64u || !parseSpec(argv[a],&specs[3*nspec],
                                    &specs[3*nspec+1],&specs[3*nspec+2])) {
            fprintf(stderr,"Unknown window or range: %s (see --list)\n",argv[a]);
            return 1;
        }
        ++nspec;
    }
    if(geteuid()!=0) {fputs("N33N requires root/admin.\n",stderr);return 2;}
    FILE *fp=fopen(argv[2],"rb");
    if(!fp) {perror("Cannot open IP Discovery file");return 8;}
    uint8_t *req=malloc(4096u);
    const size_t hdr=sizeof(struct n33_peek_request);
    const size_t romLen=req ? fread(req+hdr,1u,4096u-hdr,fp) : 0u;
    const int more=fgetc(fp)!=EOF;
    fclose(fp);
    if(!req || romLen<64u || more) {
        fputs("IP Discovery file must be 64 bytes to 4064 bytes "
              "(use tools/ipd-from-capture.py output, not a full VBIOS).\n",stderr);
        free(req);return 8;
    }
    io_service_t svc=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!svc) {fputs("Navi33Probe service not loaded.\n",stderr);free(req);return 3;}
    io_connect_t conn=IO_OBJECT_NULL;
    kern_return_t kr=IOServiceOpen(svc,mach_task_self(),N33_UC_TYPE,&conn);
    IOObjectRelease(svc);
    if(kr!=KERN_SUCCESS) {
        fprintf(stderr,"N33N open refused (boot flags?): 0x%x\n",kr);
        free(req);return 4;
    }
    const uint64_t in[2]={N33_ABI_VERSION,0u};
    uint64_t hello[3]={0,0,0};uint32_t count=3u;
    kr=IOConnectCallScalarMethod(conn,N33_SEL_HELLO,in,2u,hello,&count);
    if(kr!=KERN_SUCCESS || count!=3u || hello[0]!=N33_ABI_VERSION ||
       hello[1]!=N33_UC_TYPE || hello[2]!=N33_SEL_COUNT) {
        fputs("N33N ABI version mismatch (Kext and n33peek must match).\n",stderr);
        IOServiceClose(conn);free(req);return 5;
    }
    int ok=1;
    if(all) {
        for(uint32_t i=0;i<N33_PEEK_WINDOW_COUNT;++i)
            ok&=peekWindow(conn,req,hdr+romLen,i,0u,n33_peek_windows[i].count);
    } else {
        for(uint32_t s=0;s<nspec;++s)
            ok&=peekWindow(conn,req,hdr+romLen,specs[3*s],specs[3*s+1],
                           specs[3*s+2]);
    }
    IOServiceClose(conn);
    free(req);
    puts("GPU writes: NONE; allowlisted BAR5 reads only.");
    return ok ? 0 : 7;
}
