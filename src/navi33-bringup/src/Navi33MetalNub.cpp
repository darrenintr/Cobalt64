// SPDX-License-Identifier: MIT
// See Navi33MetalNub.hpp. Ported from navi48-bringup's Navi48MetalNub.cpp (kext 0.0.664) without the
// display-pipe glue (the ops table is always "display off") and without the publish selectors.
#include "Navi33MetalNub.hpp"
#include "Navi33Probe.hpp"
#include "Navi33N48nClient.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>

namespace nm=navi33::metal;
static_assert(nm::kNotReady==(uint32_t)kIOReturnNotReady && nm::kBadArg==(uint32_t)kIOReturnBadArgument &&
              nm::kNotFound==(uint32_t)kIOReturnNotFound && nm::kUnsupported==(uint32_t)kIOReturnUnsupported &&
              nm::kExclusive==(uint32_t)kIOReturnExclusiveAccess && nm::kBusy==(uint32_t)kIOReturnBusy &&
              nm::kNoMemory==(uint32_t)kIOReturnNoMemory && nm::kOk==(uint32_t)kIOReturnSuccess,"IOReturn codes");
static_assert(N48_METAL_UC_N48N==N48N_UC_TYPE,"the aux kext's 'N48N' is the user client's");

OSDefineMetaClassAndStructors(Navi48MetalNub, IOService)

namespace {
IOLock *gLock;
Navi48MetalNub *gNub;
volatile uint32_t gState=nm::kOff;
Navi33Probe *gOwner;                 // the nub's provider (not retained: the nub dies with it)
struct Dev { uint32_t magic; IOBufferMemoryDescriptor *md; volatile uint32_t *va; void *accel; };
constexpr uint32_t kDevMagic=0x4D44564Eu;
Dev *gDev;
const char kAccelName[]="AMD Radeon RX 7600 (Navi33)";

void *op_device_open(void *accel,void *nub) {
    if(!nub || !accel || gDev)return nullptr;
    IOBufferMemoryDescriptor *md=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMemoryMapperNone,nm::kStampBytes,nm::kStampPhysMask);
    if(!md)return nullptr;
    if(md->prepare()!=kIOReturnSuccess){md->release();return nullptr;}
    IOByteCount seg=0;
    const addr64_t phys=md->getPhysicalSegment(0,&seg,kIOMemoryMapperNone);
    void *cpu=md->getBytesNoCopy();
    if(!cpu || !nm::stamp_page_ok(phys,seg,md->getLength())){md->complete();md->release();return nullptr;}
    bzero(cpu,nm::kStampBytes);
    Dev *d=static_cast<Dev *>(IOMalloc(sizeof(Dev)));
    if(!d){md->complete();md->release();return nullptr;}
    d->magic=kDevMagic;d->md=md;d->va=static_cast<volatile uint32_t *>(cpu);d->accel=accel;
    gDev=d;
    IOLog("Navi33Probe: metal device_open: stamp page phys %#llx\n",static_cast<unsigned long long>(phys));
    return d;
}
void op_device_close(void *ctx) {
    Dev *d=static_cast<Dev *>(ctx);
    if(!d || d!=gDev || d->magic!=kDevMagic)return;
    gDev=nullptr;d->magic=0;d->accel=nullptr;
    if(d->md){d->md->complete();d->md->release();d->md=nullptr;}
    IOFree(d,sizeof(Dev));
    IOLog("Navi33Probe: metal device_close\n");
}
int op_populate_config(uint8_t *cfg,uint32_t bytes) {
    const int rc=nm::config_populate(cfg,bytes,reinterpret_cast<uint64_t>(kAccelName));
    IOLog("Navi33Probe: metal populate_config %d\n",rc);
    return rc;
}
void *op_stamp_memory(void *ctx,uint32_t *n) {
    Dev *d=static_cast<Dev *>(ctx);
    if(n)*n=0;
    return d && d==gDev && d->magic==kDevMagic ? static_cast<void *>(d->md) : nullptr;
}
volatile uint32_t *op_stamp_va(void *ctx) {
    Dev *d=static_cast<Dev *>(ctx);
    return d && d==gDev && d->magic==kDevMagic ? d->va : nullptr;
}
int op_task_window(void *,uint32_t kind,uint64_t *size,uint64_t *reserve) { return nm::task_window(kind,size,reserve); }
uint32_t op_factory_mask(void *) {
    uint32_t v=0;
    return nm::factory_mask(PE_parse_boot_argn("navi33-metal-fact",&v,sizeof(v)),v);
}
int op_mm_hook(void *,uint32_t,void *) { return static_cast<int>(kIOReturnUnsupported); }
void op_trace(uint32_t event,uint64_t a,uint64_t b) {
    IOLog("Navi33Probe: metal aux trace %u %#llx %#llx\n",event,static_cast<unsigned long long>(a),static_cast<unsigned long long>(b));
}
nm::VhookCounts gVhCounts;
int op_vhook(void *,uint32_t cls,uint32_t slot,void *,const uint64_t *args,uint32_t nargs,uint64_t *ret) {
    const uint32_t n=nm::vhook_count(gVhCounts,cls,slot);
    const uint32_t mask=(cls==N48_VC_VIDMEMORY || cls==N48_VC_RESOURCE) ? op_factory_mask(nullptr) : 0u;
    const int h=nm::vhook_handle(mask,cls,slot,args,nargs,ret,nm::kKernelMin);
    if(nm::vhook_log_kind(n)!=nm::kVhLogNone)
        IOLog("Navi33Probe: metal vhook cls %u slot %u nargs %u call %u: %s\n",cls,slot,nargs,n,h ? "handled" : "default");
    return h;
}
// The accelerator's newUserClient hands its 'N48N' opens here: the same client RADV opens on
// Navi33Probe, attached to the accelerator, owned by Navi33Probe (all GPU state lives there).
int32_t op_native_open(void *ctx,void *accel,void *task,void *securityID,uint32_t type,void **handler) {
    if(!handler)return static_cast<int32_t>(kIOReturnBadArgument);
    Dev *d=static_cast<Dev *>(ctx);
    const bool live=d && d==__atomic_load_n(&gDev,__ATOMIC_ACQUIRE) && d->magic==kDevMagic;
    const uint32_t v=nm::native_open_verdict(type==N48_METAL_UC_N48N,live,live && accel==d->accel,gNub!=nullptr && gOwner!=nullptr);
    if(v!=nm::kOk)return static_cast<int32_t>(v);
    IOUserClient *uc=nullptr;
    const IOReturn rc=Navi33N48nClient::create(gOwner,static_cast<IOService *>(accel),static_cast<task_t>(task),securityID,type,&uc);
    if(rc==kIOReturnSuccess)*handler=uc;
    return static_cast<int32_t>(rc);
}
// Display off: flags 0, no display hook, no PCI getter (ABI 3, native_open last).
const N48MetalOps gOps={
    N48_METAL_OPS_MAGIC,N48_METAL_ABI,static_cast<uint32_t>(sizeof(N48MetalOps)),0u,nm::kOpsFlags,0u,
    op_device_open,op_device_close,op_populate_config,op_stamp_memory,op_stamp_va,op_task_window,
    op_factory_mask,op_mm_hook,op_trace,
    op_vhook,nm::kOpsCaps,0,
    0u,0u,nullptr,nullptr,
    op_native_open,
};
void ensureLock() {
    if(gLock)return;
    IOLock *l=IOLockAlloc();
    if(l && !OSCompareAndSwapPtr(nullptr,l,&gLock))IOLockFree(l);
}
} // namespace

