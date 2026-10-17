// Host-only; no access to GPU or PCI. Synthetic status reads.
#include "../src/Navi33Psp13Status.h"
#include <cassert>
#include <cstdio>
using namespace navi33;
using namespace navi33::psp13;

struct Mock {
    uint32_t life=1u, version=0x01020304u, boot=0x80000000u;
    unsigned reads=0u;
    unsigned failAt=0u;
    bool allOnes=false, changing=false;
};
static bool mockRead(void *ctx, uint64_t offset, uint32_t *out) {
    auto *m=static_cast<Mock *>(ctx);
    ++m->reads;
    if (m->reads==m->failAt) return false;
    if (m->allOnes) {*out=UINT32_MAX;return true;}
    if (offset == 0x1004u) {
        *out=m->life+(m->changing && m->reads==3u ? 1u : 0u);
        return true;
    }
    if (offset == 0x1020u) {*out=m->version;return true;}
    if (offset == 0x1040u) {*out=m->boot;return true;}
    return false;
}
int main() {
    const Plan p={N33_PSP_STOPPED,0x1004u,0x1020u};
    Mock m{};
    assert(readOnce(p,false,mockRead,&m).state==N33_PSP_DISABLED);
    assert(m.reads==0u);
    assert(readOnce(Plan{},true,mockRead,&m).state==N33_PSP_NO_INVENTORY);
    assert(m.reads==0u);
    assert(readOnce(p,true,nullptr,&m).state==N33_PSP_READ_ERROR);
    assert(m.reads==0u);
    assert(readOnce({N33_PSP_STOPPED,0x1004u,0x1004u},true,mockRead,&m).state==N33_PSP_BAD_MMIO_PLAN);
    assert(m.reads==0u);
    auto s=readOnce(p,true,mockRead,&m);
    assert(s.state==N33_PSP_RUNNING);
    assert(s.signOfLife==1u && s.sosVersion==0x01020304u);
    assert(s.readCount==3u && m.reads==3u);
    auto info=report(s);
    assert(info.size==64u && info.state==N33_PSP_RUNNING);
    assert(info.sos_version==0x01020304u && info.reserved0==0u);
    assert(info.reserved1==0u && info.reserved2==0u && info.reserved3==0u);
    m=Mock{};m.life=0u;
    assert(readOnce(p,true,mockRead,&m).state==N33_PSP_STOPPED && m.reads==1u);
    m=Mock{};m.allOnes=true;
    assert(readOnce(p,true,mockRead,&m).state==N33_PSP_INVALID_READ);
    m=Mock{};m.failAt=2u;
    assert(readOnce(p,true,mockRead,&m).state==N33_PSP_READ_ERROR);
    m=Mock{};m.failAt=3u;
    assert(readOnce(p,true,mockRead,&m).state==N33_PSP_READ_ERROR);
    m=Mock{};m.changing=true;
    assert(readOnce(p,true,mockRead,&m).state==N33_PSP_UNSTABLE);
    // Bootloader mailbox: read once whatever the sOS state, never written.
    const Plan b={N33_PSP_STOPPED,0x1004u,0x1020u,0x1040u};
    uint32_t word=0u;
    m=Mock{};m.life=0u;
    assert(readOnce(b,true,mockRead,&m).state==N33_PSP_STOPPED);
    assert(readBootloader(b,mockRead,&m,&word) && word==kBootloaderReady);
    assert(m.reads==2u);
    m=Mock{};
    assert(!readBootloader(p,mockRead,&m,&word) && m.reads==0u);
    assert(!readBootloader(Plan{},mockRead,&m,&word) && m.reads==0u);
    assert(!readBootloader(b,nullptr,&m,&word));
    assert(!readBootloader(b,mockRead,&m,nullptr) && m.reads==0u);
    m=Mock{};m.failAt=1u;
    assert(!readBootloader(b,mockRead,&m,&word));
    static_assert(!kCanWriteMailbox && !kCanLoadFirmware,"status only");
    std::puts("PSP13 host MMIO reads and fail-safe diagnostics passed");
}
