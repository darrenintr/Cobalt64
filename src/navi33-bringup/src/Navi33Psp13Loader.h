// SPDX-License-Identifier: MIT
// PSP13 (MP0 13.0.7) sOS bootloader load: the only GPU write stage in this
// Kext, approved by the user on 2026-10-18. Host-testable: all I/O goes
// through the caller's callbacks; Navi33DarwinPspLoad.cpp supplies them.
//
// Protocol: Linux psp_v13_0.c + amdgpu_psp.c psp_hw_start(), for each part
// present in psp_13_0_7_sos.bin (KDB, SPL, SYS_DRV, then sOS):
//   wait C2PMSG_35 == 0x80000000; zero the 1 MiB fw_pri buffer and copy the
//   part in; C2PMSG_36 = fw_pri MC >> 20; C2PMSG_35 = bootloader command;
//   wait for 0x80000000 again (sOS: wait 20 ms, then C2PMSG_81 != 0).
// Upstream psp_v13_0_bootloader_load_spl() hands the bootloader &psp->kdb,
// not &psp->spl, with PSP_BL__LOAD_TOS_SPL_TABLE. Every Linux RX 7600 boot
// does that, so it is copied here on purpose (psp_v14_0 passes the SPL).
// fw_pri sits in VRAM (Linux: debug_use_vram_fw_buf / SR-IOV path).
// Not done: DRAM long training (PSP_BL__DRAM_LONG_TRAIN, only when the
// VBIOS asks), TOC/TMR, the KM ring and every later PSP command.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33Dcn321.h"
#include "Navi33Gmc11VramLive.h"
#include "Navi33NativeABI.h"
#include "Navi33Psp13Firmware.h"
#include "Navi33Sha256.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace psp13load {
// mp_13_0_2_offset.h (psp_v13_0.c includes it for 13.0.7), MP0 BASE_IDX 0.
constexpr uint32_t kC2pmsg35=0x0063u; // bootloader command / status
constexpr uint32_t kC2pmsg36=0x0064u; // fw_pri MC address >> 20
constexpr uint32_t kC2pmsg58=0x007au; // sOS version
constexpr uint32_t kC2pmsg81=0x0091u; // sOS sign of life
// amdgpu_psp.h enum psp_bootloader_cmd.
constexpr uint32_t kLoadSysDrv=0x00010000u, kLoadSos=0x00020000u;
constexpr uint32_t kLoadKdb=0x00080000u, kLoadSpl=0x10000000u;
constexpr uint32_t kBootloaderReady=0x80000000u;
constexpr uint64_t kFwPriBytes=1024u*1024u;      // PSP_1_MEG
constexpr uint64_t kWindowCeiling=256u*1024u*1024u;
constexpr uint64_t kGuardBytes=16u*1024u*1024u;
constexpr uint64_t kMinSurfaceBytes=32u*1024u*1024u;
// Linux waits 10 x usec_timeout (100k polls of udelay(1)); these are longer.
constexpr uint32_t kBootloaderWaitMs=2000u;
constexpr uint32_t kSosSettleMs=20u;              // mdelay(20) in load_sos
constexpr uint32_t kSosWaitMs=2000u;
// The one container this Kext accepts: psp_13_0_7_sos.bin from
// linux-firmware 20260915 (git 1522c78a, Ubuntu linux-firmware-amd-graphics).
constexpr size_t kPinnedBytes=248496u;
constexpr sha256::Digest kPinnedSha256={{
    0x94,0xdb,0x50,0x5f,0xa6,0x48,0x2f,0x25,
    0x8c,0x33,0xa0,0xa8,0xd4,0x12,0x05,0x0f,
    0x6d,0x84,0x3a,0xb4,0xad,0xa3,0x68,0x25,
    0x2e,0x98,0x8f,0x82,0xf8,0xa2,0x6f,0xa8}};
// The writer may touch exactly these two registers and fw_pri, nothing else.
constexpr uint32_t kWritableRegisters[2]={kC2pmsg36,kC2pmsg35};

