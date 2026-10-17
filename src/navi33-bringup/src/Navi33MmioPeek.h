// SPDX-License-Identifier: MIT
// N33_SEL_MMIO_PEEK planning and reads. Host-testable, read-only: the only
// I/O is the caller's ReadDword, and only inside an allowlisted window
// resolved through a checksum-valid Navi33 IP Discovery table.
#pragma once
#include "Navi33HwProbeLogic.h"
#include "Navi33NativeABI.h"
#include "Navi33PeekWindows.h"
#include <stdint.h>

namespace navi33 { namespace peek {
constexpr bool kMayWriteMmio=false;
static_assert(!kMayWriteMmio,"MMIO peek must never write a register");
static_assert(N33_HWID_MP0==ipid::MP0 && N33_HWID_MP1==ipid::MP1 &&
              N33_HWID_GC==ipid::GC && N33_HWID_MMHUB==ipid::MMHUB &&
              N33_HWID_NBIF==ipid::NBIF,"peek window hardware IDs");

struct Plan {
    n33_peek_state state=N33_PEEK_BAD_REQUEST;
    uint64_t offset=0u; // BAR5 byte offset of the first dword
};
// The window, range and IP version are checked before the ROM is trusted
// for anything, so a request outside the allowlist never reaches BAR5.
inline Plan plan(const IPDiscovery &disc,uint32_t window,uint32_t first,
                 uint32_t count,uint64_t barBytes) {
    Plan out{};
    if(window>=N33_PEEK_WINDOW_COUNT || !count ||
       count>N33_PEEK_MAX_DWORDS) return out;
    const n33_peek_window &w=n33_peek_windows[window];
    if(first>=w.count || count>w.count-first) {
        out.state=N33_PEEK_NOT_ALLOWED;return out;
    }
    out.state=N33_PEEK_BAD_ROM;
    IPEntry e{};
    if(inventory(disc).code!=InventoryCode::Pass ||
       !disc.isNavi33Candidate() || !disc.find(w.hwid,0u,e) ||
       e.major!=w.major || e.minor!=w.minor || e.revision!=w.revision ||
       w.segment>=e.numBases) return out;
    const uint64_t base=e.bases[w.segment];
    uint64_t lo=0u,hi=0u;
    if(!hwprobe::resolve(base,w.first+first,barBytes,&lo) ||
       !hwprobe::resolve(base,w.first+first+count-1u,barBytes,&hi)) {
        out.state=N33_PEEK_NO_BAR5;return out;
    }
    out.state=N33_PEEK_DONE; // plan only, not a hardware read
    out.offset=lo;
    return out;
}
using ReadDword=bool (*)(void *ctx,uint64_t offset,uint32_t *out);
inline n33_peek_result blank(n33_peek_state state,uint32_t window=0u,
                             uint32_t first=0u,uint32_t count=0u) {
    n33_peek_result out{};
    out.abi_version=N33_ABI_VERSION;out.size=sizeof(out);
    out.state=state;out.window=window;out.first=first;out.count=count;
    return out;
}
// All-ones values are returned as read; the tool, not the Kext, decides
// what a dark block looks like.
inline n33_peek_result read(const Plan &p,uint32_t window,uint32_t first,
                            uint32_t count,ReadDword reader,void *ctx) {
    n33_peek_result out=blank(p.state,window,first,count);
    if(p.state!=N33_PEEK_DONE)return out;
    if(!reader || !p.offset || (p.offset&3u) || count>N33_PEEK_MAX_DWORDS) {
        out.state=N33_PEEK_READ_FAILED;return out;
    }
    out.bar5_offset=p.offset;
    for(uint32_t i=0u;i<count;++i) {
        if(!reader(ctx,p.offset+4ull*i,&out.values[i])) {
            out.state=N33_PEEK_READ_FAILED;return out;
        }
        out.reads=i+1u;
    }
    return out;
}
} } // namespace navi33::peek
