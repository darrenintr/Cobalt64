// SPDX-License-Identifier: MIT
// Host-testable, no-I/O validation of the N33N selector shapes.
// BO/VM/CTX/SUBMIT/WAIT answer only behind navi33-client-gpu=1 (the kext checks the gate). Owned DMA staging is opt-in.
// PSP_LOAD's write is gated in the Kext by boot-args, not by its shape.
#pragma once
#include "Navi33NativeABI.h"
#include <stdint.h>
namespace navi33 { namespace native_policy {
enum class Access : uint8_t { Deny, Allow };
enum class Shape : uint8_t { Valid, BadArgument, Unsupported };
inline Access openAllowed(uint32_t type, bool probe, bool enabled,
                          bool off, bool admin) {
    return (type == N33_UC_TYPE && probe && enabled && !off && admin)
        ? Access::Allow : Access::Deny;
}
inline Shape validate(uint32_t selector, uint32_t scalarIn,
                      uint32_t scalarOut, uint32_t structIn,
                      uint32_t structOut, bool haveScalarsIn,
                      bool haveScalarsOut, bool haveStructOut,
                      bool haveDescriptors) {
    if (haveDescriptors) return Shape::BadArgument;
    // Explicitly refuse the future submit/memory selectors, even if the
    // requester attempts to pass a shape resembling a supported method.
    if (selector != N33_SEL_HELLO && selector != N33_SEL_QUERY_CAPS &&
        selector != N33_SEL_PSP_STATUS && selector != N33_SEL_SDMA_STATUS &&
        selector != N33_SEL_DMA_STAGE && selector != N33_SEL_DMA_STATUS &&
        selector != N33_SEL_DMA_RELEASE &&
        selector != N33_SEL_HW_PREFLIGHT &&
        selector != N33_SEL_HW_ROM_PREFLIGHT &&
        selector != N33_SEL_VRAM_GEOMETRY &&
        selector != N33_SEL_QUEUE_CENSUS &&
        selector != N33_SEL_MMIO_PEEK &&
        selector != N33_SEL_PSP_LOAD &&
        (selector < N33_SEL_BO_CREATE || selector > N33_SEL_WAIT))
        return Shape::Unsupported;
    // Client GPU selectors: scalars only, exact counts (SUBMIT: 1 + 2n, n = 1..4).
    if (selector >= N33_SEL_BO_CREATE && selector <= N33_SEL_WAIT) {
        const uint32_t in = selector == N33_SEL_BO_CREATE ? 2u : selector == N33_SEL_BO_FREE ? 1u :
            selector == N33_SEL_VM_MAP ? 4u : selector == N33_SEL_CTX_CREATE ? 1u :
            selector == N33_SEL_WAIT ? 2u : 0u;
        const uint32_t out = selector == N33_SEL_BO_CREATE ? 2u : selector == N33_SEL_CTX_CREATE ? 1u :
            selector == N33_SEL_SUBMIT ? 1u : selector == N33_SEL_WAIT ? 2u : 0u;
        const bool inOk = selector == N33_SEL_SUBMIT
            ? (scalarIn >= 3u && scalarIn <= 1u + 2u * N33_SUBMIT_MAX_IBS && (scalarIn & 1u))
            : scalarIn == in;
        // IOKit passes a non-null inband struct-output buffer even to scalar-only calls
        // (as it does scalar arrays, see 0.0.3), so only the sizes count.
        (void)haveStructOut;
        return inOk && scalarOut == out && !structIn && !structOut &&
               haveScalarsIn && haveScalarsOut == (out != 0u) ? Shape::Valid : Shape::BadArgument;
    }
    if (selector == N33_SEL_HELLO)
        return scalarIn == 2u && scalarOut == 3u && !structIn &&
               !structOut && haveScalarsIn && haveScalarsOut
                   ? Shape::Valid : Shape::BadArgument;
    if (selector == N33_SEL_HW_ROM_PREFLIGHT)
        return scalarIn==0u && scalarOut==0u &&
               structIn>=64u && structIn<=4u*1024u*1024u &&
               structOut==sizeof(n33_hw_preflight) &&
               !haveScalarsIn && !haveScalarsOut && haveStructOut
                   ? Shape::Valid : Shape::BadArgument;
    // Request header plus an IP Discovery blob, inline only (<= 4 KiB).
    if (selector == N33_SEL_MMIO_PEEK)
        return scalarIn==0u && scalarOut==0u &&
               structIn>=sizeof(n33_peek_request)+64u &&
               structIn<=4096u && structOut==sizeof(n33_peek_result) &&
               !haveScalarsIn && !haveScalarsOut && haveStructOut
                   ? Shape::Valid : Shape::BadArgument;
    // Request header, IP Discovery, then the sOS container (out of line).
    if (selector == N33_SEL_PSP_LOAD)
        return scalarIn==0u && scalarOut==0u &&
               structIn>=sizeof(n33_psp_load_request)+64u+1u &&
               structIn<=N33_PSP_LOAD_MAX_INPUT &&
               structOut==sizeof(n33_psp_load_result) &&
               !haveScalarsIn && !haveScalarsOut && haveStructOut
                   ? Shape::Valid : Shape::BadArgument;
    if (selector == N33_SEL_DMA_RELEASE)
        return scalarIn==0u && scalarOut==0u && structIn==0u &&
               structOut==0u && !haveStructOut && !haveScalarsIn &&
               !haveScalarsOut ? Shape::Valid : Shape::BadArgument;
    const uint32_t expected = selector == N33_SEL_QUERY_CAPS
        ? sizeof(n33_capabilities) :
        selector == N33_SEL_PSP_STATUS ? sizeof(n33_psp_status) :
        selector == N33_SEL_SDMA_STATUS ? sizeof(n33_sdma_status) :
        selector == N33_SEL_HW_PREFLIGHT ? sizeof(n33_hw_preflight) :
        selector == N33_SEL_VRAM_GEOMETRY ? sizeof(n33_vram_geometry_status) :
        selector == N33_SEL_QUEUE_CENSUS ? sizeof(n33_queue_census_status) :
        sizeof(n33_dma_stage_status);
    return scalarIn == 0u && scalarOut == 0u && structIn == 0u &&
           structOut == expected && haveStructOut &&
           !haveScalarsIn && !haveScalarsOut
               ? Shape::Valid : Shape::BadArgument;
}
} } // namespace navi33::native_policy