struct Part { const uint8_t *data=nullptr; uint32_t size=0u, version=0u; };
struct Firmware { Part kdb, spl, sys, sos, toc; };
// Structure only; pinned() decides whether this exact file may be sent.
inline bool fromContainer(const uint8_t *image,size_t size,Firmware &out) {
    out=Firmware{};
    const psp13fw::Inventory inv=psp13fw::inspect(image,size);
    if(inv.code!=psp13fw::Code::Pass)return false;
    const psp13fw::Component *k=psp13fw::find(inv,psp13fw::kKdb);
    const psp13fw::Component *s=psp13fw::find(inv,psp13fw::kSpl);
    const psp13fw::Component *y=psp13fw::find(inv,psp13fw::kSysDrv);
    const psp13fw::Component *o=psp13fw::find(inv,psp13fw::kSos);
    // Any other bootloader part (SOC/INTF/DBG/RAS/...) would need a step
    // this chain does not have, so such a container is refused.
    for(uint32_t i=0;i<inv.descriptorCount;++i) {
        const uint32_t t=inv.component[i].type;
        if(t!=psp13fw::kKdb && t!=psp13fw::kSpl && t!=psp13fw::kSysDrv &&
           t!=psp13fw::kSos && t!=psp13fw::kToc) return false;
    }
    if(!k || !s || !y || !o)return false;
    const psp13fw::Component *parts[4]={k,s,y,o};
    for(const psp13fw::Component *c:parts)
        if(c->size>kFwPriBytes)return false;
    out.kdb={image+k->offset,k->size,k->version};
    out.spl={image+s->offset,s->size,s->version};
    out.sys={image+y->offset,y->size,y->version};
    out.sos={image+o->offset,o->size,o->version};
    // The TOC is optional here; only the ring stage (LOAD_TOC) uses it.
    const psp13fw::Component *t=psp13fw::find(inv,psp13fw::kToc);
    if(t && t->size<=kFwPriBytes)out.toc={image+t->offset,t->size,t->version};
    return true;
}
inline bool pinned(const uint8_t *image,size_t size) {
    return image && size==kPinnedBytes &&
           sha256::equal(sha256::hash(image,size),kPinnedSha256);
}

// BAR5 byte offsets, all resolved through checksum-valid IP Discovery.
struct Registers {
    bool valid=false;
    uint64_t c2p35=0u, c2p36=0u, c2p58=0u, c2p81=0u;
    uint64_t mmFbBase=0u, mmFbTop=0u, dcnFbBase=0u, dcnFbTop=0u;
    uint64_t surfLo[4]{}, surfHi[4]{};
};
inline Registers resolve(const IPDiscovery &disc,uint64_t bar5Bytes) {
    Registers r{};
    if(inventory(disc).code!=InventoryCode::Pass || !disc.isNavi33Candidate())
        return r;
    const uint32_t psp[4]={kC2pmsg35,kC2pmsg36,kC2pmsg58,kC2pmsg81};
    uint64_t *out[4]={&r.c2p35,&r.c2p36,&r.c2p58,&r.c2p81};
    for(unsigned i=0;i<4u;++i) {
        const RegisterAddress a=resolveDword(disc,Engine::PSP13,0u,psp[i],
                                             bar5Bytes);
        if(a.code!=AddressCode::Pass)return r;
        *out[i]=a.bar5ByteOffset;
    }
    // MMHUB 3.0.2 only (mmhub_3_0_2_offset.h FB_LOCATION_BASE/TOP).
    const vram_live::Plan fb=vram_live::fromDiscovery(disc,bar5Bytes);
    if(fb.code!=vram_live::PlanCode::Pass || fb.mmRevision!=2u)return r;
    r.mmFbBase=fb.offsets[0];r.mmFbTop=fb.offsets[1];
    const dcn321::ReadPlan dcn=dcn321::buildReadPlan(disc,bar5Bytes);
    if(dcn.code!=AddressCode::Pass)return r;
    using R=dcn321::Register;
    r.dcnFbBase=dcn.offsets[static_cast<size_t>(R::VmFbBase)];
    r.dcnFbTop=dcn.offsets[static_cast<size_t>(R::VmFbTop)];
    const R lo[4]={R::Pipe0SurfaceLo,R::Pipe1SurfaceLo,R::Pipe2SurfaceLo,
                   R::Pipe3SurfaceLo};
    const R hi[4]={R::Pipe0SurfaceHi,R::Pipe1SurfaceHi,R::Pipe2SurfaceHi,
                   R::Pipe3SurfaceHi};
    for(unsigned i=0;i<4u;++i) {
        r.surfLo[i]=dcn.offsets[static_cast<size_t>(lo[i])];
        r.surfHi[i]=dcn.offsets[static_cast<size_t>(hi[i])];
    }
    r.valid=true;
    return r;
}

