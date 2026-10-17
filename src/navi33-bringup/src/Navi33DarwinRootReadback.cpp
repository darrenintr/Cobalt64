// SPDX-License-Identifier: MIT
// Independent physical VRAM root readback via PCI BAR0. Read only.
// Does not claim a BO from AMD firmware / macOS or program GMC11.
#include "Navi33DarwinRootReadback.h"
#include "Navi33Identity.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_vram {
bool RootReadback::openReadOnly(IOPCIDevice *pci,
                                 const vram_geometry::Geometry &geometry,
                                 const vram_binding::Root &root) {
    close();
    if(!pci || !root.valid)return false;
    const auto id=inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!isSupported(id))return false;
    if(root.reservation.ownedBoBytes!=vram_geometry::kPage ||
       root.reservation.rootMcAddress!=root.rootPhysicalAddress ||
       root.reservation.ownedBoMcStart!=root.rootPhysicalAddress)
        return false;
    if(root.rootPhysicalAddress<geometry.physicalFbBase ||
       root.rootPhysicalAddress-geometry.physicalFbBase!=root.cpuBar0Offset)
        return false;
    const auto locate=vram_geometry::locate(
        geometry,root.cpuBar0Offset,vram_geometry::kPage);
    if(!locate.valid || !locate.cpuBar0Visible ||
       locate.gpuPhysicalAddress!=root.rootPhysicalAddress)
        return false;
    IODeviceMemory *resource=pci->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0);
    if(!resource)return false;
    const uint64_t length=static_cast<uint64_t>(resource->getLength());
    if(root.cpuBar0Offset>length ||
       vram_geometry::kPage>length-root.cpuBar0Offset)
        return false;
    IOMemoryMap *map=resource->map();
    if(!map)return false;
    const uint64_t mapped=static_cast<uint64_t>(map->getLength());
    if(!map->getVirtualAddress() || root.cpuBar0Offset>mapped ||
       vram_geometry::kPage>mapped-root.cpuBar0Offset) {
        map->release();return false;
    }
    pci->retain();
    pci_=pci;
    bar0_=map;
    rootPhysical_=root.rootPhysicalAddress;
    rootBarOffset_=root.cpuBar0Offset;
    return true;
}

void RootReadback::close() {
    rootPhysical_=0u;
    rootBarOffset_=0u;
    if(bar0_) {bar0_->release();bar0_=nullptr;}
    if(pci_) {pci_->release();pci_=nullptr;}
}

bool RootReadback::readPhysical(uint64_t address,uint64_t *value) const {
    if(!ready() || !value || address<rootPhysical_ ||
       address-rootPhysical_>vram_geometry::kPage ||
       vram_geometry::kPage-(address-rootPhysical_)<sizeof(uint64_t) ||
       ((address-rootPhysical_)&7u))
        return false;
    const uint64_t byteOffset=rootBarOffset_+(address-rootPhysical_);
    const uint64_t length=static_cast<uint64_t>(bar0_->getLength());
    if(byteOffset>length || length-byteOffset<sizeof(uint64_t))
        return false;
    const uintptr_t base=static_cast<uintptr_t>(bar0_->getVirtualAddress());
    if(!base || byteOffset>UINTPTR_MAX-base)return false;
    // PCI BAR aperture read only: absent/all-ones is not a valid root
    // page-table entry. No volatile store is present anywhere in this class.
    const auto *word=reinterpret_cast<volatile const uint64_t *>(
        base+static_cast<uintptr_t>(byteOffset));
    const uint64_t observed=*word;
    if(observed==UINT64_MAX)return false;
    *value=observed;
    return true;
}
bool RootReadback::readWord(void *context,uint64_t address,uint64_t *out) {
    auto *self=static_cast<RootReadback *>(context);
    return self && self->readPhysical(address,out);
}
} } // namespace navi33::darwin_vram
