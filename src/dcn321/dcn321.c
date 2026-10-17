/* SPDX-License-Identifier: MIT */
#include "dcn321.h"
#include <stddef.h>
static int ready(const struct dcn321_device *d) {
    return d && d->io.read && d->io.write && d->bar_dwords &&
        (d->pci_device == 0x7480u || d->pci_device == 0x7483u);
}
static int address(const struct dcn321_device *d,const uint32_t r[4],unsigned pipe,uint32_t *out) {
    uint64_t abs;
    if (!ready(d) || !out || pipe >= DCN321_PIPE_COUNT) return DCN321_ARG;
    abs=(uint64_t)d->segment2+r[pipe];
    if (abs >= d->bar_dwords) return DCN321_RANGE;
    *out=(uint32_t)abs;
    return DCN321_OK;
}
static int rd(struct dcn321_device *d,const uint32_t r[4],unsigned p,uint32_t *v) {
    uint32_t a;
    int rc=address(d,r,p,&a);
    return rc ? rc : (d->io.read(d->io.cookie,a,v) ? DCN321_IO : DCN321_OK);
}
static int wr(struct dcn321_device *d,const uint32_t r[4],unsigned p,uint32_t v) {
    uint32_t a;
    int rc=address(d,r,p,&a);
    if (rc) return rc;
    if (!d->armed) return DCN321_DISABLED;
    return d->io.write(d->io.cookie,a,v) ? DCN321_IO : DCN321_OK;
}
static int field(struct dcn321_device *d,const uint32_t r[4],unsigned p,uint32_t mask,uint32_t value) {
    uint32_t prev;
    int rc=rd(d,r,p,&prev);
    if (rc) return rc;
    return wr(d,r,p,(prev & ~mask)|(value & mask));
}
int dcn321_init(struct dcn321_device *d,const struct dcn321_io *io,uint32_t pci,
                uint32_t seg2,uint32_t bar,uint64_t lo,uint64_t hi) {
    if (!d || !io || !io->read || !io->write || !bar || !seg2 ||
        (pci!=0x7480u && pci!=0x7483u) || !lo || hi<=lo || hi>(1ull<<48) ||
        (uint64_t)seg2+0x2400u>=bar) return DCN321_ARG;
    d->io=*io;d->pci_device=pci;d->segment2=seg2;d->bar_dwords=bar;
    d->vram_base=lo;d->vram_limit=hi;d->armed=false;
    return DCN321_OK;
}
int dcn321_validate_mode(const struct dcn321_mode *m) {
    uint32_t ht,vt;
    if (!m || !m->width || !m->height || !m->h_sync || !m->v_sync ||
        !m->h_front || !m->v_front || !m->pixel_clock_khz) return DCN321_ARG;
    ht=(uint32_t)m->width+m->h_front+m->h_sync+m->h_back;
    vt=(uint32_t)m->height+m->v_front+m->v_sync+m->v_back;
    if (ht>0x7fffu || vt>0x7fffu || m->h_sync>0x7fffu || m->v_sync>0x7fffu ||
        m->pixel_clock_khz>1200000u) return DCN321_RANGE;
    return DCN321_OK;
}
int dcn321_read_frame(struct dcn321_device *d,unsigned p,uint32_t *count) {
    int rc;
    if (!count) return DCN321_ARG;
    rc=rd(d,dcn321_r_otg_otg_status_frame_count,p,count);
    if (!rc) *count&=DCN321_F_OTG_OTG_STATUS_FRAME_COUNT_OTG_FRAME_COUNT_MASK;
    return rc;
}
#define WRITE(reg,val) do { rc=wr(d,dcn321_r_otg_##reg,p,(val));if(rc)return rc; }while(0)
#define FIELD(reg,mask,val) do { rc=field(d,dcn321_r_otg_##reg,p,(mask),(val));if(rc)return rc; }while(0)
int dcn321_program_otg_timing(struct dcn321_device *d,unsigned p,const struct dcn321_mode *m) {
    uint32_t ht,vt,hs,he,vs,ve,ctl;
    int rc=dcn321_validate_mode(m);
    if (rc) return rc;
    if (!ready(d) || p>=4) return DCN321_ARG;
    if (!d->armed) return DCN321_DISABLED;
    /* Never reprogram a running pipe: that can blank a single-GPU machine. */
    rc=rd(d,dcn321_r_otg_otg_control,p,&ctl);
    if (rc) return rc;
    if (ctl & DCN321_F_OTG_OTG_CONTROL_OTG_MASTER_EN_MASK) return DCN321_PRECONDITION;
    ht=(uint32_t)m->width+m->h_front+m->h_sync+m->h_back;
    vt=(uint32_t)m->height+m->v_front+m->v_sync+m->v_back;
    hs=ht-m->h_front; he=hs-m->width;
    vs=vt-m->v_front; ve=vs-m->height;
    WRITE(otg_h_total,ht-1u);
    WRITE(otg_h_sync_a,(uint32_t)m->h_sync<<16);
    WRITE(otg_h_blank_start_end,(he<<16)|hs);
    FIELD(otg_h_sync_a_cntl,DCN321_F_OTG_OTG_H_SYNC_A_CNTL_OTG_H_SYNC_A_POL_MASK,m->h_positive?0u:1u);
    WRITE(otg_v_total,vt-1u);
    WRITE(otg_v_total_min,vt-1u);
    WRITE(otg_v_total_max,vt-1u);
    WRITE(otg_v_sync_a,(uint32_t)m->v_sync<<16);
    WRITE(otg_v_blank_start_end,(ve<<16)|vs);
    FIELD(otg_v_sync_a_cntl,DCN321_F_OTG_OTG_V_SYNC_A_CNTL_OTG_V_SYNC_A_POL_MASK,m->v_positive?0u:1u);
    FIELD(otg_h_timing_cntl,DCN321_F_OTG_OTG_H_TIMING_CNTL_OTG_H_TIMING_DIV_MODE_MASK,0u);
    FIELD(otg_control,DCN321_F_OTG_OTG_CONTROL_OTG_START_POINT_CNTL_MASK,m->displayport?0x1000u:0u);
    return DCN321_OK;
}
#undef WRITE
#undef FIELD
int dcn321_flip_primary(struct dcn321_device *d,unsigned p,const struct dcn321_scanout *s) {
    uint64_t size;
    uint32_t a,ctl;
    int rc;
    if (!ready(d) || !s || p>=4) return DCN321_ARG;
    if (!d->armed) return DCN321_DISABLED;
    if (!s->pitch_pixels || !s->height || s->pitch_pixels>16384u ||
        s->address==0 || (s->address&255u)) return DCN321_ARG;
    size=(uint64_t)s->pitch_pixels*(uint64_t)s->height*4u;
    if (s->address<d->vram_base || s->address>=d->vram_limit ||
        size>d->vram_limit-s->address) return DCN321_RANGE;
    /* Fail closed unless display timing already runs; all other pipe resources are caller-owned. */
    rc=rd(d,dcn321_r_otg_otg_control,p,&ctl);
    if(rc)return rc;
    if (!(ctl&DCN321_F_OTG_OTG_CONTROL_OTG_MASTER_EN_MASK))return DCN321_PRECONDITION;
    rc=rd(d,dcn321_r_hubpreq_dcsurf_flip_control,p,&ctl);
    if(rc)return rc;
    if (ctl&DCN321_F_HUBPREQ_DCSURF_FLIP_CONTROL_SURFACE_UPDATE_LOCK_MASK)
        return DCN321_PRECONDITION;
    rc=address(d,dcn321_r_hubpreq_dcsurf_primary_surface_address_high,p,&a);
    if(rc)return rc;
    if(d->io.write(d->io.cookie,a,(uint32_t)(s->address>>32)))return DCN321_IO;
    rc=address(d,dcn321_r_hubpreq_dcsurf_primary_surface_address,p,&a);
    if(rc)return rc;
    /* AMD DC requires high DWORD first, then low DWORD to latch. */
    return d->io.write(d->io.cookie,a,(uint32_t)s->address)?DCN321_IO:DCN321_OK;
}


