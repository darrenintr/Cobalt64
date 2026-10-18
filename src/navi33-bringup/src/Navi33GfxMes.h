// SPDX-License-Identifier: MIT
// Offline GFX11 MES (micro engine scheduler) bring-up plan for gc_11_0_0.
// Mirrors mes_v11_0_enable(), mes_v11_0_queue_init_register() and the
// MES MAP_QUEUES call in mes_v11_0_kiq_enable_queue(), for Navi33 where
// enable_mes and enable_mes_kiq are both set (amdgpu_discovery.c). Upstream
// sources are torvalds/linux master, fetched 2026-10-18. The planner returns
// ordered register writes and PM4 dwords. It performs no I/O and is not
// hardware evidence.
#pragma once
#include "Navi33Gfx11KiqHqd.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace gfx11_mes {

// gc_11_0_0_offset.h, all GC segment 1.
constexpr uint32_t kCpMesCntl=0x2807u;
constexpr uint32_t kCpMesPrgrmCntrStart=0x2800u;
constexpr uint32_t kCpMesPrgrmCntrStartHi=0x289du;
constexpr uint32_t kGc11SegMes=1u;

// gc_11_0_0_sh_mask.h, CP_MES_CNTL.
constexpr uint32_t kMesInvalidateIcache=0x00000010u;   // bit 4
constexpr uint32_t kMesPipe0Reset=0x00010000u;         // bit 16
constexpr uint32_t kMesPipe1Reset=0x00020000u;         // bit 17
constexpr uint32_t kMesPipe0Active=0x04000000u;        // bit 26
constexpr uint32_t kMesPipe1Active=0x08000000u;        // bit 27
constexpr uint32_t kMesHalt=0x40000000u;               // bit 30

// The MES instance is GRBM me 3 (mes_v11_0.c soc21_grbm_select(adev, 3, ...)).
constexpr uint8_t kMesGrbmMe=3u;
constexpr uint8_t kNoGrbmPipe=0xffu;

// Pipe 0 schedules; pipe 1 is the MES KIQ (AMDGPU_MES_SCHED_PIPE / KIQ_PIPE).
constexpr uint8_t kSchedPipe=0u;
constexpr uint8_t kKiqPipe=1u;

// mes_v11_0.c and sh_mask: CP_HQD_PQ_CONTROL.NO_UPDATE_RPTR is bit 27.
constexpr uint32_t kPqNoUpdateRptrMask=0x08000000u;
constexpr uint32_t kPersistentPreload=0x55u;

// mes_v11_0.c: the MES wptr-poll address is 8-byte aligned, not 4-byte.
constexpr uint64_t kWptrPollMask=0xfffffff8ull;

// amdgpu_mes.h AMDGPU_MAX_MES_PIPES.
constexpr uint32_t kMaxMesPipes=2u;

// nvd.h / soc15d.h MAP_QUEUES encodings.
constexpr uint32_t kPacketMapQueues=0xa2u;
constexpr uint32_t kPacketCountMapQueues=5u;   // 6 body dwords
constexpr uint32_t kMapQueueMe=2u;             // AMDGPU_RING_TYPE_MES
constexpr uint32_t kMapQueueEngineSel=5u;      // MES engine

// soc15d.h PACKET3(op, n): type 3, n = body dwords - 1.
constexpr uint32_t packet3(uint32_t op,uint32_t count) {
    return (3u<<30u)|((count&0x3fffu)<<16u)|((op&0xffu)<<8u);
}

enum class Code : uint8_t { Refused=0, PlanOnly };
enum Refusal : uint32_t {
    NoFirmware    = 1u<<0,   // uc_start_addr is zero
    BadStartAddr  = 1u<<1,   // not 4-byte aligned
    QueueRefused  = 1u<<2,   // the KIQ queue inputs failed their own checks
};

struct Write {
    uint8_t grbmMe;
    uint8_t grbmPipe;        // kNoGrbmPipe when the write is not under a select
    uint32_t seg;
    uint32_t dword;
    uint32_t value;
    const char *name;
};

constexpr size_t kMaxWrites=24;
struct Plan {
    Code code=Code::Refused;
    uint32_t refusalMask=0u;
    uint8_t count=0u;
    Write writes[kMaxWrites]{};
};

