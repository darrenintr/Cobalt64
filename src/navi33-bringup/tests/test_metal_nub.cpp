// SPDX-License-Identifier: MIT
// G41: the Metal nub's pure logic (Navi33MetalPure.h): hard rule 6's publish gate, the IOAccelConfig
// fill and its static check, the stamp page, the task window, native_open's verdict and the ops
// header shared byte for byte with the aux accelerator kext.
#include "../src/Navi33MetalPure.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
namespace nm=navi33::metal;
static std::string slurp(const char *p) { std::ifstream f(p,std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>()); }
int main() {
    // Rule 6: nothing without the boot-arg; nothing while any live evidence is missing; once only.
    const nm::Evidence all{true,true,true,true,true,false};
    assert(nm::publish_verdict(all,nm::kOff)==nm::kOk);
    nm::Evidence e=all;e.bootarg=false;assert(nm::publish_verdict(e,nm::kOff)==nm::kUnsupported);
    for(int i=0;i<4;++i) {
        e=all;
        bool *const miss[4]={&e.started,&e.compute,&e.gfx,&e.flush};
        *miss[i]=false;
        assert(nm::publish_verdict(e,nm::kOff)==nm::kNotReady);
    }
    e=all;e.hung=true;assert(nm::publish_verdict(e,nm::kOff)==nm::kNotReady);
    assert(nm::publish_verdict(all,nm::kPublished)==nm::kExclusive && nm::publish_verdict(all,nm::kTerminating)==nm::kBusy);
    // IOAccelConfig: the family defaults kept where not stored, the limits checked, a refusal leaves cfg as it was.
    uint8_t cfg[nm::kCfgBytes];for(uint32_t i=0;i<nm::kCfgBytes;++i)cfg[i]=uint8_t(0x11+i);
    uint8_t before[nm::kCfgBytes];for(uint32_t i=0;i<nm::kCfgBytes;++i)before[i]=cfg[i];
    assert(nm::config_populate(cfg,nm::kCfgBytes,0x1234)==0 && nm::ld_le(cfg+nm::kOffName,8)==0x1234 && nm::ld_le(cfg+nm::kOffF0,4)==0x480000u);
    assert(cfg[0x4c]==before[0x4c] && cfg[nm::kOffPipeUC]==0);
    uint8_t zero[nm::kCfgBytes]={};uint8_t z2[nm::kCfgBytes]={};
    assert(nm::config_populate(zero,nm::kCfgBytes,0x1234)!=0);for(uint32_t i=0;i<nm::kCfgBytes;++i)assert(zero[i]==z2[i]);
    assert(nm::config_populate(cfg,nm::kCfgBytes-1,0x1234)!=0 && nm::config_populate(cfg,nm::kCfgBytes,0)!=0);
    // Stamp page, task window, native_open.
    assert(nm::stamp_page_ok(0x1000,0x1000,0x1000) && !nm::stamp_page_ok(0x1800,0x1000,0x1000) && !nm::stamp_page_ok(1ull<<52,0x1000,0x1000));
    uint64_t sz=0,res=0;assert(nm::task_window(0,&sz,&res)==0 && sz==nm::kTaskVaSize && res==0x1000 && nm::task_window(2,&sz,&res)!=0);
    assert(nm::native_open_verdict(true,true,true,true)==nm::kOk && nm::native_open_verdict(false,true,true,true)==nm::kUnsupported &&
           nm::native_open_verdict(true,false,true,true)==nm::kNotReady && nm::native_open_verdict(true,true,false,true)==nm::kBadArg &&
           nm::native_open_verdict(true,true,true,false)==nm::kNotReady);
    // The ops ABI is the aux kext's, byte for byte.
    const std::string mine=slurp("src/Navi48MetalOps.h"), aux=slurp("../../tools/native/navi48accel/src/Navi48MetalOps.h");
    assert(!mine.empty() && mine==aux);
    puts("Metal nub (G41): rule 6 publish gate, IOAccelConfig, stamp page, task window, native_open, shared ops ABI: passed");
}