/* Read-only pipe inventory.  Caller must ensure BAR/ASIC lifetime and MMIO
 * serialization.  Values describe internal display state, not photons. */
int dcn321_snapshot_pipe(struct dcn321_device *d, unsigned p,
                         struct dcn321_pipe_state *out) {
    struct dcn321_pipe_state s = {0u, 0u, 0u, 0u, false, false, false, false, false};
    uint32_t ctl, clock, flip, lock, surf, pitch, lo, hi;
    int rc;
    if (!out || !ready(d) || p >= DCN321_PIPE_COUNT) return DCN321_ARG;
#define SNAP(reg, val) do { \
        rc = rd(d, dcn321_r_##reg, p, &(val)); if (rc) return rc; \
    } while (0)
    SNAP(otg_otg_control, ctl);
    SNAP(otg_otg_clock_control, clock);
    SNAP(otg_otg_master_update_lock, lock);
    SNAP(otg_otg_status_frame_count, s.frame_counter);
    SNAP(hubpreq_dcsurf_flip_control, flip);
    SNAP(hubpreq_dcsurf_surface_control, surf);
    SNAP(hubp_dcsurf_surface_config, s.surface_format);
    SNAP(hubpreq_dcsurf_surface_pitch, pitch);
    SNAP(hubpreq_dcsurf_primary_surface_address_high, hi);
    SNAP(hubpreq_dcsurf_primary_surface_address, lo);
#undef SNAP
    s.otg_enabled = (ctl & DCN321_F_OTG_OTG_CONTROL_OTG_MASTER_EN_MASK) != 0;
    s.clock_on = (clock & DCN321_F_OTG_OTG_CLOCK_CONTROL_OTG_CLOCK_ON_MASK) != 0;
    s.update_locked = (lock &
        (DCN321_F_OTG_OTG_MASTER_UPDATE_LOCK_OTG_MASTER_UPDATE_LOCK_MASK |
         DCN321_F_OTG_OTG_MASTER_UPDATE_LOCK_UPDATE_LOCK_STATUS_MASK)) != 0;
    s.flip_pending = (flip &
        DCN321_F_HUBPREQ_DCSURF_FLIP_CONTROL_SURFACE_FLIP_PENDING_MASK) != 0;
    s.dcc_enabled = (surf &
        DCN321_F_HUBPREQ_DCSURF_SURFACE_CONTROL_PRIMARY_SURFACE_DCC_EN_MASK) != 0;
    s.frame_counter &= DCN321_F_OTG_OTG_STATUS_FRAME_COUNT_OTG_FRAME_COUNT_MASK;
    s.surface_format &= DCN321_F_HUBP_DCSURF_SURFACE_CONFIG_SURFACE_PIXEL_FORMAT_MASK;
    s.pitch_pixels = (pitch & DCN321_F_HUBPREQ_DCSURF_SURFACE_PITCH_PITCH_MASK) + 1u;
    s.gpu_address = ((uint64_t)hi << 32) | lo;
    *out = s;
    return DCN321_OK;
}