inline void push(Plan &p,const Write &w) {
    if(p.count<kMaxWrites) p.writes[p.count++]=w;
}

// Enable: mes_v11_0_enable(adev, true) with enable_mes_kiq set.
struct EnableInput {
    uint32_t cntlCurrent=0u;        // CP_MES_CNTL read before the first step
    uint64_t ucStart[kMaxMesPipes]{}; // mes.uc_start_addr[] from the firmware
    bool kiq=true;                  // enable_mes_kiq
};

// Disable: mes_v11_0_enable(adev, false). Read-modify-write from current.
inline Plan buildEnable(const EnableInput &in) {
    Plan p{};
    if(in.ucStart[kSchedPipe]==0u || (in.kiq && in.ucStart[kKiqPipe]==0u)) {
        p.refusalMask=NoFirmware;
        return p;
    }
    for(uint8_t pipe=0;pipe<kMaxMesPipes;++pipe) {
        if(!in.kiq && pipe==kKiqPipe) continue;
        if(in.ucStart[pipe]&3u) {
            p.refusalMask=BadStartAddr;
            return p;
        }
    }

    // Step 1: reset both pipes (PIPE1_RESET follows enable_mes_kiq).
    uint32_t cntl=in.cntlCurrent;
    cntl=gfx11_kiq::setField(cntl,kMesPipe0Reset,16u,1u);
    cntl=gfx11_kiq::setField(cntl,kMesPipe1Reset,17u,in.kiq ? 1u : 0u);
    push(p,{kMesGrbmMe,kNoGrbmPipe,kGc11SegMes,kCpMesCntl,cntl,
            "CP_MES_CNTL reset pipes"});

    // Step 2: program the start address per pipe, in ucode-dword units.
    for(uint8_t pipe=0;pipe<kMaxMesPipes;++pipe) {
        if(!in.kiq && pipe==kKiqPipe) continue;
        const uint64_t addr=in.ucStart[pipe]>>2u;
        push(p,{kMesGrbmMe,pipe,kGc11SegMes,kCpMesPrgrmCntrStart,
                static_cast<uint32_t>(addr),"CP_MES_PRGRM_CNTR_START"});
        push(p,{kMesGrbmMe,pipe,kGc11SegMes,kCpMesPrgrmCntrStartHi,
                static_cast<uint32_t>(addr>>32u),"CP_MES_PRGRM_CNTR_START_HI"});
    }

    // Step 3: unhalt and activate. Upstream builds this word from zero, so
    // every other bit, including HALT and the resets, is cleared.
    uint32_t active=0u;
    active=gfx11_kiq::setField(active,kMesPipe0Active,26u,1u);
    active=gfx11_kiq::setField(active,kMesPipe1Active,27u,in.kiq ? 1u : 0u);
    push(p,{kMesGrbmMe,kNoGrbmPipe,kGc11SegMes,kCpMesCntl,active,
            "CP_MES_CNTL unhalt and activate"});

    p.code=Code::PlanOnly;
    return p;
}

inline Plan buildDisable(uint32_t cntlCurrent,bool kiq) {
    Plan p{};
    uint32_t cntl=cntlCurrent;
    cntl=gfx11_kiq::setField(cntl,kMesPipe0Active,26u,0u);
    cntl=gfx11_kiq::setField(cntl,kMesPipe1Active,27u,0u);
    cntl=gfx11_kiq::setField(cntl,kMesInvalidateIcache,4u,1u);
    cntl=gfx11_kiq::setField(cntl,kMesPipe0Reset,16u,1u);
    cntl=gfx11_kiq::setField(cntl,kMesPipe1Reset,17u,kiq ? 1u : 0u);
    cntl=gfx11_kiq::setField(cntl,kMesHalt,30u,1u);
    push(p,{kMesGrbmMe,kNoGrbmPipe,kGc11SegMes,kCpMesCntl,cntl,
            "CP_MES_CNTL halt"});
    p.code=Code::PlanOnly;
    return p;
}

