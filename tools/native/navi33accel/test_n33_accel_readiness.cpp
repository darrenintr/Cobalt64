// SPDX-License-Identifier: MIT
#include "n33_accel_readiness.h"

#include <cstdio>
#include <cstring>

namespace {
unsigned failures = 0;
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static n33_capabilities readyCaps() {
    n33_capabilities caps{};
    caps.abi_version = N33_ABI_VERSION;
    caps.size = sizeof(caps);
    caps.vendor_id = N33_PCI_VENDOR;
    caps.device_id = N33_PCI_DEVICE;
    caps.gfx_ip_version = N33_GFX_IP_VERSION;
    caps.family = N33_FAMILY_GFX1102;
    caps.capabilities = N33_CAP_PCI_INVENTORY | N33_ACCEL_REQUIRED_CAPS;
    caps.vram_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    caps.gtt_bytes = 16ull * 1024ull * 1024ull * 1024ull;
    return caps;
}

static n33_accel_backend_evidence readyEvidence() {
    n33_accel_backend_evidence evidence{};
    evidence.abi_version = N33_ACCEL_BACKEND_ABI;
    evidence.size = sizeof(evidence);
    evidence.evidence_bits = N33_ACCEL_REQUIRED_EVIDENCE;
    evidence.device_owner_epoch = 1u;
    evidence.vm_root_epoch = 1u;
    evidence.gmc_context_epoch = 1u;
    evidence.sdma_queue_epoch = 1u;
    evidence.gfx_queue_epoch = 1u;
    evidence.recovery_epoch = 1u;
    evidence.client_transport_epoch = 1u;
    return evidence;
}
} // namespace

int main() {
    auto caps = readyCaps();
    auto evidence = readyEvidence();
    CHECK(n33_accel_backend_ready(&caps, &evidence));
    CHECK(!n33_accel_backend_ready(nullptr, &evidence));
    CHECK(!n33_accel_backend_ready(&caps, nullptr));

    // Current Navi33Probe state: inventory only, no GPU/Metal capability.
    auto current = caps;
    current.capabilities = N33_CAP_PCI_INVENTORY;
    current.vram_bytes = 0u;
    current.gtt_bytes = 0u;
    CHECK(!n33_accel_backend_ready(&current, &evidence));

    const uint32_t capsToRemove[] = {
        N33_CAP_HW_VM, N33_CAP_HW_SDMA, N33_CAP_HW_GFX, N33_CAP_METAL
    };
    for (uint32_t bit : capsToRemove) {
        auto missing = caps;
        missing.capabilities &= ~bit;
        CHECK(!n33_accel_backend_ready(&missing, &evidence));
    }

    for (unsigned bit = 0; bit < 21; ++bit) {
        auto missing = evidence;
        missing.evidence_bits &= ~(1u << bit);
        CHECK(!n33_accel_backend_ready(&caps, &missing));
    }

    for (unsigned i = 0; i < 7; ++i) {
        auto stale = evidence;
        uint64_t *epochs[] = {
            &stale.device_owner_epoch, &stale.vm_root_epoch,
            &stale.gmc_context_epoch, &stale.sdma_queue_epoch,
            &stale.gfx_queue_epoch, &stale.recovery_epoch,
            &stale.client_transport_epoch
        };
        *epochs[i] = 0u;
        CHECK(!n33_accel_backend_ready(&caps, &stale));
    }

    // A resource from another device-owner generation is stale even if it is
    // individually nonzero; do not accept mixed ownership epochs.
    for (unsigned i = 1; i < 7; ++i) {
        auto stale = evidence;
        uint64_t *epochs[] = {
            &stale.device_owner_epoch, &stale.vm_root_epoch,
            &stale.gmc_context_epoch, &stale.sdma_queue_epoch,
            &stale.gfx_queue_epoch, &stale.recovery_epoch,
            &stale.client_transport_epoch
        };
        *epochs[i] = stale.device_owner_epoch + 1u;
        CHECK(!n33_accel_backend_ready(&caps, &stale));
    }

    auto bad = evidence;
    bad.abi_version++;
    CHECK(!n33_accel_backend_ready(&caps, &bad));
    bad = evidence; bad.size++;
    CHECK(!n33_accel_backend_ready(&caps, &bad));
    bad = evidence; bad.reserved = 1u;
    CHECK(!n33_accel_backend_ready(&caps, &bad));
    bad = evidence; bad.evidence_bits |= 1u << 31;
    CHECK(!n33_accel_backend_ready(&caps, &bad));

    bad = evidence; caps.reserved0 = 1u;
    CHECK(!n33_accel_backend_ready(&caps, &bad));
    caps = readyCaps();
    caps.reserved1 = 1u;
    CHECK(!n33_accel_backend_ready(&caps, &bad));

    if (failures) return 1;
    std::puts("PASS Navi33 accelerator readiness is capability-, evidence-, and epoch-gated");
    return 0;
}
