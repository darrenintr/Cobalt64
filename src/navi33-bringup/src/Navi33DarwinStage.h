// SPDX-License-Identifier: MIT
// Real macOS device-IOMMU mappings retained only for an N33N root client.
// No GPU hardware register writes or command submission in this class.
#pragma once
#include "Navi33NativeABI.h"
#include "Navi33DmaLifetime.h"
#include <stdint.h>
#include <stddef.h>

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
    // Kernel-only host-visible access to the exact retained device-IOMMU
    // staging allocation; these methods NEVER expose DMA addresses or a
    // mutable pointer to N33N user space.
    bool pinnedAndMapped() const;
    bool readByte(size_t offset,uint8_t *out) const;
    bool synchronizeForDevice();
    bool synchronizeForHost();
    // Kernel-only: caller MUST quarantine before its first MMIO write.
    // Once set, client close may NOT unpin this memory without a real
    // externally verified GPU reset. No in-process reset override exists.
    bool quarantineBeforePossibleGpuAccess();
    bool requiresExternalReset() const {
        return lifetime_.requiresExternalHardwareReset();
    }
    // Kernel-only diagnostic data, never copied to N33N userspace.
    // IOVA alone cannot authorize GMC11 page-table register writes.
    uint64_t candidateTableDeviceIova() const {
        return dmaPrepared_ && memoryPrepared_ &&
               cached_.state==N33_DMA_STAGED ? candidatePtIova_ : 0u;
    }
    void release();

private:
    IOMapper *mapper_=nullptr;
    IOBufferMemoryDescriptor *memory_=nullptr;
    IODMACommand *command_=nullptr;
    bool memoryPrepared_=false;
    bool descriptorBound_=false;
    bool dmaPrepared_=false;
    bool hostSynced_=false;
    dma_lifetime::State lifetime_{};
    uint64_t candidatePtIova_=0u;
    n33_dma_stage_status cached_{};
    n33_dma_stage_status failure(n33_dma_stage_state code);
};
static_assert(sizeof(n33_dma_stage_status)==64,"Native DMA staging status ABI");
} }