// The MES KIQ-pipe queue, as mes_v11_0_queue_init_register() writes it.
// The MQD fields and the MES-specific bits (NO_UPDATE_RPTR, wptr poll mask,
// the RPTR_BLOCK_SIZE expression) are the ones mes_v11_0_mqd_init() uses.
struct QueueInput {
    gfx11_kiq::KiqQueue q{};        // addresses, sizes, doorbell
    uint32_t vmidCurrent=0u;        // CP_HQD_VMID read before the RMW
    uint32_t doorbellCurrent=0u;    // CP_HQD_PQ_DOORBELL_CONTROL before the RMW
};

inline Plan buildKiqQueueRegs(const QueueInput &in) {
    using namespace gfx11_kiq;
    Plan p{};
    const KiqQueue &q=in.q;
    uint32_t refused=0u;
    if(!isPow2(q.queueBytes) || q.queueBytes<kGpuPageBytes ||
       q.queueBytes>kMaxQueueBytes)
        refused|=BadQueueSize;
    if(!aligned(q.mqdGpuVa,4u) || !aligned(q.pqGpuVa,256u) ||
       !aligned(q.rptrGpuVa,4u) || !aligned(q.wptrGpuVa,8u))
        refused|=BadAlignment;
    if(q.mqdGpuVa>kMaxGpuAddress || q.pqGpuVa>kMaxGpuAddress ||
       q.rptrGpuVa>kMaxGpuAddress || q.wptrGpuVa>kMaxGpuAddress)
        refused|=AddressTooWide;
    if(q.useDoorbell && q.doorbellIndex>kMaxDoorbellIndex)
        refused|=BadDoorbell;
    if(q.hqdActiveAtStart)
        refused|=DequeueRequired;
    if(refused) {
        p.refusalMask=QueueRefused;
        return p;
    }

    const uint8_t me=kMesGrbmMe, pipe=kKiqPipe;
    auto reg=[&](uint32_t dword,uint32_t value,const char *name) {
        push(p,{me,pipe,0u,dword,value,name});
    };

    // CP_HQD_VMID.VMID = 0 (read-modify-write).
    reg(kCpHqdVmid,setField(in.vmidCurrent,0x0000000fu,0u,0u),
        "CP_HQD_VMID.VMID=0");
    // CP_HQD_PQ_DOORBELL_CONTROL.DOORBELL_EN = 0 (read-modify-write).
    reg(kCpHqdPqDoorbellControl,
        setField(in.doorbellCurrent,kDoorbellEnMask,30u,0u),
        "CP_HQD_PQ_DOORBELL_CONTROL.EN=0");
    reg(kCpMqdBaseAddr,static_cast<uint32_t>(q.mqdGpuVa&0xfffffffcu),
        "CP_MQD_BASE_ADDR");
    reg(kCpMqdBaseAddrHi,static_cast<uint32_t>(q.mqdGpuVa>>32u),
        "CP_MQD_BASE_ADDR_HI");
    // Upstream writes the literal 0 here, not the mqd field.
    reg(kCpMqdControl,0u,"CP_MQD_CONTROL=0");
    reg(kCpHqdPqBase,static_cast<uint32_t>(q.pqGpuVa>>8u),"CP_HQD_PQ_BASE");
    reg(kCpHqdPqBaseHi,static_cast<uint32_t>(q.pqGpuVa>>40u),
        "CP_HQD_PQ_BASE_HI");
    reg(kCpHqdPqRptrReportAddr,static_cast<uint32_t>(q.rptrGpuVa&0xfffffffcu),
        "CP_HQD_PQ_RPTR_REPORT_ADDR");
    reg(kCpHqdPqRptrReportAddrHi,
        static_cast<uint32_t>((q.rptrGpuVa>>32u)&0xffffu),
        "CP_HQD_PQ_RPTR_REPORT_ADDR_HI");

    // mes_v11_0_mqd_init() pq control. The RPTR_BLOCK_SIZE value is passed
    // already shifted by 8, and setField shifts it again, so the field reads
    // 0 on hardware. This mirrors upstream on purpose.
    uint32_t pq=kPqControlDefault;
    pq=setField(pq,kPqQueueSizeMask,0u,orderBase2(q.queueBytes/4u)-1u);
    pq=setField(pq,kPqRptrBlockSizeMask,kPqRptrBlockSizeShift,
                (orderBase2(kGpuPageBytes/4u)-1u)<<8u);
    pq=setField(pq,kPqUnordDispatchMask,28u,1u);
    pq=setField(pq,kPqTunnelDispatchMask,29u,0u);
    pq=setField(pq,kPqPrivStateMask,30u,1u);
    pq=setField(pq,kPqKmdQueueMask,31u,1u);
    pq=setField(pq,kPqNoUpdateRptrMask,27u,1u);
    reg(kCpHqdPqControl,pq,"CP_HQD_PQ_CONTROL");

    reg(kCpHqdPqWptrPollAddr,
        static_cast<uint32_t>(q.wptrGpuVa&kWptrPollMask),
        "CP_HQD_PQ_WPTR_POLL_ADDR");
    reg(kCpHqdPqWptrPollAddrHi,
        static_cast<uint32_t>((q.wptrGpuVa>>32u)&0xffffu),
        "CP_HQD_PQ_WPTR_POLL_ADDR_HI");

    uint32_t doorbell=kPqDoorbellControlDefault;
    if(q.useDoorbell) {
        doorbell=setField(doorbell,kDoorbellOffsetMask,kDoorbellOffsetShift,
                          q.doorbellIndex);
        doorbell=setField(doorbell,kDoorbellEnMask,30u,1u);
    } else {
        doorbell=setField(doorbell,kDoorbellEnMask,30u,0u);
    }
    reg(kCpHqdPqDoorbellControl,doorbell,"CP_HQD_PQ_DOORBELL_CONTROL");

    const uint32_t persistent=setField(kPersistentStateDefault,
        kPersistentPreloadMask,kPersistentPreloadShift,kPersistentPreload);
    reg(kCpHqdPersistentState,persistent,"CP_HQD_PERSISTENT_STATE");
    reg(kCpHqdActive,1u,"CP_HQD_ACTIVE=1");

    p.code=Code::PlanOnly;
    return p;
}

