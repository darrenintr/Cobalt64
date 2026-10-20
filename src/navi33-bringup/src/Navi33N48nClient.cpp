// SPDX-License-Identifier: MIT
#include "Navi33N48nClient.hpp"
#include "Navi33Probe.hpp"
#include <libkern/libkern.h>

OSDefineMetaClassAndStructors(Navi33N48nClient, IOUserClient)

namespace {
// Struct sizes per selector (0: none). SUBMIT is 32 + 32 n; READREGS outputs 4 n.
bool shapeOk(uint32_t sel, const IOExternalMethodArguments *a) {
    if (a->structureInputDescriptor || a->structureOutputDescriptor) return false;
    const uint32_t si = a->scalarInputCount, so = a->scalarOutputCount;
    const uint64_t in = a->structureInputSize, out = a->structureOutputSize;
    switch (sel) {
    case N48N_SEL_HELLO: return si == 2u && so == 4u && !in && !out;
    case N48N_SEL_QUERYINFO: return !si && !so && !in && out == sizeof(n48n_info);
    case N48N_SEL_READREGS: return si == 3u && !so && !in && out >= 4u && out <= 64u && !(out & 3u);
    case N48N_SEL_BOCREATE: return !si && so == 4u && in == sizeof(n48n_gem_create_in) && !out;
    case N48N_SEL_BOFREE: return si == 1u && !so && !in && !out;
    case N48N_SEL_GEMVA: return !si && !so && in == sizeof(n48n_gem_va) && !out;
    case N48N_SEL_CTX: return !si && !so && in == sizeof(n48n_ctx) && out == sizeof(n48n_ctx);
    case N48N_SEL_SUBMIT: return !si && so == 1u && in > sizeof(n48n_cs_in) &&
        in <= sizeof(n48n_cs_in) + navi33::client_gpu::kMaxIbs * sizeof(n48n_cs_ib) &&
        !((in - sizeof(n48n_cs_in)) % sizeof(n48n_cs_ib)) && !out;
    case N48N_SEL_WAITSEQ: return si == 3u && so == 3u && !in && !out;
    default: return false;
    }
}
}

bool Navi33N48nClient::initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) {
    if (type != N48N_UC_TYPE ||
        clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return false;
    if (!super::initWithTask(owningTask, securityID, type, properties)) return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}

bool Navi33N48nClient::start(IOService *provider) {
    Navi33Probe *probe = owner_ ? owner_ : OSDynamicCast(Navi33Probe, provider);
    if (!probe || !super::start(provider)) return false;
    owner_ = probe;
    return true;
}

IOReturn Navi33N48nClient::create(Navi33Probe *owner, IOService *provider, task_t task, void *securityID, UInt32 type,
                                  IOUserClient **out) {
    if (!owner || !provider || !out) return kIOReturnBadArgument;
    auto *c = OSTypeAlloc(Navi33N48nClient);
    if (!c) return kIOReturnNoMemory;
    if (!c->initWithTask(task, securityID, type, nullptr)) { c->release(); return kIOReturnNotPrivileged; }
    c->owner_ = owner;
    if (!c->attach(provider)) { c->release(); return kIOReturnInternalError; }
    if (!c->start(provider)) { c->detach(provider); c->release(); return kIOReturnInternalError; }
    *out = c;
    return kIOReturnSuccess;
}

void Navi33N48nClient::revokeCpuMappings(uint32_t handle) {
    for (uint32_t i = 0; i < kMaxBos; ++i) {
        if ((handle && i + 1u != handle) || !boMd_[i]) continue;
        while (IOMemoryMap *map = removeMappingForDescriptor(boMd_[i])) { map->unmap(); map->release(); }
        boMd_[i]->release();
        boMd_[i] = nullptr;
    }
    if (owner_ && session_.open) owner_->gpuCpuUnmapped(this, handle);
}

IOReturn Navi33N48nClient::clientClose() {
    if (lock_) IOLockLock(lock_);
    revokeCpuMappings(0u);
    if (owner_ && session_.open) owner_->gpuContext(this, N33_CTX_OP_CLOSE, nullptr);
    session_ = navi33::n48n_front::Session{};
    owner_ = nullptr;
    if (lock_) IOLockUnlock(lock_);
    terminate();
    return kIOReturnSuccess;
}

IOReturn Navi33N48nClient::clientDied() { return clientClose(); }

void Navi33N48nClient::free() {
    for (IOMemoryDescriptor *&md : boMd_) if (md) { md->release(); md = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    super::free();
}

IOReturn Navi33N48nClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (!options || !memory || !type || type > kMaxBos || !lock_) return kIOReturnBadArgument;
    IOLockLock(lock_);
    if (!owner_ || !session_.open) { IOLockUnlock(lock_); return kIOReturnNotReady; }
    IOMemoryDescriptor *md = boMd_[type - 1u];
    if (!md) {
        md = owner_->gpuBoMemory(this, type);
        if (!md) { IOLockUnlock(lock_); return kIOReturnBadArgument; }
        boMd_[type - 1u] = md;
    }
    md->retain();
    IOLockUnlock(lock_);
    *options = kIOMapInhibitCache;
    *memory = md;
    return kIOReturnSuccess;
}

IOReturn Navi33N48nClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                          IOExternalMethodDispatch *, OSObject *, void *) {
    if (!args || !lock_) return kIOReturnBadArgument;
    if (selector >= N48N_SEL_COUNT) return kIOReturnUnsupported;
    if (!shapeOk(selector, args)) return kIOReturnBadArgument;
    IOLockLock(lock_);
    IOReturn r = kIOReturnNotReady;
    if (!owner_) { IOLockUnlock(lock_); return r; }
    if (selector == N48N_SEL_HELLO) {
        const uint64_t *in = args->scalarInput;
        if (in[0] != N48N_ABI_VERSION || (in[1] & ~(1ull << 8))) r = kIOReturnBadArgument;
        else if (session_.open) r = kIOReturnSuccess;
        else if ((r = owner_->gpuContext(this, N33_CTX_OP_OPEN, &args->scalarOutput[3])) == kIOReturnSuccess)
            r = owner_->n48nAttach(this, session_);
        if (r == kIOReturnSuccess) {
            args->scalarOutput[0] = N48N_ABI_VERSION;
            args->scalarOutput[1] = owner_->kextBuild();
            args->scalarOutput[2] = navi33::client_gpu::kMaxIbs;
        }
    } else if (!session_.open) {
        r = kIOReturnNotReady;                    // HELLO first
    } else {
        if (selector == N48N_SEL_BOFREE) {
            if (!args->scalarInput[0] || args->scalarInput[0] > kMaxBos) { IOLockUnlock(lock_); return kIOReturnBadArgument; }
            revokeCpuMappings(static_cast<uint32_t>(args->scalarInput[0]));   // before the pages can be reused
        }
        r = owner_->n48nCall(this, session_, selector, args);
    }
    IOLockUnlock(lock_);
    return r;
}
