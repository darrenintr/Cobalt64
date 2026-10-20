// Navi33 console-source census: pure classification only, no hardware.
// Inputs mirror the bare-metal facts in AGENTS.md (2026-10-19 08:47).
#include "../src/Navi33ConsoleSources.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
namespace c=navi33::console;
int main() {
    const uint64_t bar0=0x830000000ull,bar0Bytes=256ull<<20;
    const c::Source fhd{0u,7680u,1920u,1080u,32u,1u};
    auto at=[&](uint64_t base){c::Source s=fhd;s.base=base;return c::classify(s,bar0,bar0Bytes);};
    assert(at(0u).where==c::Where::Empty);
    // A framebuffer's kernel mapping is not a physical console.
    assert(at(0xffffffa06a800000ull).where==c::Where::Virtual);
    // boot_args->Video on bare metal: the firmware's BAR0, not macOS's.
    assert(at(0x7c00000000ull).where==c::Where::OutsideBar0);
    // IOPCIFamily-style relocated base with its flag bit: offset 0.
    {const c::Classified k=at(0x830000001ull);
     assert(k.where==c::Where::InsideBar0 && k.phys==bar0 && k.bar0Offset==0u && k.bytes==7680ull*1080u);}
    // A 16 MiB offset is reported as measured, not rounded to 0.
    assert(at(bar0+(16ull<<20)).bar0Offset==16ull<<20);
    // A console that runs past the end of BAR0 is not inside it.
    assert(at(bar0+bar0Bytes-4096u).where==c::Where::OutsideBar0);
    assert(at(bar0+bar0Bytes).where==c::Where::OutsideBar0);
    // Geometry that cannot describe the scanout is refused before any range check.
    {c::Source s=fhd;s.base=bar0;s.height=0u;assert(c::classify(s,bar0,bar0Bytes).where==c::Where::BadGeometry);}
    {c::Source s=fhd;s.base=bar0;s.rowBytes=1919u*4u;assert(c::classify(s,bar0,bar0Bytes).where==c::Where::BadGeometry);}
    {c::Source s=fhd;s.base=bar0;s.depth=0u;assert(c::classify(s,bar0,bar0Bytes).where==c::Where::BadGeometry);}
    // Huge geometry near the physical limit must not wrap into BAR0.
    {c::Source s{c::kPhysLimit-0x1000u,0xffffffffu,1u,0xffffffffu,8u,0u};
     assert(c::classify(s,c::kPhysLimit-0x2000u,0x2000u).where==c::Where::OutsideBar0);}
    // No BAR0 known: never inside.
    assert(c::classify(c::Source{bar0,7680u,1920u,1080u,32u,1u},0u,0u).where==c::Where::OutsideBar0);
    for(auto w:{c::Where::Empty,c::Where::Virtual,c::Where::BadGeometry,c::Where::OutsideBar0,c::Where::InsideBar0})
        assert(c::name(w)[0]!='?');
    std::puts("Navi33 console sources: boot/platform consoles classified against BAR0 (pure, no hardware)");
}