// MAP_QUEUES for the MES scheduler ring, sent on the KIQ path after the MES
// queue is up (kiq_map_queues for AMDGPU_RING_TYPE_MES: me 2, engine 5).
struct MapInput {
    uint64_t mqdGpuVa=0u;
    uint64_t wptrGpuVa=0u;
    uint32_t doorbellIndex=0u;      // ring->doorbell_index (already << 1)
    uint32_t queue=0u;
    uint32_t pipe=kSchedPipe;
};

struct MapPacket {
    uint32_t dw[7];
    bool ok=false;
};

inline MapPacket buildMapMesRing(const MapInput &in) {
    MapPacket out{};
    if(in.mqdGpuVa>gfx11_kiq::kMaxGpuAddress ||
       in.wptrGpuVa>gfx11_kiq::kMaxGpuAddress ||
       in.doorbellIndex>gfx11_kiq::kMaxDoorbellIndex ||
       in.queue>7u || in.pipe>=kMaxMesPipes ||
       !gfx11_kiq::aligned(in.mqdGpuVa,4u) ||
       !gfx11_kiq::aligned(in.wptrGpuVa,4u))
        return out;
    out.dw[0]=packet3(kPacketMapQueues,kPacketCountMapQueues);
    // QUEUE_SEL 0, VMID 0, QUEUE, PIPE, ME 2, QUEUE_TYPE 0, ALLOC 0,
    // ENGINE_SEL 5, NUM_QUEUES 1.
    out.dw[1]=(in.queue<<13u)|(in.pipe<<16u)|(kMapQueueMe<<18u)|
              (kMapQueueEngineSel<<26u)|(1u<<29u);
    out.dw[2]=in.doorbellIndex<<2u;
    out.dw[3]=static_cast<uint32_t>(in.mqdGpuVa);
    out.dw[4]=static_cast<uint32_t>(in.mqdGpuVa>>32u);
    out.dw[5]=static_cast<uint32_t>(in.wptrGpuVa);
    out.dw[6]=static_cast<uint32_t>(in.wptrGpuVa>>32u);
    out.ok=true;
    return out;
}

} } // namespace navi33::gfx11_mes
