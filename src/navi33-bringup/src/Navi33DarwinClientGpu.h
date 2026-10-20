// SPDX-License-Identifier: MIT
// IOKit host of the per-client GPU contexts (Navi33ClientGpu.h, Phase B4). Keeps BAR5
// and the CPU-visible BAR0 mapped for the life of the kext once a client opened a
// context. Register writes go only through client_gpu::allowed; CPU VRAM access only
// inside live, unpinned blocks of the provider's heap. The provider serializes every
// call under its PSP lock.
#pragma once
#include "Navi33ClientGpu.h"
#include "Navi33DarwinPspLoad.h"
class IOMemoryMap;
class IOMemoryDescriptor;
namespace navi33 { namespace darwin_client {
struct Host {
    IOMemoryMap *regs=nullptr, *bar=nullptr;
    client_gpu::Regs r{};
    client_gpu::Device dev;
    const vram_heap::Heap *heap=nullptr;
    uint64_t fbBaseMc=0, bar0Phys=0, bar0Bytes=0;
    int startCode=-1;              // client_gpu::Code of the one start attempt, -1 before it
};
// The first context of the boot starts the device: maps, BAR0 == VRAM proof (as every
// stage), heap reservations, GFXHUB. Later calls return the recorded outcome.
client_gpu::Code start(Host &h,IOPCIDevice *pci,const darwin_psp::Request &console,vram_heap::Heap &heap);
client_gpu::Hw hw(Host &h);
// A CPU mapping of one client buffer: a sub-range of BAR0 (the caller releases it).
IOMemoryDescriptor *boMemory(Host &h,IOPCIDevice *pci,uint32_t slot,uint32_t handle);
} }
