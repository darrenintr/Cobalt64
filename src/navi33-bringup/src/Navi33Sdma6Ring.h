// Offline software SDMA6 command-ring model. NO GPU submission.
// References (AMD MIT): Linux amdgpu_ring.c ring_write/commit/undo and
// sdma_v6_0.c copy packet definitions.
// Ring words remain CPU memory; no pinned BO, doorbell, wptr hardware,
// MMIO mapping, firmware initialization, interrupts or real fence waits.
#pragma once
#include "Navi33Sdma6Packet.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 {
namespace sdma6 {

constexpr size_t kMaxPendingFences=32u;
constexpr size_t kMinRingDwords=8u;
constexpr size_t kMaxRingDwords=4096u;

enum class RingCode : uint8_t {
    Pass=0, NotInitialized, BadRingSize, InvalidPacket,
    NoSpace, TooManyBatches, NoPublishedPacket, InvalidFence, Overflow
};

struct RingResult {
    RingCode code=RingCode::NotInitialized;
    uint64_t fence=0u;
};

class Ring {
public:
    bool reset(uint32_t *storage,size_t dwordCount) {
        storage_=nullptr;length_=0;consumed_=0;published_=0;
        issued_=0;completed_=0;next_=0;pendingCount_=0;
        for(auto &p: pending_) p={0u,0u};
        if(!storage || dwordCount<kMinRingDwords ||
           dwordCount>kMaxRingDwords ||
           (dwordCount&(dwordCount-1u))!=0u)return false;
        storage_=storage;length_=dwordCount;
        for(size_t i=0;i<length_;++i)storage_[i]=0u;
        return true;
    }
    RingResult publish(const CopyPacket *packets,size_t n) {
        if(!storage_)return {RingCode::NotInitialized,0u};
        if(!packets || !n || n>length_/kCopyPacketDwords)
            return {RingCode::InvalidPacket,0u};
        if(pendingCount_>=kMaxPendingFences)
            return {RingCode::TooManyBatches,0u};
        if(issued_==UINT64_MAX || n>UINT64_MAX/kCopyPacketDwords)
            return {RingCode::Overflow,0u};
        const uint64_t dwords=static_cast<uint64_t>(n)*kCopyPacketDwords;
        if(published_>UINT64_MAX-dwords)
            return {RingCode::Overflow,0u};
        if(published_<consumed_ ||
           published_-consumed_>length_ ||
           dwords>length_-(published_-consumed_))
            return {RingCode::NoSpace,0u};
        for(size_t i=0;i<n;++i)
            if(packets[i].status!=CopyCode::Pass ||
               inspectCopy(packets[i].words,kCopyPacketDwords).status != CopyCode::Pass)
                return {RingCode::InvalidPacket,0u};

        // Copy every validated packet first; a single pointer assignment
        // publishes the complete batch for this single-threaded simulator.
        for(size_t i=0;i<n;++i) {
            for(size_t j=0;j<kCopyPacketDwords;++j) {
                const size_t index=static_cast<size_t>(
                    (published_+i*kCopyPacketDwords+j)&(length_-1u));
                storage_[index]=packets[i].words[j];
            }
        }
        published_+=dwords;
        issued_++;
        pending_[next_]={issued_,published_};
        next_=(next_+1u)%kMaxPendingFences;
        pendingCount_++;
        return {RingCode::Pass,issued_};
    }
    RingCode peek(CopyDescription &out) const {
        out=CopyDescription{};
        if(!storage_)return RingCode::NotInitialized;
        if(consumed_==published_)return RingCode::NoPublishedPacket;
        if(published_-consumed_<kCopyPacketDwords)
            return RingCode::InvalidPacket;
        uint32_t dwords[kCopyPacketDwords]{};
        for(size_t i=0;i<kCopyPacketDwords;++i)
            dwords[i]=storage_[(consumed_+i)&(length_-1u)];
        out=inspectCopy(dwords,kCopyPacketDwords);
        return out.status==CopyCode::Pass?RingCode::Pass:RingCode::InvalidPacket;
    }
    RingCode consume() {
        CopyDescription d{};
        const RingCode status=peek(d);
        if(status!=RingCode::Pass)return status;
        consumed_+=kCopyPacketDwords;
        while(pendingCount_>0u) {
            const size_t oldest=(next_+kMaxPendingFences-pendingCount_)%
                                kMaxPendingFences;
            const Pending f=pending_[oldest];
            if(f.writeEnd>consumed_)break;
            completed_=f.id;
            pending_[oldest]={0u,0u};
            pendingCount_--;
        }
        return RingCode::Pass;
    }
    bool fenceComplete(uint64_t sequence) const {
        return storage_ && sequence!=0u && sequence<=completed_;
    }
    bool fenceKnown(uint64_t sequence) const {
        return storage_ && sequence!=0u && sequence<=issued_;
    }
    uint64_t publishedDwords() const {return published_;}
    uint64_t consumedDwords() const {return consumed_;}
    uint64_t lastIssuedFence() const {return issued_;}
    uint64_t lastCompletedFence() const {return completed_;}
    size_t inFlightBatches() const {return pendingCount_;}

private:
    struct Pending {
        uint64_t id,writeEnd;
    };
    uint32_t *storage_=nullptr;
    size_t length_=0;
    uint64_t consumed_=0,published_=0;
    uint64_t issued_=0,completed_=0;
    size_t next_=0,pendingCount_=0;
    Pending pending_[kMaxPendingFences]{};
};

constexpr bool kCanRingDoorbell = false;
constexpr bool kCanSubmitToHardware = false;
static_assert(!kCanRingDoorbell && !kCanSubmitToHardware,
              "Software ring may not publish a PCI/SDMA hardware pointer");
} // namespace sdma6
} // namespace navi33
