/* SPDX-License-Identifier: MIT */
#include "../dcn321.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fake { uint32_t regs[0x3000]; unsigned writes; };
static int rd(void *cookie,uint32_t a,uint32_t *v) {
    struct fake *f=(struct fake *)cookie;
    if(a>=0x3000)return -1;
    *v=f->regs[a];return 0;
}
static int wr(void *cookie,uint32_t a,uint32_t v) {
    struct fake *f=(struct fake *)cookie;
    if(a>=0x3000)return -1;
    f->regs[a]=v;f->writes++;return 0;
}
int main(void) {
    struct fake f;
    struct dcn321_device d;
    struct dcn321_io io={&f,rd,wr,0};
    struct dcn321_mode m={1920,1080,88,44,148,4,5,36,true,true,false,148500};
    struct dcn321_scanout s={0x10010000ull,1920,1080};
    uint32_t count;
    memset(&f,0,sizeof(f));
    assert(dcn321_init(&d,&io,0x7480,0x100,0x3000,0x10000000ull,0x12000000ull)==0);
    assert(!d.armed);
    assert(dcn321_validate_mode(&m)==0);
    assert(dcn321_program_otg_timing(&d,0,&m)==DCN321_DISABLED);
    assert(f.writes==0);
    assert(dcn321_read_frame(&d,0,&count)==0);
    d.armed=true;
    assert(dcn321_program_otg_timing(&d,0,&m)==0);
    assert(f.regs[0x100+0x1b2a]==2199u);
    assert(f.regs[0x100+0x1b2f]==1124u);
    assert(f.regs[0x100+0x1b2b]==(192u<<16|2112u));
    assert(f.regs[0x100+0x1b36]==(41u<<16|1121u));
    assert(f.regs[0x100+0x1b37]==(5u<<16));
    assert(dcn321_flip_primary(&d,0,&s)==DCN321_PRECONDITION);
    f.regs[0x100+0x1b41]=1u; /* simulated active pipe, NOT enabled by this library */
    f.regs[0x100+0x1b85]=0x100u; /* clock on */
    f.regs[0x100+0x0607]=1919u; /* linear 1920-pixel pitch */
    f.regs[0x100+0x05e5]=8u; /* 32bpp ARGB */
    f.regs[0x100+0x05ea]=(1080u<<16)|1920u; /* viewport */
    assert(dcn321_program_otg_timing(&d,0,&m)==DCN321_PRECONDITION);
    assert(dcn321_flip_primary(&d,0,&s)==0);
    assert(f.regs[0x100+0x060a]==0x10010000u);
    s.address=0x11fff000ull;
    assert(dcn321_flip_primary(&d,0,&s)==DCN321_RANGE);
    assert(dcn321_read_frame(&d,4,&count)==DCN321_ARG);
    assert(dcn321_init(&d,&io,0x73bf,0x100,0x3000,0x10000000ull,0x12000000ull)==DCN321_ARG);
    puts("DCN321 host tests passed (mock hardware only)");
    return 0;
}
