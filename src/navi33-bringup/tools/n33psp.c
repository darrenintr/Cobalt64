/* SPDX-License-Identifier: MIT
 * PSP13 sOS bootloader load for the RX 7600 (N33_SEL_PSP_LOAD).
 *   sudo n33psp --plan --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin
 *       Reads and checks everything, writes nothing (run.sh does this).
 *   sudo n33psp --load --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin --i-have-ssh
 *       The first GPU write. Needs navi33-psp-load=1 and no navi33-psp-off,
 *       runs once per boot, and should be started over SSH: if the card
 *       stops answering, only a full power-off resets the PSP.
 * Never run --load from run.sh or the boot autorun.
 */
#include "../src/Navi33NativeABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *stateText(uint32_t s) {
    switch(s) {
    case N33_PSPL_DISABLED: return "disabled (needs navi33-hw-probe=1, no navi33-hw-off)";
    case N33_PSPL_BAD_REQUEST: return "bad request";
    case N33_PSPL_NO_PROVIDER: return "RX7600 PCI provider unavailable";
    case N33_PSPL_BAD_ROM: return "IP Discovery invalid or IP version differs";
    case N33_PSPL_BAD_FIRMWARE: return "not the pinned psp_13_0_7_sos.bin";
    case N33_PSPL_NO_BARS: return "BAR0 or BAR5 not assigned";
    case N33_PSPL_MAP_FAILED: return "cannot map BAR5 or the fw_pri window";
    case N33_PSPL_READ_FAILED: return "a register read failed or read all-ones";
    case N33_PSPL_SOS_ALREADY_ALIVE: return "sOS already alive: nothing to load";
    case N33_PSPL_BOOTLOADER_BUSY: return "bootloader not at 0x80000000";
    case N33_PSPL_BAD_FB_LOCATION: return "FB location not posted or MMHUB/DCN disagree";
    case N33_PSPL_NO_CONSOLE: return "GOP console not found inside BAR0";
    case N33_PSPL_APERTURE_MISMATCH: return "no scanout surface at the console's BAR0 offset";
    case N33_PSPL_NO_WINDOW: return "no safe 1 MiB fw_pri window";
    case N33_PSPL_PLAN_OK: return "plan OK, nothing written";
    case N33_PSPL_WRITE_DISABLED: return "plan OK, but the write gate is closed (navi33-psp-load=1, no navi33-psp-off)";
    case N33_PSPL_ALREADY_ATTEMPTED: return "a load already ran this boot: power the Mac off fully first";
    case N33_PSPL_STAGE_VERIFY_FAILED: return "fw_pri read-back mismatch (no command sent for that part)";
    case N33_PSPL_BOOTLOADER_TIMEOUT: return "bootloader did not return to 0x80000000";
    case N33_PSPL_SOS_TIMEOUT: return "sOS did not sign on (C2PMSG_81 stayed 0)";
    case N33_PSPL_SOS_ALIVE: return "sOS ALIVE";
    default: return "unknown state";
    }
}
static const char *stepText(uint32_t s) {
    static const char *names[]={"none","KDB","SPL","SYS_DRV","sOS"};
    return s<5u ? names[s] : "?";
}
static uint8_t *slurp(const char *path,size_t max,size_t *len) {
    FILE *fp=fopen(path,"rb");
    if(!fp)return NULL;
    uint8_t *buf=malloc(max+1u);
    *len=buf ? fread(buf,1u,max+1u,fp) : 0u;
    fclose(fp);
    if(!buf || *len>max) {free(buf);return NULL;}
    return buf;
}
static void print(const struct n33_psp_load_result *r) {
    printf("state %u: %s\n",r->state,stateText(r->state));
    printf("flags 0x%02x:%s%s%s%s%s%s%s\n",r->flags,
           (r->flags&N33_PSPL_FLAG_FW_PINNED) ? " fw-pinned":"",
           (r->flags&N33_PSPL_FLAG_CONSOLE_FOUND) ? " console-found":"",
           (r->flags&N33_PSPL_FLAG_APERTURE_MATCH) ? " aperture-match":"",
           (r->flags&N33_PSPL_FLAG_DCN_FB_MATCH) ? " dcn-fb-match":"",
           (r->flags&N33_PSPL_FLAG_WRITE_GATE_OPEN) ? " WRITE-GATE-OPEN":"",
           (r->flags&N33_PSPL_FLAG_WROTE_VRAM) ? " WROTE-VRAM":"",
           (r->flags&N33_PSPL_FLAG_WROTE_REGISTERS) ? " WROTE-REGISTERS":"");
    printf("firmware: sOS 0x%08x (%u B), KDB %u B, SPL %u B, SYS_DRV %u B\n",
           r->fw_sos_version,r->fw_sos_bytes,r->fw_kdb_bytes,
           r->fw_spl_bytes,r->fw_sys_bytes);
    printf("PSP before: C2PMSG_35 %08x C2PMSG_81 %08x\n",
           r->c2pmsg35_before,r->c2pmsg81_before);
    printf("MMHUB FB_LOCATION %08x-%08x, DCN FB base %08x\n",
           r->mm_fb_base,r->mm_fb_top,r->dcn_fb_base);
    for(unsigned i=0;i<4u;++i)
        printf("HUBP%u surface MC 0x%010llx%s\n",i,
               (unsigned long long)r->surface_mc[i],
               (r->active_pipes&(1u<<i)) ? " (active)":"");
    printf("BAR0 0x%llx + %llu MiB; console 0x%llx (%llu KiB) = vram+0x%llx\n",
           (unsigned long long)r->bar0_phys,
           (unsigned long long)(r->bar0_bytes>>20),
           (unsigned long long)r->console_phys,
           (unsigned long long)(r->console_bytes>>10),
           (unsigned long long)r->console_vram_offset);
    printf("fw_pri vram+0x%llx = MC 0x%llx (C2PMSG_36 0x%08llx)\n",
           (unsigned long long)r->fw_pri_vram_offset,
           (unsigned long long)r->fw_pri_mc,
           (unsigned long long)(r->fw_pri_mc>>20));
    if((r->flags&(N33_PSPL_FLAG_WROTE_VRAM|N33_PSPL_FLAG_WROTE_REGISTERS)) ||
       r->register_writes)
        printf("load: reached %s, %u parts done, %u register writes, "
               "C2PMSG_35 last %08x, C2PMSG_81 %08x, sOS version %08x, %u ms\n",
               stepText(r->step),r->steps_done,r->register_writes,
               r->c2pmsg35_last,r->c2pmsg81_after,r->sos_version_after,
               r->elapsed_ms);
}
static int call(io_connect_t conn,uint32_t mode,const uint8_t *rom,size_t romLen,
                const uint8_t *fw,size_t fwLen,struct n33_psp_load_result *r) {
    const size_t total=sizeof(struct n33_psp_load_request)+romLen+fwLen;
    uint8_t *req=malloc(total);
    if(!req)return 0;
    struct n33_psp_load_request h={N33_ABI_VERSION,sizeof(h),mode,
                                   (uint32_t)romLen,(uint32_t)fwLen,0u,0u};
    memcpy(req,&h,sizeof(h));
    memcpy(req+sizeof(h),rom,romLen);
    memcpy(req+sizeof(h)+romLen,fw,fwLen);
    size_t size=sizeof(*r);
    memset(r,0,sizeof(*r));
    const kern_return_t kr=IOConnectCallStructMethod(conn,N33_SEL_PSP_LOAD,
                                                     req,total,r,&size);
    free(req);
    if(kr!=KERN_SUCCESS || size!=sizeof(*r) ||
       r->abi_version!=N33_ABI_VERSION || r->size!=sizeof(*r)) {
        printf("call failed: 0x%x\n",kr);
        return 0;
    }
    return 1;
}
int main(int argc,char **argv) {
    const char *romPath=NULL,*fwPath=NULL;
    int plan=0,load=0,ssh=0;
    for(int a=1;a<argc;++a) {
        if(!strcmp(argv[a],"--plan"))plan=1;
        else if(!strcmp(argv[a],"--load"))load=1;
        else if(!strcmp(argv[a],"--i-have-ssh"))ssh=1;
        else if(!strcmp(argv[a],"--rom") && a+1<argc)romPath=argv[++a];
        else if(!strcmp(argv[a],"--fw") && a+1<argc)fwPath=argv[++a];
        else {plan=load=0;break;}
    }
    if(plan==load || !romPath || !fwPath || (load && !ssh)) {
        fputs("Usage: sudo n33psp --plan --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin\n"
              "       sudo n33psp --load --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin --i-have-ssh\n"
              "--plan writes nothing. --load is the first GPU write: run it over SSH;\n"
              "if the card stops answering, power the Mac off fully to reset the PSP.\n",
              stderr);
        return 1;
    }
    if(geteuid()!=0) {fputs("N33N requires root/admin.\n",stderr);return 2;}
    size_t romLen=0u,fwLen=0u;
    uint8_t *rom=slurp(romPath,4096u,&romLen);
    uint8_t *fw=slurp(fwPath,N33_PSP_LOAD_MAX_INPUT-4096u-
                      sizeof(struct n33_psp_load_request),&fwLen);
    if(!rom || romLen<64u || !fw || !fwLen) {
        fputs("Cannot read the IP Discovery (64 B to 4 KiB) or firmware file.\n",stderr);
        free(rom);free(fw);return 8;
    }
    io_service_t svc=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if(!svc) {fputs("Navi33Probe service not loaded.\n",stderr);free(rom);free(fw);return 3;}
    io_connect_t conn=IO_OBJECT_NULL;
    kern_return_t kr=IOServiceOpen(svc,mach_task_self(),N33_UC_TYPE,&conn);
    IOObjectRelease(svc);
    if(kr!=KERN_SUCCESS) {
        fprintf(stderr,"N33N open refused (boot flags?): 0x%x\n",kr);
        free(rom);free(fw);return 4;
    }
    const uint64_t in[2]={N33_ABI_VERSION,0u};
    uint64_t hello[3]={0,0,0};uint32_t count=3u;
    kr=IOConnectCallScalarMethod(conn,N33_SEL_HELLO,in,2u,hello,&count);
    if(kr!=KERN_SUCCESS || count!=3u || hello[0]!=N33_ABI_VERSION ||
       hello[1]!=N33_UC_TYPE || hello[2]!=N33_SEL_COUNT) {
        fputs("N33N ABI version mismatch (Kext and n33psp must match).\n",stderr);
        IOServiceClose(conn);free(rom);free(fw);return 5;
    }
    struct n33_psp_load_result r;
    int rc=7;
    puts("== PSP13 sOS load plan (read-only)");
    if(call(conn,N33_PSP_LOAD_PLAN,rom,romLen,fw,fwLen,&r)) {
        print(&r);
        rc=r.state==N33_PSPL_PLAN_OK ? 0 : 7;
    }
    if(load && rc==0) {
        if(!(r.flags&N33_PSPL_FLAG_WRITE_GATE_OPEN)) {
            puts("Write gate closed: boot with navi33-psp-load=1 and without navi33-psp-off=1.");
            rc=6;
        } else {
            puts("== PSP13 sOS LOAD (writes C2PMSG_35/36 and the fw_pri window)");
            fflush(stdout);
            rc=7;
            if(call(conn,N33_PSP_LOAD_RUN,rom,romLen,fw,fwLen,&r)) {
                print(&r);
                rc=r.state==N33_PSPL_SOS_ALIVE ? 0 : 9;
            }
        }
    } else if(load) {
        puts("Plan refused: no write was attempted.");
    }
    IOServiceClose(conn);
    free(rom);free(fw);
    puts(load ? "GPU writes: PSP13 bootloader handshake only (see flags)."
              : "GPU writes: NONE; plan only.");
    return rc;
}
