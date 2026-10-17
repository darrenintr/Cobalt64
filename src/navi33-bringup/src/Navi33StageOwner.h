// SPDX-License-Identifier: MIT
// Provider-wide root-client ownership of ONE pinned macOS DMA staging session.
//
// Does NOT confer PCI, GPU VMID0, SDMA firmware or queue ownership. It only
// prevents simultaneous N33N clients from holding competing diagnostic DMA
// sessions. All methods must be serialized by the provider IOLock.
#pragma once
#include <stdint.h>
namespace navi33 { namespace stage_owner {

struct Ticket {
    uint64_t epoch=0u;  // 0 == refused/not owned
    constexpr explicit operator bool() const {return epoch!=0u;}
};

class Registry {
public:
    Ticket claim(const void *client) {
        if(shuttingDown_ || !client)return {};
        if(owner_) return owner_==client ? Ticket{epoch_} : Ticket{};
        if(epoch_==UINT64_MAX)return {}; // no recycled generations
        ++epoch_;
        owner_=client;
        return {epoch_};
    }
    bool holds(const void *client,Ticket ticket) const {
        return !shuttingDown_ && client && owner_==client &&
               ticket.epoch!=0u && ticket.epoch==epoch_;
    }
    bool release(const void *client,Ticket ticket) {
        if(!holds(client,ticket))return false;
        owner_=nullptr;
        return true;
    }
    void shutdown() {
        shuttingDown_=true;
        owner_=nullptr;
    }
    bool available() const {return !shuttingDown_ && !owner_;}
private:
    const void *owner_=nullptr;
    uint64_t epoch_=0u;
    bool shuttingDown_=false;
};
} } // namespace navi33::stage_owner