using Read=bool (*)(void *ctx,uint64_t bar5Offset,uint32_t *out);
struct Snapshot {
    uint32_t c2p35=0u, c2p81=0u, mmFbBase=0u, mmFbTop=0u;
    uint32_t dcnFbBase=0u, dcnFbTop=0u, surfLo[4]{}, surfHi[4]{};
};
// Read-only. All-ones anywhere means a block is not answering: refuse.
inline bool readSnapshot(const Registers &r,Read read,void *ctx,Snapshot &s) {
    s=Snapshot{};
    if(!r.valid || !read)return false;
    struct Item { uint64_t off; uint32_t *dst; };
    const Item items[]={
        {r.c2p35,&s.c2p35},{r.c2p81,&s.c2p81},
        {r.mmFbBase,&s.mmFbBase},{r.mmFbTop,&s.mmFbTop},
        {r.dcnFbBase,&s.dcnFbBase},{r.dcnFbTop,&s.dcnFbTop},
        {r.surfLo[0],&s.surfLo[0]},{r.surfHi[0],&s.surfHi[0]},
        {r.surfLo[1],&s.surfLo[1]},{r.surfHi[1],&s.surfHi[1]},
        {r.surfLo[2],&s.surfLo[2]},{r.surfHi[2],&s.surfHi[2]},
        {r.surfLo[3],&s.surfLo[3]},{r.surfHi[3],&s.surfHi[3]}};
    for(const Item &it:items)
        if(!read(ctx,it.off,it.dst) || *it.dst==UINT32_MAX)return false;
    return true;
}

