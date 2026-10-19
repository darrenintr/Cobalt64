// SPDX-License-Identifier: MIT
// Boot-time bring-up (Phase B1): the eight write stages proven in the VM on
// 2026-10-19 (sOS 20:33, ring 20:34, SMU image, records, AUTOLOAD_RLC, SCPM
// 20:59, MES 21:06, MEC queue 21:32), in that order, each through its own
// executor with its own plan check and latch. Pure sequencing: the stages run
// through a callback. It stops at the first stage that misses its success
// state. A refusal that wrote nothing is retried only where tonight's runs
// showed a transient: the PSP ring not ready right after the sOS, and the IMU
// core status flicker (0x11) before SCPM.
#pragma once
#include "Navi33NativeABI.h"
#include <stddef.h>
#include <stdint.h>
namespace navi33 { namespace autostart {
enum class Payload : uint8_t { None, Sos, IpFw };
struct Step {
    uint32_t mode, startAt, stopAfter; Payload payload; bool autoload;
    uint32_t success, retryState; const char *name;
};
constexpr Step kSteps[]={
    {N33_PSP_LOAD_RUN,0,0,Payload::Sos,false,N33_PSPL_SOS_ALIVE,0,"sOS"},
    {N33_PSP_LOAD_RING_RUN,0,N33_PSP_RING_STOP_TMR,Payload::Sos,false,N33_PSPL_RING_DONE,
     N33_PSPL_RING_NOT_READY,"ring"},
    {N33_PSP_LOAD_FW_RUN,26,27,Payload::IpFw,false,N33_PSPL_FW_DONE,0,"smu-fw"},
    {N33_PSP_LOAD_FW_RUN,0,26,Payload::IpFw,false,N33_PSPL_FW_DONE,0,"ip-fw"},
    {N33_PSP_LOAD_FW_RUN,27,28,Payload::IpFw,true,N33_PSPL_FW_DONE,0,"autoload"},
    {N33_SMU_SCPM_RUN,0,0,Payload::None,false,N33_PSPL_SMU_GC_READY,
     N33_PSPL_SMU_BAD_PRECONDITION,"scpm"},
    {N33_MES_START_RUN,0,0,Payload::None,false,N33_PSPL_MES_RUNNING,0,"mes"},
    {N33_MEC_QUEUE_RUN,0,0,Payload::None,false,N33_PSPL_MEC_QUEUE_OK,0,"mec"},
};
constexpr unsigned kStepCount=sizeof(kSteps)/sizeof(kSteps[0]);
constexpr unsigned kRetries=20u; constexpr uint32_t kRetryMs=50u;

// navi33-autostart=1 opens it; navi33-autostart-off and every kill switch of a
// stage it runs close it, as do a closed or killed hardware probe.
struct Gates { bool on=false, autostartOff=false, pspOff=false, smuOff=false,
               mesOff=false, mecOff=false, hwProbe=false, hwOff=false; };
inline bool allowed(const Gates &g) {
    return g.on && !g.autostartOff && !g.pspOff && !g.smuOff && !g.mesOff && !g.mecOff &&
           g.hwProbe && !g.hwOff;
}
struct Report {
    uint32_t reached=0;               // steps that met their success state
    uint32_t state[kStepCount]{}, writes[kStepCount]{}, tries[kStepCount]{};
    bool done=false;
};
using RunStep=uint32_t (*)(void *ctx,const Step &step,uint32_t *writes);
using Sleep=void (*)(void *ctx,uint32_t ms);
inline Report run(void *ctx,RunStep runStep,Sleep sleep) {
    Report r{};
    if(!runStep || !sleep)return r;
    for(unsigned i=0;i<kStepCount;++i) {
        const Step &s=kSteps[i];
        for(;;) {
            uint32_t writes=0u;
            r.state[i]=runStep(ctx,s,&writes);
            r.writes[i]+=writes;++r.tries[i];
            if(r.state[i]==s.success)break;
            // Retry only a zero-write refusal of the stage's known transient.
            if(!s.retryState || r.state[i]!=s.retryState || writes || r.tries[i]>kRetries)return r;
            sleep(ctx,kRetryMs);
        }
        ++r.reached;
    }
    r.done=true;return r;
}
} }