uint32_t Navi48MetalNub::publish(Navi33Probe *owner,const nm::Evidence &e,uint64_t vramBytes) {
    if(!owner)return nm::kBadArg;
    ensureLock();
    if(!gLock)return nm::kNoMemory;
    IOLockLock(gLock);
    const uint32_t verdict=nm::publish_verdict(e,static_cast<nm::State>(gState));
    if(verdict!=nm::kOk){IOLockUnlock(gLock);return verdict;}
    Navi48MetalNub *nub=OSTypeAlloc(Navi48MetalNub);
    OSDictionary *props=OSDictionary::withCapacity(4);
    bool ok=nub && props && nub->init(props);
    if(props)props->release();
    if(ok) {
        nub->setProperty("model","AMD Radeon RX 7600");
        nub->setProperty("Navi48,MetalABI",static_cast<unsigned long long>(N48_METAL_ABI),32);
        nub->setProperty("Navi48,TaskVASize",static_cast<unsigned long long>(nm::kTaskVaSize),64);
        nub->setProperty("Navi48,VRAMBytes",static_cast<unsigned long long>(vramBytes),64);
        nub->setProperty("Navi48,Ready",static_cast<unsigned long long>(nm::kReadyYes),32);
        nub->setProperty("Navi33,Evidence","compute, gfx ring, TLB flush (hard rule 6)");
        ok=nub->attach(owner);
    }
    if(!ok){IOLockUnlock(gLock);if(nub)nub->release();return nm::kNoMemory;}
    gNub=nub;gOwner=owner;gState=nm::sm_after_publish(static_cast<nm::State>(gState));
    nub->retain();
    IOLockUnlock(gLock);
    IOLog("Navi33Probe: metal nub published (ops ABI %u): the aux accelerator kext may match it\n",N48_METAL_ABI);
    nub->registerService();
    nub->release();
    return nm::kOk;
}
bool Navi48MetalNub::published() { return gNub!=nullptr; }
void Navi48MetalNub::shutdown() {
    if(!gLock)return;
    IOLockLock(gLock);
    Navi48MetalNub *nub=gNub;
    if(nub){gNub=nullptr;gOwner=nullptr;gState=nm::sm_after_withdraw(static_cast<nm::State>(gState));}
    IOLockUnlock(gLock);
    if(nub){nub->terminate();nub->release();}
}
IOReturn Navi48MetalNub::callPlatformFunction(const OSSymbol *functionName,bool waitForFunction,void *param1,void *param2,
                                              void *param3,void *param4) {
    if(functionName && functionName->isEqualTo(N48_METAL_FN_SYMBOL)) {
        if(!param1)return kIOReturnBadArgument;
        *static_cast<const N48MetalOps **>(param1)=&gOps;
        return kIOReturnSuccess;
    }
    return super::callPlatformFunction(functionName,waitForFunction,param1,param2,param3,param4);
}
void Navi48MetalNub::free() {
    if(gLock){IOLockLock(gLock);gState=nm::sm_after_free(static_cast<nm::State>(gState));IOLockUnlock(gLock);}
    super::free();
}
