// Navi33 GMC11 FB geometry: offline data is synthetic, no GPU MMIO.
#include "../src/Navi33Gmc11VramGeometry.h"
#include <cassert>
#include <cstdio>
using namespace navi33::vram_geometry;
static PhysicalInventory physical() {
    PhysicalInventory r{};
    r.realBytes=8ull*1024ull*1024ull*1024ull;
    r.assignedBar0Bytes=256ull*1024ull*1024ull;
    r.independentlyVerifiedMemsize=true;
    return r;
}
int main() {
    const Hub mm{0x6000u,0x61ffu,0x100u};
    const Hub gfx=mm;
    const auto ok=decode(mm,gfx,physical());
    assert(ok.code==Code::Pass);
    assert(ok.geometry.mcStart==(0x6000ull<<24u));
    assert(ok.geometry.mcWindowBytes==8ull*1024ull*1024ull*1024ull);
    assert(ok.geometry.physicalFbBase==(0x100ull<<24u));
    assert(ok.geometry.realBytes==physical().realBytes);
    assert(ok.geometry.cpuVisibleBytes==physical().assignedBar0Bytes);
    const auto first=locate(ok.geometry,4096u,4096u);
    assert(first.valid && first.cpuBar0Visible &&
           first.gpuMcAddress==(0x6000ull<<24u)+4096u &&
           first.gpuPhysicalAddress==(0x100ull<<24u)+4096u &&
           first.bar0Offset==4096u);
    const auto tail=locate(ok.geometry,physical().realBytes-4096u,4096u);
    assert(tail.valid && !tail.cpuBar0Visible);
    assert(!locate(ok.geometry,physical().realBytes,4096u).valid);
    assert(!locate(ok.geometry,2048u,4096u).valid);
    assert(!locate(ok.geometry,4096u,4095u).valid);
    assert(locateMc(ok.geometry,first.gpuMcAddress,4096u).gpuPhysicalAddress==
           first.gpuPhysicalAddress);
    assert(!locateMc(ok.geometry,0u,4096u).valid);
    auto p=physical();
    p.independentlyVerifiedMemsize=false;
    assert(decode(mm,gfx,p).code==Code::BadInventory);
    p=physical();p.realBytes+=4096u;
    assert(decode(mm,gfx,p).code==Code::InconsistentCapacity);
    auto wrong=gfx;wrong.fbOffset++;
    assert(decode(mm,wrong,physical()).code==Code::DisagreeingHubs);
    wrong=gfx;wrong.fbTop=0xfffffff0u;
    assert(decode(mm,wrong,physical()).code==Code::InvalidHub);
    wrong=gfx;wrong.fbTop=wrong.fbBase-1u;
    assert(decode(mm,wrong,physical()).code==Code::InvalidHub);
    p=physical();p.assignedBar0Bytes=16ull*1024ull*1024ull*1024ull;
    assert(decode(mm,gfx,p).geometry.cpuVisibleBytes==p.realBytes);
    const Hub high{0xfffffeu,0xffffffu,0u};
    p.realBytes=16ull*1024ull*1024ull;
    assert(decode(high,high,p).code==Code::Pass); // 48-bit endpoint
    const Hub bad{0xffffffu,0xffffffu,0xffffffu};
    p.realBytes=32ull*1024ull*1024ull;
    assert(decode(bad,bad,p).code==Code::InconsistentCapacity);
    std::puts("GMC11 MC VRAM geometry and BAR0 aperture conversion OK (offline)");
}
