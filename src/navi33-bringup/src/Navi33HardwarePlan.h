// Navi33HardwarePlan - CPU-only, fail-closed hardware-generation gate.
// Data source: Linux amdgpu_discovery.c set_{gmc,psp,smu,sdma,display,gc}_ip_blocks.
// This module contains NO I/O methods, pointer to MMIO, firmware loader,
// command queue, or write API. A successful metadata plan != working GPU.
#pragma once
#include "Navi33Discovery.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 {

enum class Engine : uint8_t { GFX11, PSP13, SMU13, SDMA6, DCN321, Count };

struct EngineSpec {
    const char *name;
    uint16_t hwid;
    uint8_t major, minor, revision;
    uint8_t instance;
};

constexpr EngineSpec kNavi33Engines[] = {
    {"GFX11",   ipid::GC,    11u, 0u, 2u, 0u},
    {"PSP13",   ipid::MP0,   13u, 0u, 7u, 0u},
    {"SMU13",   ipid::MP1,   13u, 0u, 7u, 0u},
    {"SDMA6",   ipid::SDMA0,  6u, 0u, 2u, 0u},
    {"DCN321",  ipid::DMU,    3u, 2u, 1u, 0u}
};

constexpr size_t kNavi33EngineCount =
    sizeof(kNavi33Engines) / sizeof(kNavi33Engines[0]);
static_assert(kNavi33EngineCount == static_cast<size_t>(Engine::Count),
              "Navi33 engine contract is incomplete");
static_assert(kNavi33Engines[0].major == 11u &&
              kNavi33Engines[1].major == 13u &&
              kNavi33Engines[4].major == 3u,
              "Never reuse RDNA4 GC12/PSP14/DCN4 constants");

enum class InventoryCode : uint8_t {
    Pass = 0, NoDiscovery, MissingOrDuplicateEngine,
    UnsupportedEngineVersion, MissingEngineBases
};

struct InventoryReport {
    InventoryCode code = InventoryCode::NoDiscovery;
    Engine failingEngine = Engine::GFX11;
    uint8_t matchedEngines = 0;
};

inline InventoryReport inventory(const IPDiscovery &discovery) {
    if (!discovery.valid())
        return {InventoryCode::NoDiscovery, Engine::GFX11, 0u};
    for (size_t i = 0; i < kNavi33EngineCount; ++i) {
        const EngineSpec &spec = kNavi33Engines[i];
        const Engine engine = static_cast<Engine>(i);
        IPEntry entry{};
        // IPDiscovery::find() also rejects duplicate IDs/instances.
        if (!discovery.find(spec.hwid, spec.instance, entry))
            return {InventoryCode::MissingOrDuplicateEngine, engine,
                    static_cast<uint8_t>(i)};
        if (entry.major != spec.major || entry.minor != spec.minor ||
            entry.revision != spec.revision)
            return {InventoryCode::UnsupportedEngineVersion, engine,
                    static_cast<uint8_t>(i)};
        if (entry.numBases == 0u)
            return {InventoryCode::MissingEngineBases, engine,
                    static_cast<uint8_t>(i)};
    }
    return {InventoryCode::Pass, Engine::Count,
            static_cast<uint8_t>(kNavi33EngineCount)};
}

enum class AddressCode : uint8_t {
    Pass = 0, InventoryRejected, NoBar5, MissingEngine,
    MissingSegment, UnmappedSegment, OutsideBar5
};

struct RegisterAddress {
    AddressCode code = AddressCode::InventoryRejected;
    uint64_t bar5ByteOffset = 0;
};

inline RegisterAddress resolveDword(const IPDiscovery &discovery,
                                   Engine engine,
                                   uint8_t segment, uint32_t registerDword,
                                   uint64_t assignedBar5Bytes) {
    // This function produces an OFFSET ONLY. It intentionally does not
    // return a mapped pointer, address of PCI physical memory, or a read API.
    if (inventory(discovery).code != InventoryCode::Pass)
        return {AddressCode::InventoryRejected, 0u};
    if (assignedBar5Bytes < 4u)
        return {AddressCode::NoBar5, 0u};
    const size_t idx = static_cast<size_t>(engine);
    if (idx >= kNavi33EngineCount)
        return {AddressCode::MissingEngine, 0u};
    const EngineSpec &spec = kNavi33Engines[idx];
    IPEntry entry{};
    if (!discovery.find(spec.hwid, spec.instance, entry))
        return {AddressCode::MissingEngine, 0u};
    if (segment >= entry.numBases)
        return {AddressCode::MissingSegment, 0u};

    // BAR5 is the PCI MMIO aperture (registers), separate from BAR0 VRAM.
    // Discovery bases are DWORD offsets and *may* be 64-bit in IPDS v4.
    // Reject 0/sentinel bases and any offset outside the assigned resource.
    const uint64_t base = entry.bases[segment];
    if (base == 0u || base == UINT64_MAX || base == 0xffffffffu)
        return {AddressCode::UnmappedSegment, 0u};
    const uint64_t lastDword = (assignedBar5Bytes - 4u) / 4u;
    if (base > lastDword || registerDword > lastDword - base)
        return {AddressCode::OutsideBar5, 0u};
    return {AddressCode::Pass, (base + registerDword) * 4u};
}

// GFX/PSP/SMU/SDMA firmware-init/command-submission paths are intentionally
// blocked in this build. This is not a hidden boot argument or an override.
constexpr bool kFirmwareLoadingImplemented = false;
constexpr bool kGpuCommandSubmissionImplemented = false;
constexpr bool kRegisterWritesImplemented = false;
static_assert(!kFirmwareLoadingImplemented &&
              !kGpuCommandSubmissionImplemented &&
              !kRegisterWritesImplemented,
              "This stage must never write a single-GPU's registers");

} // namespace navi33