int dcn321_wait_frame_progress(struct dcn321_device *d, unsigned p,
                               unsigned samples, uint32_t interval_us) {
    struct dcn321_pipe_state s;
    uint32_t count;
    unsigned i;
    int rc;
    if (!ready(d) || !d->io.delay_us || p >= DCN321_PIPE_COUNT ||
        !samples || samples > 1000u || !interval_us || interval_us > 1000000u)
        return DCN321_ARG;
    rc = dcn321_snapshot_pipe(d, p, &s);
    if (rc) return rc;
    if (!s.otg_enabled || !s.clock_on) return DCN321_PRECONDITION;
    for (i = 0; i < samples; ++i) {
        d->io.delay_us(d->io.cookie, interval_us);
        rc = dcn321_read_frame(d, p, &count);
        if (rc) return rc;
        /* Frame counter is 24 bits; modulo subtraction handles wraparound. */
        if (((count - s.frame_counter) & 0x00ffffffu) != 0u)
            return DCN321_OK;
    }
    return DCN321_TIMEOUT;
}

struct dcn321_edit {
    const uint32_t *registers;
    uint32_t mask, value;
    uint32_t mmio_dword, previous, replacement;
};

/* Stage only a linear, non-DCC, full-viewport 32bpp plane on a completely
 * disabled OTG.  Does not set any pixel clocks, DML watermarks, MPC/OPP,
 * DPP, VM translation, link encoder or OTG_MASTER_EN.  Caller owns the VRAM
 * reservation, firmware handoff, concurrency and rollback of other blocks. */
