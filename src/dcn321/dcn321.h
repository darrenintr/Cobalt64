/* SPDX-License-Identifier: MIT */
#ifndef COBALT_DCN321_H
#define COBALT_DCN321_H
#include <stdint.h>
#include <stdbool.h>
#include "dcn321_regs.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { DCN321_OK=0, DCN321_ARG=-1, DCN321_RANGE=-2, DCN321_DISABLED=-3,
       DCN321_PRECONDITION=-4, DCN321_IO=-5, DCN321_TIMEOUT=-6 };
struct dcn321_io {
    void *cookie;
    /* MMIO dword addresses, not byte addresses. Must be mapped by caller. */
    int (*read)(void *, uint32_t, uint32_t *);
    int (*write)(void *, uint32_t, uint32_t);
    void (*delay_us)(void *, uint32_t);
};
struct dcn321_device {
    struct dcn321_io io;
    uint32_t segment2;       /* DCN IP-discovery BASE_IDX=2, in DWORDS */
    uint32_t bar_dwords;
    uint64_t vram_base, vram_limit; /* reserved scanout MC address range [base,limit) */
    uint32_t pci_device;     /* 0x7480/0x7483: Navi33 only */
    bool armed;              /* explicit second gate, default false */
};
struct dcn321_mode {
    uint16_t width,height;
    uint16_t h_front,h_sync,h_back;
    uint16_t v_front,v_sync,v_back;
    bool h_positive,v_positive;
    bool displayport;
    uint32_t pixel_clock_khz;
};
struct dcn321_scanout {
    uint64_t address; /* MC address; must have been allocated and pinned by caller */
    uint32_t pitch_pixels;
    uint32_t height;
};
/* Read-only probes are allowed unarmed. Register writes require explicit armed=true. */
int dcn321_init(struct dcn321_device *, const struct dcn321_io *, uint32_t,
                uint32_t, uint32_t, uint64_t, uint64_t);
int dcn321_read_frame(struct dcn321_device *, unsigned, uint32_t *);
int dcn321_validate_mode(const struct dcn321_mode *);
/* Timing-only: require caller to have powered DCN, set pixel clocks, trained link,
 * configured MPC/OPP/DPP/HubP, and blanked the pipe. Does not enable OTG. */
int dcn321_program_otg_timing(struct dcn321_device *, unsigned, const struct dcn321_mode *);
/* Atomic primary-address update for a fully configured and already active HUBP.
 * Does not configure tiling, pitch, pixel format, VM or scanout routing. */
int dcn321_flip_primary(struct dcn321_device *, unsigned, const struct dcn321_scanout *);
#ifdef __cplusplus
}
#endif
#endif
