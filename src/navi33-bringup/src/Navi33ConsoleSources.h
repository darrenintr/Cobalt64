// SPDX-License-Identifier: MIT
// Read-only census of the console sources the PSP plan could use, for the
// bare-metal NO_CONSOLE investigation (BARE_METAL_ACCELERATION_PLAN.md step 1).
// On bare metal boot_args->Video names the firmware console at 0x7c00000000
// (the firmware's 8 GiB BAR0), while macOS runs BAR0 at 0x830000000, 256 MiB
// (AGENTS.md, 2026-10-19 08:47). This header only classifies each raw source
// against the live BAR0, and resolve() picks the console the PSP plan checks. No I/O.
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
// Which console the PSP plan should check. The 2026-10-20 21:08 bare-metal
// capture showed boot_args->Video at the firmware's BAR0 (0x7c00000000,
// outside), while the platform console read at start() was IOPCIFamily's
// relocated copy, 0x830000001, inside BAR0 with identical geometry; by plan
// time a framebuffer had replaced it with a kernel mapping. So: a boot console
// inside BAR0 wins as before; otherwise the start-time platform console is used
// only when it is inside BAR0 and its geometry equals the boot console's. Any
// other combination resolves to nothing (the plan then reports NO_CONSOLE).
// This picks the console the plan checks; plan()'s live HUBP surface check
// still has to prove the offset before any write.
enum class Pick : uint8_t { None=0, Boot, Relocated };
struct Resolved { Pick pick=Pick::None; uint64_t phys=0u, bytes=0u; };
inline bool sameGeometry(const Source &a,const Source &b) {
    return a.rowBytes==b.rowBytes && a.width==b.width && a.height==b.height && a.depth==b.depth;
}
inline Resolved resolve(const Source &boot,const Source &startPlatform,
                        uint64_t bar0Phys,uint64_t bar0Bytes) {
    const Classified b=classify(boot,bar0Phys,bar0Bytes);
    if(b.where==Where::InsideBar0)return Resolved{Pick::Boot,b.phys,b.bytes};
    const Classified p=classify(startPlatform,bar0Phys,bar0Bytes);
    if(b.where==Where::OutsideBar0 && p.where==Where::InsideBar0 && sameGeometry(boot,startPlatform))
        return Resolved{Pick::Relocated,p.phys,p.bytes};
    return Resolved{};
}
inline const char *name(Pick k) {
    return k==Pick::Boot ? "boot" : k==Pick::Relocated ? "relocated" : "none";
}
} } // namespace navi33::console