int dcn321_prepare_linear_plane(struct dcn321_device *d, unsigned p,
                                const struct dcn321_mode *m,
                                const struct dcn321_scanout *s,
                                enum dcn321_pixel_order order) {
    struct dcn321_edit edits[10];
    uint32_t ctl, clock, lock, flip;
    uint64_t length;
    size_t i;
    int rc, restore_failed = 0;
    uint32_t crossbar;
    const uint32_t xbar_mask =
        DCN321_F_HUBPRET_HUBPRET_CONTROL_CROSSBAR_SRC_CB_B_MASK |
        DCN321_F_HUBPRET_HUBPRET_CONTROL_CROSSBAR_SRC_CR_R_MASK;

    if (!ready(d) || p >= DCN321_PIPE_COUNT || !m || !s ||
        (order != DCN321_ARGB8888 && order != DCN321_ABGR8888))
        return DCN321_ARG;
    rc = dcn321_validate_mode(m);
    if (rc) return rc;
    if (!s->address || (s->address & 255u) || !s->pitch_pixels ||
        s->pitch_pixels > 16384u || m->width > 16383u ||
        m->height > 16383u || s->pitch_pixels < m->width ||
        s->height < m->height || s->height > 16383u)
        return DCN321_ARG;
    length = (uint64_t)s->pitch_pixels * (uint64_t)s->height * 4u;
    if (s->address < d->vram_base || s->address >= d->vram_limit ||
        length > d->vram_limit - s->address)
        return DCN321_RANGE;
    if (!d->armed) return DCN321_DISABLED;

    rc = rd(d, dcn321_r_otg_otg_control, p, &ctl);
    if (rc) return rc;
    rc = rd(d, dcn321_r_otg_otg_clock_control, p, &clock);
    if (rc) return rc;
    rc = rd(d, dcn321_r_otg_otg_master_update_lock, p, &lock);
    if (rc) return rc;
    rc = rd(d, dcn321_r_hubpreq_dcsurf_flip_control, p, &flip);
    if (rc) return rc;
    if ((ctl & DCN321_F_OTG_OTG_CONTROL_OTG_MASTER_EN_MASK) ||
        !(clock & DCN321_F_OTG_OTG_CLOCK_CONTROL_OTG_CLOCK_ON_MASK) ||
        (lock & (DCN321_F_OTG_OTG_MASTER_UPDATE_LOCK_OTG_MASTER_UPDATE_LOCK_MASK |
                 DCN321_F_OTG_OTG_MASTER_UPDATE_LOCK_UPDATE_LOCK_STATUS_MASK)) ||
        (flip & (DCN321_F_HUBPREQ_DCSURF_FLIP_CONTROL_SURFACE_UPDATE_LOCK_MASK |
                 DCN321_F_HUBPREQ_DCSURF_FLIP_CONTROL_SURFACE_FLIP_PENDING_MASK)))
        return DCN321_PRECONDITION;

    /* Linux dcn20/dcn32 hubp uses pixel format 8 for ARGB/ABGR8888,
     * PITCH=surface_pitch-1, zero META_PITCH with DCC disabled. */
    crossbar = order == DCN321_ARGB8888 ?
        ((2u << DCN321_F_HUBPRET_HUBPRET_CONTROL_CROSSBAR_SRC_CB_B_SHIFT) |
         (3u << DCN321_F_HUBPRET_HUBPRET_CONTROL_CROSSBAR_SRC_CR_R_SHIFT)) :
        ((3u << DCN321_F_HUBPRET_HUBPRET_CONTROL_CROSSBAR_SRC_CB_B_SHIFT) |
         (2u << DCN321_F_HUBPRET_HUBPRET_CONTROL_CROSSBAR_SRC_CR_R_SHIFT));

#define STEP(i, reg, bits, data) do { \
        edits[i].registers = dcn321_r_##reg; \
        edits[i].mask = (bits); edits[i].value = (data); \
    } while (0)
    STEP(0, hubpreq_dcsurf_surface_control,
         DCN321_F_HUBPREQ_DCSURF_SURFACE_CONTROL_PRIMARY_SURFACE_DCC_EN_MASK |
         DCN321_F_HUBPREQ_DCSURF_SURFACE_CONTROL_PRIMARY_SURFACE_TMZ_MASK, 0u);
    STEP(1, hubp_dcsurf_tiling_config,
         DCN321_F_HUBP_DCSURF_TILING_CONFIG_SW_MODE_MASK, 0u);
    STEP(2, hubp_dcsurf_surface_config,
         DCN321_F_HUBP_DCSURF_SURFACE_CONFIG_SURFACE_PIXEL_FORMAT_MASK |
         DCN321_F_HUBP_DCSURF_SURFACE_CONFIG_ROTATION_ANGLE_MASK |
         DCN321_F_HUBP_DCSURF_SURFACE_CONFIG_H_MIRROR_EN_MASK, 8u);
    STEP(3, hubpret_hubpret_control, xbar_mask, crossbar);
    STEP(4, hubpreq_vmid_settings_0,
         DCN321_F_HUBPREQ_VMID_SETTINGS_0_VMID_MASK, 0u);
    STEP(5, hubpreq_dcsurf_surface_pitch,
         DCN321_F_HUBPREQ_DCSURF_SURFACE_PITCH_PITCH_MASK |
         DCN321_F_HUBPREQ_DCSURF_SURFACE_PITCH_META_PITCH_MASK,
         s->pitch_pixels - 1u);
    STEP(6, hubp_dcsurf_pri_viewport_start,
         DCN321_F_HUBP_DCSURF_PRI_VIEWPORT_START_PRI_VIEWPORT_X_START_MASK |
         DCN321_F_HUBP_DCSURF_PRI_VIEWPORT_START_PRI_VIEWPORT_Y_START_MASK, 0u);
    STEP(7, hubp_dcsurf_pri_viewport_dimension,
         DCN321_F_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION_PRI_VIEWPORT_WIDTH_MASK |
         DCN321_F_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION_PRI_VIEWPORT_HEIGHT_MASK,
         ((uint32_t)m->height << 16u) | m->width);
    STEP(8, hubpreq_dcsurf_primary_surface_address_high, UINT32_MAX,
         (uint32_t)(s->address >> 32u));
    STEP(9, hubpreq_dcsurf_primary_surface_address, UINT32_MAX,
         (uint32_t)s->address);
#undef STEP

    /* Snapshot the entire transaction BEFORE its first write.  Prevent a
     * bad BAR boundary or preflight read error from partially modifying HW. */
    for (i = 0; i < 10u; ++i) {
        rc = address(d, edits[i].registers, p, &edits[i].mmio_dword);
        if (rc) return rc;
        if (d->io.read(d->io.cookie, edits[i].mmio_dword,
                       &edits[i].previous))
            return DCN321_IO;
        edits[i].replacement =
            (edits[i].previous & ~edits[i].mask) |
            (edits[i].value & edits[i].mask);
    }
    for (i = 0; i < 10u; ++i) {
        if (d->io.write(d->io.cookie, edits[i].mmio_dword,
                        edits[i].replacement)) {
            size_t j;
            /* Restore in forward order to preserve high-then-low latch
             * semantics for the surface MC address.  The failed write may
             * have reached HW, so include its register in compensation. */
            for (j = 0; j <= i; ++j) {
                if (d->io.write(d->io.cookie, edits[j].mmio_dword,
                                edits[j].previous))
                    restore_failed = 1;
            }
            return restore_failed ? DCN321_ROLLBACK : DCN321_IO;
        }
    }
    return DCN321_OK;
}
