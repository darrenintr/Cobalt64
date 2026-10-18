/* SPDX-License-Identifier: MIT
 * PSP13 sOS bootloader load for the RX 7600 (N33_SEL_PSP_LOAD).
 *   sudo n33psp --plan --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin
 *       Reads and checks everything, writes nothing (run.sh does this).
 *   sudo n33psp --load --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin --i-have-ssh
 *       The first GPU write. Needs navi33-psp-load=1 and no navi33-psp-off,
 *       runs once per boot, and should be started over SSH: if the card
 *       stops answering, only a full power-off resets the PSP.
 *   sudo n33psp --ring-plan N --rom ... --fw ...
 *   sudo n33psp --ring N --rom ... --fw ... --i-have-ssh
 *       The ring stage, after the sOS is alive: N=1 creates the KM ring,
 *       2 also sends LOAD_TOC, 3 also SETUP_TMR. --ring-plan writes nothing.
 *       --ring needs navi33-psp-ring=1 and no navi33-psp-off, and runs once
 *       per boot (a rerun resumes the existing ring).
 *   sudo n33psp --ip-fw-plan START STOP --rom ... --fwdir DIR
 *   sudo n33psp --ip-fw START STOP --rom ... --fwdir DIR --i-have-ssh
 *       The IP firmware stage, after the ring and TMR exist: LOAD_IP_FW for
 *       the records [START, STOP) of the 27-record list (0-based). STOP 28
 *       also sends AUTOLOAD_RLC, which needs navi33-psp-autoload=1 on top of
 *       navi33-psp-fw=1. DIR holds the eight decompressed linux-firmware files
 *       (gc_11_0_2_{pfp,me,mec,rlc,imu,mes_2,mes1}.bin, sdma_6_0_2.bin and
 *       smu_13_0_7.bin); the Kext accepts only the pinned ones. Record 26 is
 *       the SMU image, which Linux sends FIRST: run `26 27` before `0 26`.
 * Never run --load, --ring or --ip-fw from run.sh or the boot autorun.
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
    case N33_PSPL_SOS_NOT_ALIVE: return "sOS not alive (C2PMSG_81 = 0): load it first";
    case N33_PSPL_RING_NOT_READY: return "sOS not ready for ring creation (C2PMSG_64)";
    case N33_PSPL_RING_REGS_UNEXPECTED: return "ring registers hold an unexpected ring";
    case N33_PSPL_RING_CREATE_TIMEOUT: return "ring creation got no response in C2PMSG_64";
    case N33_PSPL_CMD_TIMEOUT: return "ring command: fence never arrived";
    case N33_PSPL_CMD_FAILED: return "ring command answered with an error status";
    case N33_PSPL_TOC_BAD: return "TOC missing or the TMR size/address is not acceptable";
    case N33_PSPL_RING_DONE: return "ring stage DONE";
    case N33_PSPL_RING_MISSING: return "our KM ring does not exist: run the ring stage first";
    case N33_PSPL_FW_BAD_IMAGES: return "firmware images are not the pinned set, or a header/offset is bad";
    case N33_PSPL_AUTOLOAD_DISABLED: return "AUTOLOAD_RLC needs navi33-psp-autoload=1 (nothing was sent)";
    case N33_PSPL_FW_DONE: return "firmware stage DONE";
    case N33_PSPL_SMU_NOT_READY: return "SMU mailbox not idle/OK";
    case N33_PSPL_SMU_BAD_PRECONDITION: return "needs the recorded post-autoload stalled IMU state";
    case N33_PSPL_SMU_WRITE_FAILED: return "SMU register write failed; full power-off required";
    case N33_PSPL_SMU_TIMEOUT: return "SMU response timeout; full power-off required";
    case N33_PSPL_SMU_REJECTED: return "SMU rejected a message (see raw replies); full power-off required";
    case N33_PSPL_SMU_ACKED: return "SMU accepted all three messages; RLC boot NOT complete";
    case N33_PSPL_SMU_GC_READY: return "SMU accepted all three messages; RLC boot complete bit set";
    default: return "unknown state";
    }
}
static const char *stepText(uint32_t s) {
    static const char *names[]={"none","KDB","SPL","SYS_DRV","sOS","RING_CREATE",
                                "LOAD_TOC","SETUP_TMR","FW_LOAD","AUTOLOAD_RLC"};
    return s<10u ? names[s] : "?";
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
    if(r->smu_marker==N33_SMU_MARKER) {
        printf("SMU mask %08x:%08x; write gate %s\n",r->smu_mask_high,r->smu_mask_low,
               (r->flags&N33_PSPL_FLAG_WRITE_GATE_OPEN) ? "OPEN" : "closed");
        printf("planned messages: 0x05(high)=%08x, 0x04(low)=%08x, 0x06(enable)=0\n",
               r->smu_mask_high,r->smu_mask_low);
        printf("mailbox before: msg %08x arg %08x response %08x; IMU scratch %08x/%08x\n",
               r->smu_message_before,r->smu_argument_before,r->smu_response_before,
               r->smu_scratch0,r->smu_scratch1);
        printf("core %08x->%08x reset %08x->%08x RLC %08x->%08x\n",
               r->smu_core_before,r->smu_core_after,r->smu_reset_before,r->smu_reset_after,
               r->smu_rlc_before,r->smu_rlc_after);
        printf("completed %u/3, writes %u, last msg/arg %x/%x, responses %x/%x/%x, %u ms\n",
               r->steps_done,r->register_writes,r->smu_last_message,r->smu_last_argument,
               r->smu_responses[0],r->smu_responses[1],r->smu_responses[2],r->elapsed_ms);
        return;
    }
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
    if(r->ring_mc || r->tmr_mc) {
        printf("ring MC 0x%llx, cmd 0x%llx, fence 0x%llx, TMR MC 0x%llx\n",
               (unsigned long long)r->ring_mc,(unsigned long long)r->cmd_mc,
               (unsigned long long)r->fence_mc,(unsigned long long)r->tmr_mc);
        printf("ring regs: C2PMSG_64 %08x C2PMSG_67 %08x; ring flags 0x%02x:%s%s%s%s%s\n",
               r->c2pmsg64_last,r->c2pmsg67_last,r->ring_flags,
               (r->ring_flags&N33_PSPR_FLAG_RESUMED) ? " resumed":"",
               (r->ring_flags&N33_PSPR_FLAG_RING_CREATED) ? " ring-created":"",
               (r->ring_flags&N33_PSPR_FLAG_TOC_LOADED) ? " toc-loaded":"",
               (r->ring_flags&N33_PSPR_FLAG_TMR_SET) ? " tmr-set":"",
               (r->ring_flags&N33_PSPR_FLAG_GATE_OPEN) ? " WRITE-GATE-OPEN":"");
        if(r->ring_flags&N33_PSPR_FLAG_AUTOLOAD_GATE)puts("autoload gate: OPEN (navi33-psp-autoload=1)");
        if(r->ring_flags&N33_PSPR_FLAG_FW_LOADED)puts("ring flags: fw-loaded");
        if(r->fw_record_count) {
            printf("fw: records %u in the list, last sent %u, staging/TMR address 0x%llx\n",
                   r->fw_record_count,r->fw_last_record,
                   (unsigned long long)r->fw_tmr_addr);
            if(r->steps_done || r->register_writes)
                printf("fw: reached %s, %u steps done%s, %u register writes, "
                       "cmd status %08x, fence %u, %u ms\n",
                       stepText(r->step),r->steps_done,
                       (r->ring_flags&N33_PSPR_FLAG_AUTOLOAD_SENT) ? " (AUTOLOAD_RLC sent)":"",
                       r->register_writes,r->cmd_status,r->fence_seen,r->elapsed_ms);
        }
        else if(r->steps_done || r->register_writes)
            printf("ring: reached %s, %u steps done, %u register writes, "
                   "cmd status %08x, fence %u, TOC %u B, TMR %u B, %u ms\n",
                   stepText(r->step),r->steps_done,r->register_writes,
                   r->cmd_status,r->fence_seen,r->toc_bytes,r->tmr_bytes,
                   r->elapsed_ms);
        return;
    }
    if((r->flags&(N33_PSPL_FLAG_WROTE_VRAM|N33_PSPL_FLAG_WROTE_REGISTERS)) ||
       r->register_writes)
        printf("load: reached %s, %u parts done, %u register writes, "
               "C2PMSG_35 last %08x, C2PMSG_81 %08x, sOS version %08x, %u ms\n",
               stepText(r->step),r->steps_done,r->register_writes,
               r->c2pmsg35_last,r->c2pmsg81_after,r->sos_version_after,
               r->elapsed_ms);
}
static int call(io_connect_t conn,uint32_t mode,uint32_t startAt,uint32_t stopAfter,
                const uint8_t *rom,size_t romLen,
                const uint8_t *fw,size_t fwLen,struct n33_psp_load_result *r) {
    const size_t total=sizeof(struct n33_psp_load_request)+romLen+fwLen;
    uint8_t *req=malloc(total);
    if(!req)return 0;
    struct n33_psp_load_request h={N33_ABI_VERSION,sizeof(h),mode,
                                   (uint32_t)romLen,(uint32_t)fwLen,stopAfter,startAt};
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
/* The eight pinned files, in the order Navi33Psp13IpFw.h expects them. */
static const char *const kIpFwFiles[9]={
    "gc_11_0_2_pfp","gc_11_0_2_me","gc_11_0_2_mec","gc_11_0_2_rlc",
    "gc_11_0_2_imu","sdma_6_0_2","gc_11_0_2_mes_2","gc_11_0_2_mes1","smu_13_0_7"};
