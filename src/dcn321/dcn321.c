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
