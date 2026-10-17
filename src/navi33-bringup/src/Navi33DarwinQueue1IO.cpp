// SPDX-License-Identifier: MIT
// Isolated physical IOKit callbacks: real BAR5 reads + retained DMA
// buffer/cache sync. There is deliberately NO GPU MMIO write path.
#include "Navi33DarwinQueue1IO.h"
#include "Navi33Identity.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_queue1 {

bool PhysicalIO::openReadOnly(IOPCIDevice *pci,
                              darwin_stage::Session *stage,
                              const IPDiscovery &discovery) {
    close();
    if(!pci || !stage || !stage->pinnedAndMapped())return false;
    const Identity id=inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!isSupported(id))return false;
    // Reject unverified IP discovery; never guess BASE_IDX or BAR size.
    if(inventory(discovery).code!=InventoryCode::Pass ||
       !discovery.isNavi33Candidate())return false;
    IODeviceMemory *range=pci->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress5);
    if(!range || range->getLength()<4u)return false;
    const uint64_t bytes=static_cast<uint64_t>(range->getLength());
    const auto plan=sdma6_bar::buildPlan(discovery,bytes,1u);
    if(!sdma6_bar::validPlan(plan))return false;
    IOMemoryMap *map=range->map();
    if(!map)return false;
    if(static_cast<uint64_t>(map->getLength())<bytes ||
       !map->getVirtualAddress()) {
        map->release();
        return false;
    }
    pci->retain();
    pci_=pci;
    bar5_=map;
    stage_=stage;
    queue1Plan_=plan;
    return true;
}
void PhysicalIO::close() {
    stage_=nullptr;
    queue1Plan_={};
    if(bar5_) {bar5_->release();bar5_=nullptr;}
    if(pci_) {pci_->release();pci_=nullptr;}
}
bool PhysicalIO::readBar5(uint64_t byteOffset,uint32_t *out) const {
    if(!bar5_ || !out || (byteOffset&3ull) ||
       byteOffset>static_cast<uint64_t>(bar5_->getLength()) ||
       static_cast<uint64_t>(bar5_->getLength())-byteOffset<4u)
        return false;
    const uintptr_t addr=static_cast<uintptr_t>(bar5_->getVirtualAddress());
    if(!addr || byteOffset>UINTPTR_MAX-addr)return false;
    // MMIO mapping is a device resource, not a RAM or PCI ROM pointer.
    // Volatile prevents ordinary compiler load elimination. Access is
    // serialized externally; this does not lock the device's firmware.
    const auto *reg=reinterpret_cast<volatile const uint32_t *>(
        addr+static_cast<uintptr_t>(byteOffset));
    const uint32_t value=*reg;
    if(value==UINT32_MAX)return false; // absent/unmapped device window
    *out=value;
    return true;
}
bool PhysicalIO::readQueueRegister(void *opaque,uint32_t reg,uint32_t *out) {
    auto *self=static_cast<PhysicalIO *>(opaque);
    if(!self || !self->isBound() || !out)return false;
    uint64_t off=0u;
    if(!sdma6_bar::translate(self->queue1Plan_,reg,&off))return false;
    return self->readBar5(off,out);
}
bool PhysicalIO::denyQueueRegisterWrite(void *,uint32_t,uint32_t) {
    // DO NOT turn this into an ordinary BAR5 write callback. That would
    // bypass PCI/firmware ownership, GMC11/VRAM BO and recovery proof.
    return false;
}
bool PhysicalIO::beforeGpu(void *opaque) {
    auto *self=static_cast<PhysicalIO *>(opaque);
    return self && self->isBound() &&
           self->stage_->synchronizeForDevice();
}
bool PhysicalIO::afterGpu(void *opaque) {
    auto *self=static_cast<PhysicalIO *>(opaque);
    return self && self->isBound() &&
           self->stage_->synchronizeForHost();
}
bool PhysicalIO::readStagedByte(void *opaque,size_t off,uint8_t *out) {
    auto *self=static_cast<PhysicalIO *>(opaque);
    return self && self->isBound() &&
           self->stage_->readByte(off,out);
}
void PhysicalIO::delayUs(void *,uint32_t micros) {
    if(micros>=1u && micros<=100000u)IODelay(micros);
}
sdma6_mmio::IO PhysicalIO::callbacks() {
    if(!isBound())return {};
    return {this,readQueueRegister,denyQueueRegisterWrite,
            beforeGpu,afterGpu,readStagedByte,delayUs};
}
} } // namespace navi33::darwin_queue1
