// SPDX-License-Identifier: MIT
// Real macOS device-IOMMU mappings retained only for an N33N root client.
// No GPU hardware register writes or command submission in this class.
#pragma once
#include "Navi33NativeABI.h"
#include <stdint.h>

class IOPCIDevice;
class IOMapper;
class IOBufferMemoryDescriptor;
class IODMACommand;

namespace navi33 { namespace darwin_stage {
class Session {
public:
    ~Session() { release(); }
    n33_dma_stage_status prepare(IOPCIDevice *pci);
    n33_dma_stage_status snapshot() const;
    void release();

private:
    IOMapper *mapper_=nullptr;
    IOBufferMemoryDescriptor *memory_=nullptr;
    IODMACommand *command_=nullptr;
    bool memoryPrepared_=false;
    bool descriptorBound_=false;
    bool dmaPrepared_=false;
    n33_dma_stage_status cached_{};
    n33_dma_stage_status failure(n33_dma_stage_state code);
};
static_assert(sizeof(n33_dma_stage_status)==64,"Native DMA staging status ABI");
} }
