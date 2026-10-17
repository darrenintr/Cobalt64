// Normal Navi33 macOS Kext must never contain MMHUB hardware writes.
#include "../src/Navi33Gmc11Mmhub.h"
#include <cassert>
int main(){
    static_assert(!navi33::mmhub11::kWritePathCompiled,
                  "MMHUB write backend must be disabled by default");
    navi33::mmhub11::Controller c{};
    assert(c.prepare({}, {}, {}, {}).code==navi33::mmhub11::Code::Disabled);
    assert(c.invalidateAndActivate().code==navi33::mmhub11::Code::Disabled);
    assert(c.abortBeforeActivation().code==navi33::mmhub11::Code::Disabled);
    assert(c.mayReleaseMappedMemory());
}
