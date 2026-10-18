// RX 7600 macOS DMA-address preflight, never a GPU execution path.
// Apple IOKit API references: IODMACommand, IOBufferMemoryDescriptor,
// IOMapper::copyMapperForDevice, IOMemoryDescriptor prepare()/complete().
// When available, the OS IOMMU is asked for temporary I/O bus addresses for
// one 64 KiB *system RAM* buffer. If there is no mapper, a separate
// kIOMemoryMapperNone path checks CPU-physical candidates. Both remain inside
// a CPU-owned shadow PTE array and never reach the GPU.
#include "Navi33DarwinDma.h"
#include "Navi33DmaScatter.h"
#include "Navi33Identity.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 {
namespace darwin_dma {

namespace {
struct Lifetime {
    IOBufferMemoryDescriptor *memory=nullptr;
    IOMapper *mapper=nullptr;
    IODMACommand *command=nullptr;
    bool memoryPrepared=false;
    bool descriptorBound=false;
    bool dmaPrepared=false;

    ~Lifetime() {
        // Revoke the DMA translation first, then unpin RAM. Never leave a
        // bus-address mapping alive past the diagnostic's return.
        if(command) {
            if(dmaPrepared) (void)command->complete();
            if(descriptorBound) (void)command->clearMemoryDescriptor(false);
            command->release();
        }
        if(memory) {
            if(memoryPrepared) (void)memory->complete();
            memory->release();
        }
        if(mapper) mapper->release();
    }
};

struct PhysicalLifetime {
    IOBufferMemoryDescriptor *memory=nullptr;
    bool prepared=false;
    ~PhysicalLifetime() {
        if(memory) {
            if(prepared) (void)memory->complete();
            memory->release();
        }
    }
};

// Some x86 macOS boots have no installed IOKit device mapper. In that
// configuration, report only whether ordinary wired pages have representable
// CPU-physical addresses. Do not call these addresses GPU bus addresses and
// never submit DMA from this preflight.
Report inspectPhysicalCandidate() {
    constexpr mach_vm_size_t bytes=static_cast<mach_vm_size_t>(dma::kDiagnosticBytes);
    constexpr mach_vm_address_t physMask=0x0000FFFFFFFFFFFFull &
        ~(static_cast<mach_vm_address_t>(gmc11::kPageBytes)-1ull);
    PhysicalLifetime guard{};
    guard.memory=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMemoryMapperNone,
        bytes,physMask);
    if(!guard.memory || guard.memory->getLength()<bytes)
        return {Code::PhysicalNoMemory,0,0};
    if(guard.memory->prepare(kIODirectionInOut)!=kIOReturnSuccess)
        return {Code::PhysicalPrepareFailed,0,0};
    guard.prepared=true;

    IOByteCount contiguousBytes=0;
    const addr64_t physical=guard.memory->getPhysicalSegment(
        0u,&contiguousBytes,kIOMemoryMapperNone);
    if(!physical || contiguousBytes<bytes ||
       (physical&(gmc11::kPageBytes-1u)) ||
       physical>=gmc11::kMaxConservativeDmaAddress ||
       bytes>gmc11::kMaxConservativeDmaAddress-physical)
        return {Code::PhysicalAddressInvalid,0,0};
    dma::Segment pages[dma::kMaxPages]{};
    for(size_t i=0u;i<dma::kMaxPages;++i)
        pages[i]={static_cast<uint64_t>(physical+
                          i*gmc11::kPageBytes),gmc11::kPageBytes};

    uint64_t pageAddresses[dma::kMaxPages]{};
    const dma::Plan flattened=dma::flatten(
        pages,dma::kMaxPages,dma::kDiagnosticBytes,
        pageAddresses,dma::kMaxPages);
    if(flattened.code!=dma::Status::Pass)
        return {Code::PhysicalAddressInvalid,0,0};

    // Validate the same GFX11 PTE encoding in an ordinary CPU array. This
    // does not install a page table or establish that the GPU can reach it.
    const gmc11::Aperture fakeGpuVa{0x100000000ull,dma::kDiagnosticBytes};
    uint64_t shadow[dma::kMaxPages]{};
    const dma::Plan mapped=dma::mapIntoShadow(
        pages,dma::kMaxPages,dma::kDiagnosticBytes,fakeGpuVa,
        fakeGpuVa.gpuVaBase,shadow,dma::kMaxPages);
    if(mapped.code!=dma::Status::Pass)
        return {Code::PhysicalAddressInvalid,0,0};
    for(size_t i=0u;i<mapped.pages;++i)
        if(gmc11::parseSystemPte(shadow[i]).code!=gmc11::Code::Pass)
            return {Code::PhysicalAddressInvalid,0,0};
    if(gmc11::unmapSystemPages(fakeGpuVa,fakeGpuVa.gpuVaBase,
           mapped.pages,shadow,dma::kMaxPages).code!=gmc11::Code::Pass)
        return {Code::PhysicalAddressInvalid,0,0};
    for(size_t i=0u;i<dma::kMaxPages;++i)
        if(shadow[i]!=0u)
            return {Code::PhysicalAddressInvalid,0,0};

