// The Kext/default build must not compile a physical register write path.
#include "../src/Navi33Sdma6MmioQueue.h"
#include <cassert>
#include <cstdio>
using namespace navi33::sdma6_mmio;
int main() {
    static_assert(!kWriteBackendCompiled,
                  "Unsafe MMIO must be OFF by default");
    Queue q{};
    const Observation o=q.configure(Preconditions{},Request{},IO{});
    assert(o.status==Code::NotBuilt);
    assert(q.submit().status==Code::NotBuilt);
    assert(q.waitAndVerify(1u).status==Code::NotBuilt);
    assert(q.stopAfterCompleted().status==Code::NotBuilt);
    assert(q.abortBeforeSubmit().status==Code::NotBuilt);
    assert(q.mayUnmapGpuMemory());
    std::puts("Default Navi33 build cannot write SDMA MMIO registers");
}