struct Range { uint64_t phys=0u, bytes=0u; };
// The GOP console as the boot loader handed it over (boot_args->Video).
// getConsoleInfo() is useless here: once a framebuffer owns the console it
// returns the kernel's mapping (0xffffffa06a800000 on 2026-10-18). Low bits
// can carry flags (RDNA4FB saw 0x840000001); x86-64 physical is 52 bits.
constexpr uint64_t kPhysLimit=1ull<<52u;
inline Range bootConsole(uint64_t base,uint32_t rowBytes,uint32_t height) {
    if(base>=kPhysLimit)return Range{};
    return Range{base&~0xfffull,static_cast<uint64_t>(rowBytes)*height};
}
// The PC firmware's console as the host saw it, for a guest with none. In
// the Tahoe VM (2026-10-18) the guest's AMD GOP switched DCN off, so the
// guest boots without it (rombar=0) and boot_args->Video is empty. The host
// records the firmware framebuffer from Linux's boot_params as a BAR0
// offset (vm/prepare-card.sh); the guest's BAR0 is the same aperture. Only
// used when the boot loader gave no console, and plan() still demands an
// active HUBP surface at exactly this offset.
inline Range hostConsole(const Range &boot,bool given,uint64_t offset,
                         uint64_t bytes,const Range &bar0) {
    if(boot.phys || !given || (offset&0xfffull) || !bytes || !bar0.phys ||
       offset>=bar0.bytes || bytes>bar0.bytes-offset)return Range{};
    return Range{bar0.phys+offset,bytes};
}
struct Plan {
    n33_psp_load_state state=N33_PSPL_BAD_REQUEST;
    uint32_t flags=0u, activePipes=0u;
    uint64_t fbBaseMc=0u, fbTopMc=0u, surfaceMc[4]{};
    uint64_t consoleOffset=0u, fwPriOffset=0u, fwPriMc=0u;
};
inline bool clearOf(uint64_t win,uint64_t start,uint64_t bytes,
                    uint64_t width=kFwPriBytes) {
    // The window keeps kGuardBytes away from [start, start+bytes) on both sides.
    const uint64_t lo=start>kGuardBytes ? start-kGuardBytes : 0u;
    const uint64_t hi=start+bytes+kGuardBytes;
    return win+width<=lo || win>=hi;
}
// Pure decision: where fw_pri goes, and whether a load may start at all.
// BAR0 offset == VRAM offset is proven, without a write, by the scanout
// surface the GOP programmed: surface MC - FB base == console BAR0 offset.
// The ring stage reuses every check below for a wider window (fw_pri plus the
// ring/command/fence pages) and needs the sOS alive instead of absent, so
// those two preconditions are parameters; plan() keeps the sOS-load meaning.
inline Plan planWindow(const Snapshot &s,const Range &console,const Range &bar0,
                       bool sosMustBeAlive,uint64_t windowBytes) {
    Plan p{};
    const uint64_t field=0x00ffffffu;
    p.fbBaseMc=(s.mmFbBase&field)<<24u;
    p.fbTopMc=((s.mmFbTop&field)<<24u)|0xffffffu;
    for(unsigned i=0;i<4u;++i)
        p.surfaceMc[i]=dcn321::combineSurfaceAddress(s.surfLo[i],s.surfHi[i]);
    if(!sosMustBeAlive) {
        if(s.c2p81) {p.state=N33_PSPL_SOS_ALREADY_ALIVE;return p;}
        if(s.c2p35!=kBootloaderReady) {p.state=N33_PSPL_BOOTLOADER_BUSY;return p;}
    }
    if(!(s.mmFbBase&field) || (s.mmFbTop&field)<(s.mmFbBase&field)) {
        p.state=N33_PSPL_BAD_FB_LOCATION;return p;
    }
    if(s.dcnFbBase&field) {
        if((s.dcnFbBase&field)!=(s.mmFbBase&field) ||
           (s.dcnFbTop&field)!=(s.mmFbTop&field)) {
            p.state=N33_PSPL_BAD_FB_LOCATION;return p;
        }
        p.flags|=N33_PSPL_FLAG_DCN_FB_MATCH;
    }
    if(!bar0.phys || bar0.bytes<kWindowCeiling) {
        p.state=N33_PSPL_NO_WINDOW;return p;
    }
    if(!console.phys || !console.bytes || console.phys<bar0.phys ||
       console.phys-bar0.phys>bar0.bytes ||
       console.bytes>bar0.bytes-(console.phys-bar0.phys)) {
        p.state=N33_PSPL_NO_CONSOLE;return p;
    }
    p.consoleOffset=console.phys-bar0.phys;
    p.flags|=N33_PSPL_FLAG_CONSOLE_FOUND;
    bool match=false;
    for(unsigned i=0;i<4u;++i) {
        const uint64_t mc=p.surfaceMc[i];
        if(!mc)continue;
        if(mc<p.fbBaseMc || mc>p.fbTopMc) {
            p.state=N33_PSPL_BAD_FB_LOCATION;return p;
        }
        p.activePipes|=1u<<i;
        if(mc-p.fbBaseMc==p.consoleOffset)match=true;
    }
    if(!match) {p.state=N33_PSPL_APERTURE_MISMATCH;return p;}
    p.flags|=N33_PSPL_FLAG_APERTURE_MATCH;
    // 16 MiB below the first 256 MiB of VRAM: far from the GOP console at
    // the bottom and from the firmware reservations at the top of VRAM.
    const uint64_t win=kWindowCeiling-kGuardBytes;
    if(!clearOf(win,p.consoleOffset,console.bytes,windowBytes)) {
        p.state=N33_PSPL_NO_WINDOW;return p;
    }
    for(unsigned i=0;i<4u;++i) {
        if(!(p.activePipes&(1u<<i)))continue;
        const uint64_t bytes=console.bytes>kMinSurfaceBytes ?
            console.bytes : kMinSurfaceBytes;
        if(!clearOf(win,p.surfaceMc[i]-p.fbBaseMc,bytes,windowBytes)) {
            p.state=N33_PSPL_NO_WINDOW;return p;
        }
    }
    if(p.fbBaseMc+win+windowBytes-1u>p.fbTopMc ||
       ((p.fbBaseMc+win)>>20u)>UINT32_MAX) {
        p.state=N33_PSPL_NO_WINDOW;return p;
    }
    p.fwPriOffset=win;
    p.fwPriMc=p.fbBaseMc+win;
    p.state=N33_PSPL_PLAN_OK;
    return p;
}
inline Plan plan(const Snapshot &s,const Range &console,const Range &bar0) {
    return planWindow(s,console,bar0,false,kFwPriBytes);
}