static uint8_t *slurpIpFw(const char *dir,size_t *len) {
    const size_t max=N33_PSP_LOAD_MAX_INPUT-4096u-sizeof(struct n33_psp_load_request);
    uint8_t *all=malloc(max);
    size_t used=0u;
    if(!all)return NULL;
    for(int k=0;k<9;++k) {
        char path[1024];
        size_t n=0u;
        snprintf(path,sizeof(path),"%s/%s.bin",dir,kIpFwFiles[k]);
        uint8_t *one=slurp(path,max-used,&n);
        if(!one || !n) {
            fprintf(stderr,"Cannot read %s (the eight decompressed firmware files are needed).\n",path);
            free(one);free(all);return NULL;
        }
        memcpy(all+used,one,n);used+=n;free(one);
    }
    *len=used;
    return all;
}
int main(int argc,char **argv) {
    const char *romPath=NULL,*fwPath=NULL,*fwDir=NULL;
    /* mode: 1 plan, 2 load, 3 ring-plan, 4 ring, 5 ip-fw-plan, 6 ip-fw */
    int mode=0,ssh=0,bad=0;
    uint32_t start=0u,stop=0u;
    for(int a=1;a<argc && !bad;++a) {
        const char *arg=argv[a];
        if(!strcmp(arg,"--plan") || !strcmp(arg,"--load") ||
           !strcmp(arg,"--ring-plan") || !strcmp(arg,"--ring") ||
           !strcmp(arg,"--ip-fw-plan") || !strcmp(arg,"--ip-fw") ||
           !strcmp(arg,"--smu-plan") || !strcmp(arg,"--smu-enable")) {
            if(mode) {bad=1;break;}
            mode=!strcmp(arg,"--plan") ? 1 : !strcmp(arg,"--load") ? 2 :
                 !strcmp(arg,"--ring-plan") ? 3 : !strcmp(arg,"--ring") ? 4 :
                 !strcmp(arg,"--ip-fw-plan") ? 5 : !strcmp(arg,"--ip-fw") ? 6 :
                 !strcmp(arg,"--smu-plan") ? 7 : 8;
            if(mode==3 || mode==4) {
                if(a+1>=argc || strlen(argv[a+1])!=1u ||
                   argv[a+1][0]<'1' || argv[a+1][0]>'3') {bad=1;break;}
                stop=(uint32_t)(argv[++a][0]-'0');
            } else if(mode==5 || mode==6) {
                char *e1=NULL,*e2=NULL;
                if(a+2>=argc) {bad=1;break;}
                const unsigned long s1=strtoul(argv[a+1],&e1,10);
                const unsigned long s2=strtoul(argv[a+2],&e2,10);
                if(!*argv[a+1] || *e1 || !*argv[a+2] || *e2 || s2<1ul ||
                   s2>N33_PSP_FW_STOP_AUTOLOAD || s1>=s2) {bad=1;break;}
                start=(uint32_t)s1;stop=(uint32_t)s2;a+=2;
            }
        }
        else if(!strcmp(arg,"--i-have-ssh"))ssh=1;
        else if(!strcmp(arg,"--rom") && a+1<argc)romPath=argv[++a];
        else if(!strcmp(arg,"--fw") && a+1<argc)fwPath=argv[++a];
        else if(!strcmp(arg,"--fwdir") && a+1<argc)fwDir=argv[++a];
        else bad=1;
    }
    const int writes=mode==2 || mode==4 || mode==6 || mode==8;
    const int ipfw=mode==5 || mode==6;
    const int smu=mode==7 || mode==8;
    if(bad || !mode || !romPath || (smu ? (fwDir || fwPath) : ipfw ? (!fwDir || fwPath) : (!fwPath || fwDir)) ||
       (writes && !ssh)) {
        fputs("Usage: sudo n33psp --plan --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin\n"
              "       sudo n33psp --load --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin --i-have-ssh\n"
              "       sudo n33psp --ring-plan N --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin\n"
              "       sudo n33psp --ring N --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin --i-have-ssh\n"
              "       sudo n33psp --ip-fw-plan START STOP --rom rx7600-ipd.bin --fwdir DIR\n"
              "       sudo n33psp --ip-fw START STOP --rom rx7600-ipd.bin --fwdir DIR --i-have-ssh\n"
              "       sudo n33psp --smu-plan --rom rx7600-ipd.bin\n"
              "       sudo n33psp --smu-enable --rom rx7600-ipd.bin --i-have-ssh\n"
              "--plan writes nothing. --load is the first GPU write: run it over SSH;\n"
              "if the card stops answering, power the Mac off fully to reset the PSP.\n"
              "--ring N (1 ring, 2 +LOAD_TOC, 3 +SETUP_TMR) needs the sOS alive.\n"
              "--ip-fw sends records [START, STOP) of 27 (26 = SMU, send it first); STOP 28 adds AUTOLOAD_RLC.\n",
              stderr);
        return 1;
    }
    if(geteuid()!=0) {fputs("N33N requires root/admin.\n",stderr);return 2;}
    size_t romLen=0u,fwLen=0u;
    uint8_t *rom=slurp(romPath,4096u,&romLen);
    uint8_t *fw=smu ? calloc(1u,1u) : ipfw ? slurpIpFw(fwDir,&fwLen) :
                slurp(fwPath,N33_PSP_LOAD_MAX_INPUT-4096u-
                      sizeof(struct n33_psp_load_request),&fwLen);
    if(smu)fwLen=1u; // reserved zero byte; SMU has no firmware payload
    if(!rom || romLen<64u || !fw || !fwLen) {
        fputs("Cannot read the IP Discovery (64 B to 4 KiB) or the firmware.\n",stderr);
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
    const int ringStage=mode==3 || mode==4;
    const uint32_t planMode=smu ? N33_SMU_ENABLE_PLAN : ipfw ? N33_PSP_LOAD_FW_PLAN : ringStage ? N33_PSP_LOAD_RING_PLAN :
                            N33_PSP_LOAD_PLAN;
    const uint32_t runMode=smu ? N33_SMU_ENABLE_RUN : ipfw ? N33_PSP_LOAD_FW_RUN : ringStage ? N33_PSP_LOAD_RING_RUN :
                           N33_PSP_LOAD_RUN;
    struct n33_psp_load_result r;
    int rc=7;
    puts(smu ? "== SMU13 feature enable plan (read-only)" : ipfw ? "== PSP13 IP firmware plan (read-only)" :
         ringStage ? "== PSP13 KM ring plan (read-only)" : "== PSP13 sOS load plan (read-only)");
    if(call(conn,planMode,start,stop,rom,romLen,fw,fwLen,&r)) {
        print(&r);
        rc=r.state==N33_PSPL_PLAN_OK ? 0 : 7;
    }
    if(writes && rc==0) {
        const uint32_t gate=(ringStage||ipfw) ? (r.ring_flags&N33_PSPR_FLAG_GATE_OPEN)
                                              : (r.flags&N33_PSPL_FLAG_WRITE_GATE_OPEN);
        if(!gate) {
            puts(smu ? "Write gate closed: needs navi33-smu-enable=1, no navi33-smu-off or navi33-psp-off." :
                 ipfw ? "Write gate closed: boot with navi33-psp-fw=1 and without navi33-psp-off=1." :
                 ringStage ?
                 "Write gate closed: boot with navi33-psp-ring=1 and without navi33-psp-off=1." :
                 "Write gate closed: boot with navi33-psp-load=1 and without navi33-psp-off=1.");
            rc=6;
        } else if(ipfw && stop==N33_PSP_FW_STOP_AUTOLOAD &&
                  !(r.ring_flags&N33_PSPR_FLAG_AUTOLOAD_GATE)) {
            puts("STOP 28 sends AUTOLOAD_RLC: boot with navi33-psp-autoload=1 as well. Nothing was sent.");
            rc=6;
        } else {
            puts(smu ? "== SMU13 ENABLE (writes MP1 C2PMSG_66/82/90; full power-off after attempt)" :
                 ipfw ? "== PSP13 IP FIRMWARE (writes the ring write pointer and the staging area)" :
                 ringStage ?
                 "== PSP13 KM RING (writes C2PMSG_64/67/69/70/71 and the 2 MiB ring window)" :
                 "== PSP13 sOS LOAD (writes C2PMSG_35/36 and the fw_pri window)");
            fflush(stdout);
            rc=7;
            if(call(conn,runMode,start,stop,rom,romLen,fw,fwLen,&r)) {
                print(&r);
                rc=smu ? (r.state==N33_PSPL_SMU_GC_READY ? 0 : 9) : r.state==(ipfw ? N33_PSPL_FW_DONE : ringStage ? N33_PSPL_RING_DONE :
                             N33_PSPL_SOS_ALIVE) ? 0 : 9;
            }
        }
    } else if(writes) {
        puts("Plan refused: no write was attempted.");
    }
    IOServiceClose(conn);
    free(rom);free(fw);
    puts(!writes ? "GPU writes: NONE; plan only." :
         smu ? "GPU writes: SMU mailbox only (see write count); full power-off after an attempt." :
         ipfw ? "GPU writes: PSP13 ring write pointer and the firmware staging area (see flags)." :
         ringStage ? "GPU writes: PSP13 KM ring registers and the ring window (see flags)." :
                     "GPU writes: PSP13 bootloader handshake only (see flags).");
    return rc;
}
