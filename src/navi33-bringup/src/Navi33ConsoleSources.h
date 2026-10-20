// SPDX-License-Identifier: MIT
// Read-only census of the console sources the PSP plan could use, for the
// bare-metal NO_CONSOLE investigation (BARE_METAL_ACCELERATION_PLAN.md step 1).
// On bare metal boot_args->Video names the firmware console at 0x7c00000000
// (the firmware's 8 GiB BAR0), while macOS runs BAR0 at 0x830000000, 256 MiB
// (AGENTS.md, 2026-10-19 08:47). This header only classifies each raw source
// against the live BAR0; it does not pick a console or feed the plan. No I/O.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace navi33 { namespace console {
// x86-64 physical addresses are below 2^52; anything above is a kernel
// mapping (getConsoleInfo returned 0xffffffa06a800000 on 2026-10-18).
constexpr uint64_t kPhysLimit=1ull<<52u;
enum class Where : uint8_t { Empty=0, Virtual, BadGeometry, OutsideBar0, InsideBar0 };
struct Source { uint64_t base=0u; uint32_t rowBytes=0u,width=0u,height=0u,depth=0u,display=0u; };
struct Classified {
    Where where=Where::Empty;
    uint64_t phys=0u, bytes=0u, bar0Offset=0u;
};
inline const char *name(Where w) {
    switch(w) {
    case Where::Empty: return "empty";
    case Where::Virtual: return "virtual";
    case Where::BadGeometry: return "bad-geometry";
    case Where::OutsideBar0: return "outside-bar0";
    case Where::InsideBar0: return "inside-bar0";
    }
    return "?";
}
// Low bits can carry flags (IOPCIFamily stores base | 1 after relocating the
// console; RDNA4FB saw 0x840000001), so the page base is what counts.
inline Classified classify(const Source &s,uint64_t bar0Phys,uint64_t bar0Bytes) {
    Classified c{};
    if(!s.base)return c;
    if(s.base>=kPhysLimit) {c.where=Where::Virtual;return c;}
    c.phys=s.base&~0xfffull;
    c.bytes=static_cast<uint64_t>(s.rowBytes)*s.height;
    const uint64_t minRow=static_cast<uint64_t>(s.width)*((s.depth+7u)/8u);
    if(!c.bytes || !s.width || !s.depth || s.rowBytes<minRow) {c.where=Where::BadGeometry;return c;}
    if(bar0Phys && bar0Bytes && c.phys>=bar0Phys && c.phys-bar0Phys<bar0Bytes &&
       c.bytes<=bar0Bytes-(c.phys-bar0Phys)) {
        c.where=Where::InsideBar0;c.bar0Offset=c.phys-bar0Phys;return c;
    }
    c.where=Where::OutsideBar0;return c;
}
} } // namespace navi33::console