// Writer callbacks. stage() zeroes the whole fw_pri window then copies the
// part; verify() reads the window back. write32() must refuse any offset
// other than C2PMSG_35/36 (Navi33DarwinPspLoad.cpp does).
struct Hw {
    void *ctx=nullptr;
    Read read32=nullptr;
    bool (*write32)(void *ctx,uint64_t bar5Offset,uint32_t value)=nullptr;
    bool (*stage)(void *ctx,const uint8_t *src,uint32_t len)=nullptr;
    bool (*verify)(void *ctx,const uint8_t *src,uint32_t len)=nullptr;
    void (*sleepMs)(void *ctx,uint32_t ms)=nullptr;
};
struct Outcome {
    n33_psp_load_state state=N33_PSPL_BAD_REQUEST;
    n33_psp_load_step step=N33_PSPL_STEP_NONE;
    uint32_t stepsDone=0u, writes=0u, c2p35Last=0u, c2p81After=0u;
    uint32_t sosVersion=0u, elapsedMs=0u;
    bool wroteVram=false;
};
enum class Wait : uint8_t { Ready, Timeout, ReadFailed };
inline Wait waitBootloader(const Registers &r,const Hw &hw,Outcome &o) {
    for(uint32_t ms=0;;++ms) {
        uint32_t v=0u;
        if(!hw.read32(hw.ctx,r.c2p35,&v) || v==UINT32_MAX)
            return Wait::ReadFailed;
        o.c2p35Last=v;
        // Exact match, as psp_v13_0_wait_for_bootloader(): bits[7:0]
        // carry a command error and must stay clear.
        if(v==kBootloaderReady)return Wait::Ready;
        if(ms>=kBootloaderWaitMs)return Wait::Timeout;
        hw.sleepMs(hw.ctx,1u);++o.elapsedMs;
    }
}
inline n33_psp_load_state waitState(Wait w) {
    return w==Wait::Timeout ? N33_PSPL_BOOTLOADER_TIMEOUT : N33_PSPL_READ_FAILED;
}
inline Outcome run(const Plan &p,const Registers &r,const Firmware &fw,
                   const Hw &hw) {
    Outcome o{};
    if(p.state!=N33_PSPL_PLAN_OK || !r.valid || !p.fwPriMc ||
       (p.fwPriMc&(kFwPriBytes-1u)) || !hw.read32 || !hw.write32 ||
       !hw.stage || !hw.verify || !hw.sleepMs || !fw.kdb.data ||
       !fw.spl.data || !fw.sys.data || !fw.sos.data)
        return o;
    struct Step { n33_psp_load_step id; const Part *part; uint32_t cmd; };
    const Step steps[4]={
        {N33_PSPL_STEP_KDB,&fw.kdb,kLoadKdb},
        {N33_PSPL_STEP_SPL,&fw.kdb,kLoadSpl}, // upstream quirk, see top
        {N33_PSPL_STEP_SYS_DRV,&fw.sys,kLoadSysDrv},
        {N33_PSPL_STEP_SOS,&fw.sos,kLoadSos}};
    const uint32_t fwPri=static_cast<uint32_t>(p.fwPriMc>>20u);
    for(const Step &s:steps) {
        o.step=s.id;
        // psp_v13_0_is_sos_alive(): a live sOS closes the bootloader.
        uint32_t sol=0u;
        if(!hw.read32(hw.ctx,r.c2p81,&sol) || sol==UINT32_MAX) {
            o.state=N33_PSPL_READ_FAILED;return o;
        }
        if(sol) {o.c2p81After=sol;break;}
        Wait w=waitBootloader(r,hw,o);
        if(w!=Wait::Ready) {o.state=waitState(w);return o;}
        o.wroteVram=true;
        if(!hw.stage(hw.ctx,s.part->data,s.part->size) ||
           !hw.verify(hw.ctx,s.part->data,s.part->size)) {
            o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;
        }
        if(!hw.write32(hw.ctx,r.c2p36,fwPri)) {o.state=N33_PSPL_MAP_FAILED;return o;}
        ++o.writes;
        if(!hw.write32(hw.ctx,r.c2p35,s.cmd)) {o.state=N33_PSPL_MAP_FAILED;return o;}
        ++o.writes;
        if(s.id!=N33_PSPL_STEP_SOS) {
            w=waitBootloader(r,hw,o);
            if(w!=Wait::Ready) {o.state=waitState(w);return o;}
            ++o.stepsDone;
            continue;
        }
        hw.sleepMs(hw.ctx,kSosSettleMs);o.elapsedMs+=kSosSettleMs;
        for(uint32_t ms=0;;++ms) {
            if(!hw.read32(hw.ctx,r.c2p81,&sol) || sol==UINT32_MAX) {
                o.state=N33_PSPL_READ_FAILED;return o;
            }
            if(sol)break;
            if(ms>=kSosWaitMs) {
                (void)hw.read32(hw.ctx,r.c2p35,&o.c2p35Last);
                o.state=N33_PSPL_SOS_TIMEOUT;return o;
            }
            hw.sleepMs(hw.ctx,1u);++o.elapsedMs;
        }
        o.c2p81After=sol;++o.stepsDone;
    }
    if(!hw.read32(hw.ctx,r.c2p58,&o.sosVersion)) {
        o.state=N33_PSPL_READ_FAILED;return o;
    }
    o.state=N33_PSPL_SOS_ALIVE;
    return o;
}
inline n33_psp_load_result blank(n33_psp_load_state state) {
    n33_psp_load_result out{};
    out.abi_version=N33_ABI_VERSION;out.size=sizeof(out);out.state=state;
    return out;
}
inline void report(n33_psp_load_result &out,const Snapshot &s,const Plan &p) {
    out.flags|=p.flags;
    out.c2pmsg35_before=s.c2p35;out.c2pmsg81_before=s.c2p81;
    out.mm_fb_base=s.mmFbBase;out.mm_fb_top=s.mmFbTop;
    out.dcn_fb_base=s.dcnFbBase;out.active_pipes=p.activePipes;
    for(unsigned i=0;i<4u;++i)out.surface_mc[i]=p.surfaceMc[i];
    out.console_vram_offset=p.consoleOffset;
    out.fw_pri_vram_offset=p.fwPriOffset;out.fw_pri_mc=p.fwPriMc;
}
inline void report(n33_psp_load_result &out,const Outcome &o) {
    out.state=o.state;out.step=o.step;out.steps_done=o.stepsDone;
    out.register_writes=o.writes;out.c2pmsg35_last=o.c2p35Last;
    out.c2pmsg81_after=o.c2p81After;out.sos_version_after=o.sosVersion;
    out.elapsed_ms=o.elapsedMs;
    if(o.wroteVram)out.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    if(o.writes)out.flags|=N33_PSPL_FLAG_WROTE_REGISTERS;
}
} } // namespace navi33::psp13load