    return {Code::PhysicalAddressOnly,1u,
            static_cast<uint8_t>(flattened.pages)};
}
} // anonymous

Report runOnce(IOPCIDevice *pci) {
    if(!pci) return {Code::RefusedDevice,0,0};
    const Identity id=inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!isSupported(id)) return {Code::RefusedDevice,0,0};

    Lifetime guard{};
    // Prefer a device-associated IOVA. If this platform exposes no mapper,
    // inspect raw CPU-physical pages separately and return an explicitly
    // non-DMA status; never treat them as a successful device mapping.
    guard.mapper=IOMapper::copyMapperForDevice(pci);
    if(!guard.mapper) return inspectPhysicalCandidate();

    guard.memory=IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut, static_cast<vm_size_t>(dma::kDiagnosticBytes),
        static_cast<vm_offset_t>(gmc11::kPageBytes));
    if(!guard.memory || !guard.memory->getBytesNoCopy())
        return {Code::NoMemory,0,0};

    // Fill known bytes entirely in *ordinary system RAM*, not VRAM/BAR0.
    uint8_t *host=static_cast<uint8_t *>(guard.memory->getBytesNoCopy());
    for(size_t i=0;i<dma::kDiagnosticBytes;++i)
        host[i]=static_cast<uint8_t>((i ^ (i>>8u)) & 255u);

    if(guard.memory->prepare(kIODirectionInOut)!=kIOReturnSuccess)
        return {Code::MemoryPrepareFailed,0,0};
    guard.memoryPrepared=true;

    guard.command=IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 48u, dma::kDiagnosticBytes,
        IODMACommand::kMapped, dma::kDiagnosticBytes,
        static_cast<UInt32>(gmc11::kPageBytes), guard.mapper, nullptr);
    if(!guard.command) return {Code::CommandCreationFailed,0,0};

    if(guard.command->setMemoryDescriptor(guard.memory,false)!=kIOReturnSuccess)
        return {Code::CommandBindFailed,0,0};
    guard.descriptorBound=true;

    if(guard.command->prepare(0,dma::kDiagnosticBytes)!=kIOReturnSuccess)
        return {Code::CommandPrepareFailed,0,0};
    guard.dmaPrepared=true;

    dma::Segment segments[dma::kMaxPages]{};
    size_t count=0;
    UInt64 offset=0;
    while(offset<dma::kDiagnosticBytes) {
        if(count>=dma::kMaxPages) return {Code::SegmentQueryFailed,0,0};
        IODMACommand::Segment64 pair{};
        UInt32 n=1u;
        const UInt64 old=offset;
        const IOReturn err=guard.command->gen64IOVMSegments(&offset,&pair,&n);
        if((err!=kIOReturnSuccess && err!=kIOReturnOverrun) ||
           n!=1u || offset<=old || offset>dma::kDiagnosticBytes ||
           pair.fLength!=offset-old)
            return {Code::SegmentQueryFailed,0,0};
        segments[count++]={static_cast<uint64_t>(pair.fIOVMAddr),
                           static_cast<uint64_t>(pair.fLength)};
    }

    const gmc11::Aperture fakeGpuVa{0x100000000ull,dma::kDiagnosticBytes};
    uint64_t shadow[dma::kMaxPages]{};
    const dma::Plan mapped=dma::mapIntoShadow(
        segments,count,dma::kDiagnosticBytes,fakeGpuVa,fakeGpuVa.gpuVaBase,
        shadow,dma::kMaxPages);
    if(mapped.code!=dma::Status::Pass)
        return {Code::ScatterRejected,static_cast<uint8_t>(count),0};
    for(size_t i=0;i<mapped.pages;++i) {
        if(gmc11::parseSystemPte(shadow[i]).code!=gmc11::Code::Pass)
            return {Code::ShadowValidationFailed,
                    static_cast<uint8_t>(count),static_cast<uint8_t>(mapped.pages)};
    }
    if(gmc11::unmapSystemPages(fakeGpuVa,fakeGpuVa.gpuVaBase,
           mapped.pages,shadow,dma::kMaxPages).code!=gmc11::Code::Pass)
        return {Code::ShadowPageTableRejected,static_cast<uint8_t>(count),0};
    for(size_t i=0;i<dma::kMaxPages;++i)
        if(shadow[i]!=0u)
            return {Code::ShadowValidationFailed,static_cast<uint8_t>(count),0};

    // Retained DMA IOVAs are NOT returned. Lifetime destructor explicitly
    // unmaps IOMMU pages and completes the descriptor before exiting.
    IOLog("Navi33Dma: verified %u OS DMA segments / %u shadow GART pages; GPU untouched\n",
          static_cast<unsigned>(count),static_cast<unsigned>(mapped.pages));
    return {Code::Pass,static_cast<uint8_t>(count),
            static_cast<uint8_t>(mapped.pages)};
}
} // namespace darwin_dma
} // namespace navi33
