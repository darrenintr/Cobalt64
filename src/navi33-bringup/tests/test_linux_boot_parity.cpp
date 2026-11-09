// SPDX-License-Identifier: MIT
// Host-only ordering checks. NO hardware access or implied GPU success.
#include "../src/Navi33LinuxBootParity.h"
#include <assert.h>

using namespace navi33::linux_boot_parity;

int main() {
    Config cfg{};
    cfg.exactGc1102 = true;
    cfg.pspFirmwarePath = true;
    cfg.mesKiq = true;
    cfg.asyncGfx = true;
    cfg.exclusiveCardOwner = true;
    cfg.retainedGpuMemory = true;
    cfg.mesSchedulerVersion = 0x92u;

    Plan p = makePlan(cfg);
    assert(p.ready() && p.count == kMaxStages);
    assert(p.stages[12] == Stage::MesKiqQueueInit);
    assert(p.stages[13] == Stage::MesSchedulerQueueMap);
    assert(p.stages[14] == Stage::MesSetHwResources);
    assert(p.stages[15] == Stage::MesSetHwResources1);
    assert(p.stages[16] == Stage::MesQuerySchedulerStatus);
    assert(p.stages[17] == Stage::MesKiqOwnershipTransfer);
    assert(p.stages[19] == Stage::MesMapComputeQueues);
    assert(p.stages[21] == Stage::MesMapGfxQueues);
    assert(checkRecordedOrder(p, p.stages, p.count).status == Status::Ready);

    Stage corrupted[kMaxStages]{};
    for (size_t i=0;i<p.count;++i) corrupted[i]=p.stages[i];
    corrupted[13] = Stage::MesMapGfxQueues;
    OrderCheck o = checkRecordedOrder(p, corrupted, p.count);
    assert(o.status == Status::TraceMismatch && o.firstMismatch == 13u);
    assert(checkRecordedOrder(p, corrupted, p.count-1).status == Status::TraceMismatch);
    assert(checkRecordedOrder(p, nullptr, p.count).status == Status::TraceMismatch);

    cfg.mesSchedulerVersion=0x48u;
    p=makePlan(cfg);
    assert(p.ready() && p.count == kMaxStages-1u);
    assert(p.stages[15] == Stage::MesQuerySchedulerStatus);
    assert(checkRecordedOrder(p,p.stages,p.count).status == Status::Ready);

    cfg.mesSchedulerVersion=0x46u;
    assert(makePlan(cfg).status == Status::FirmwareUnsupported);
    cfg.mesSchedulerVersion=0x92u;
    cfg.g34G42Enabled=true;
    assert(makePlan(cfg).status == Status::DirectQueueConflict);
    cfg.g34G42Enabled=false;
    cfg.exclusiveCardOwner=false;
    assert(makePlan(cfg).status == Status::MissingOwnership);
    cfg.exclusiveCardOwner=true;
    cfg.retainedGpuMemory=false;
    assert(makePlan(cfg).status == Status::MissingOwnership);
    cfg.retainedGpuMemory=true;
    cfg.mesKiq=false;
    assert(makePlan(cfg).status == Status::WrongLinuxPath);
    cfg.mesKiq=true;
    cfg.exactGc1102=false;
    assert(makePlan(cfg).status == Status::WrongSilicon);
}
