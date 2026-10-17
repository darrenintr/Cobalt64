// GPU hardware is NEVER accessed here. Tests only software VRAM ledger.
#include "../src/Navi33VramBoLedger.h"
#include <cassert>
#include <cstdio>
using namespace navi33::vram_bo;
static n33_vram_geometry sampleGeometry() {
    n33_vram_geometry g{};
    g.abi_version=N33_ABI_VERSION;
    g.size=sizeof(g);
    g.state=N33_VRAM_OBSERVED;
    g.flags=N33_VRAM_FLAG_MMIO_STABLE|N33_VRAM_FLAG_BAR0_PRESENT;
    g.fb_base_bytes=0x100000000ull;
    g.fb_window_bytes=8ull<<30u;
    g.fb_top_inclusive_bytes=g.fb_base_bytes+g.fb_window_bytes-1u;
    g.mc_fb_offset_bytes=0x300000000ull;
    g.assigned_bar0_bytes=256ull<<20u;
    return g;
}
int main() {
    {
        Ledger empty{};
        assert(empty.allocate(Kind::GartRoot,kPage).code==Code::NotConfigured);
        n33_vram_geometry g=sampleGeometry();
        g.state=N33_VRAM_DISABLED;
        assert(empty.configure(g,{0x20000u,0x10000u},nullptr,0u)==
               Code::InvalidWindow);
    }
    {
        Ledger l{};
        const auto g=sampleGeometry();
        assert(l.configure(g,{0u,0u},nullptr,0u)==Code::MissingOwnedPool);
        assert(l.configure(g,{g.fb_window_bytes,4096u},nullptr,0u)==
               Code::InvalidPool);
        const Span excluded[]={{0x10000u,0x2000u},{0x15000u,0x1000u}};
        assert(l.configure(g,{0x10000u,0x10000u},excluded,2u)==
               Code::Pass);
        assert(l.configure(g,{0x10000u,0x10000u},nullptr,0u)==
               Code::Active);
        auto bad=l.allocate(Kind::GartRoot,0u);
        assert(bad.code==Code::BadRequest);
        bad=l.allocate(Kind::GartRoot,4096u,1023u);
        assert(bad.code==Code::BadRequest);
        auto a=l.allocate(Kind::GartRoot,kPage);
        assert(a.code==Code::Pass && a.span.start==0x12000u &&
               a.span.bytes==kPage);
        auto b=l.allocate(Kind::Ring,0x2000u);
        assert(b.code==Code::Pass && b.span.start==0x13000u);
        auto c=l.allocate(Kind::Scratch,4096u);
        assert(c.code==Code::Pass && c.span.start==0x16000u);
        assert(l.activeCount()==3u);
        Addresses addr{};
        assert(l.address(a.ticket,&addr)==Code::Pass);
        assert(addr.locationMc==g.fb_base_bytes+a.span.start);
        assert(addr.pteMcOffset==g.mc_fb_offset_bytes+a.span.start);
        assert(addr.candidateBar0ByteOffset==a.span.start);
        assert(addr.cpuVisibleCandidate);
        assert(!l.pin({a.ticket.slot,a.ticket.generation+1u}));
        assert(l.pin(a.ticket));
        assert(!l.release(a.ticket));
        assert(!l.pin(a.ticket));
        assert(l.unpin(a.ticket));
        assert(l.release(a.ticket));
        assert(!l.release(a.ticket));
        auto replacement=l.allocate(Kind::GartRoot,kPage);
        assert(replacement.code==Code::Pass);
        assert(replacement.span.start==a.span.start);
        assert(replacement.ticket.generation>a.ticket.generation);
        assert(!l.release(a.ticket)); // ABA protection
        assert(l.pin(replacement.ticket));
        assert(l.markGpuMayAccess(replacement.ticket));
        assert(!l.unpin(replacement.ticket));
        assert(!l.release(replacement.ticket));
        assert(l.memoryMustRemainPinned());
        assert(!l.markGpuMayAccess(b.ticket)); // unpinned
        assert(l.pin(b.ticket));
        assert(!l.release(b.ticket));
        assert(l.unpin(b.ticket));
        assert(l.release(b.ticket));
        // The poisoned root remains pinned despite other leases ending.
        assert(l.activeCount()==2u);
    }
    {
        Ledger l{};
        const auto g=sampleGeometry();
        const Span blocked[]={{0u,4096u}};
        assert(l.configure(g,{0u,4096u},blocked,1u)==Code::Pass);
        assert(l.allocate(Kind::GartRoot,4096u).code==Code::NoSpace);
    }
    {
        auto g=sampleGeometry();
        g.fb_window_bytes=0ull;
        Ledger l{};
        assert(l.configure(g,{4096u,4096u},nullptr,0u)==Code::InvalidWindow);
    }
    {
        auto g=sampleGeometry();
        g.assigned_bar0_bytes=4096u;
        Ledger l{};
        assert(l.configure(g,{0x2000u,0x2000u},nullptr,0u)==Code::Pass);
        const auto a=l.allocate(Kind::GartRoot,kPage);
        Addresses address{};
        assert(l.address(a.ticket,&address)==Code::Pass);
        assert(!address.cpuVisibleCandidate);
        assert(address.locationMc!=address.pteMcOffset);
    }
    std::puts("Navi33 VRAM ledger: external pool, protected spans, first-fit, MC spaces, ABA and poison validated on CPU");
}
