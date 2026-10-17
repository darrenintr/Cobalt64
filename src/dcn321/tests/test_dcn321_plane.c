/* SPDX-License-Identifier: MIT */
/* Host-only tests: fake MMIO, never operates a GPU. */
#include "../dcn321.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { REG_COUNT = 0x3000, SEG = 0x100 };
struct fake {
    uint32_t regs[REG_COUNT];
    unsigned read_calls, write_calls, successful_writes, delay_calls;
    unsigned fail_read_at, fail_write_at, fail_write_again_at;
    bool advance_frames;
};
static int read_mmio(void *cookie, uint32_t addr, uint32_t *out) {
    struct fake *f = (struct fake *)cookie;
    ++f->read_calls;
    if (addr >= REG_COUNT || f->read_calls == f->fail_read_at) return -1;
    *out = f->regs[addr];
    return 0;
}
static int write_mmio(void *cookie, uint32_t addr, uint32_t value) {
    struct fake *f = (struct fake *)cookie;
    ++f->write_calls;
    if (addr >= REG_COUNT || f->write_calls == f->fail_write_at ||
        f->write_calls == f->fail_write_again_at) return -1;
    f->regs[addr] = value;
    ++f->successful_writes;
    return 0;
}
static void delay_mmio(void *cookie, uint32_t usec) {
    struct fake *f = (struct fake *)cookie;
    assert(usec > 0u && usec <= 1000000u);
    ++f->delay_calls;
    if (f->advance_frames)
        f->regs[SEG + 0x1b4c] =
            (f->regs[SEG + 0x1b4c] + 1u) & 0x00ffffffu;
}
static void setup(struct fake *f, struct dcn321_device *d) {
    struct dcn321_io io;
    memset(f, 0, sizeof(*f));
    io.cookie = f;
    io.read = read_mmio;
    io.write = write_mmio;
    io.delay_us = delay_mmio;
    assert(dcn321_init(d, &io, 0x7480u, SEG, REG_COUNT,
                       0x10000000ull, 0x12000000ull) == DCN321_OK);
    f->regs[SEG + 0x1b85] = 0x100u; /* simulated OTG clock active */
}
static struct dcn321_mode mode_1080p(void) {
    const struct dcn321_mode m = {
        1920, 1080, 88, 44, 148, 4, 5, 36, true, true, false, 148500
    };
    return m;
}
static struct dcn321_scanout surface_1080p(void) {
    const struct dcn321_scanout s = {0x10010000ull, 1920, 1080};
    return s;
}
static void test_linear_plane(void) {
    struct fake f;
    struct dcn321_device d;
    struct dcn321_pipe_state before, after;
    const struct dcn321_mode m = mode_1080p();
    const struct dcn321_scanout s = surface_1080p();
    setup(&f, &d);
    assert(dcn321_snapshot_pipe(&d, 0, &before) == DCN321_OK);
    assert(!before.otg_enabled && before.clock_on);
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_DISABLED);
    assert(f.successful_writes == 0u);
    d.armed = true;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_OK);
    assert(f.successful_writes == 10u);
    assert(f.regs[SEG + 0x05e5] == 8u); /* GRPH_ARGB8888 */
    assert(f.regs[SEG + 0x05e7] == 0u); /* linear */
    assert(f.regs[SEG + 0x0607] == 1919u); /* pitch - 1 */
    assert(f.regs[SEG + 0x05ea] == ((1080u << 16) | 1920u));
    assert(f.regs[SEG + 0x060a] == 0x10010000u);
    assert(f.regs[SEG + 0x060b] == 0u);
    assert((f.regs[SEG + 0x066c] & 0x00f00000u) ==
           ((2u << 20) | (3u << 22)));
    assert(dcn321_snapshot_pipe(&d, 0, &after) == DCN321_OK);
    assert(after.pitch_pixels == 1920u && after.surface_format == 8u);
    assert(after.gpu_address == s.address && !after.dcc_enabled);
    assert(!after.otg_enabled); /* stage never turns scanout on */
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ABGR8888) ==
           DCN321_OK);
    assert((f.regs[SEG + 0x066c] & 0x00f00000u) ==
           ((3u << 20) | (2u << 22)));
}
static void test_guards(void) {
    struct fake f;
    struct dcn321_device d;
    struct dcn321_mode m = mode_1080p();
    struct dcn321_scanout s = surface_1080p();
    setup(&f, &d);
    d.armed = true;
    f.regs[SEG + 0x1b41] = 1u;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_PRECONDITION);
    f.regs[SEG + 0x1b41] = 0u;
    f.regs[SEG + 0x1b85] = 0u;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_PRECONDITION);
    f.regs[SEG + 0x1b85] = 0x100u;
    f.regs[SEG + 0x1b8a] = 0x100u;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_PRECONDITION);
    f.regs[SEG + 0x1b8a] = 0u;
    f.regs[SEG + 0x061b] = 0x100u;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_PRECONDITION);
    f.regs[SEG + 0x061b] = 0u;
    s.pitch_pixels = 1919;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_ARG);
    s = surface_1080p();
    s.address += 1u;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_ARG);
    s = surface_1080p();
    s.address = 0x11fff000ull;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_RANGE);
    s = surface_1080p();
    m.width = 16384u;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_ARG);
    m = mode_1080p();
    assert(dcn321_prepare_linear_plane(&d, 4, &m, &s, DCN321_ARGB8888) ==
           DCN321_ARG);
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s,
           (enum dcn321_pixel_order)99) == DCN321_ARG);
    assert(f.successful_writes == 0u);
}
static void test_rollback(void) {
    struct fake f;
    struct dcn321_device d;
    uint32_t saved[REG_COUNT];
    const struct dcn321_mode m = mode_1080p();
    const struct dcn321_scanout s = surface_1080p();
    setup(&f, &d);
    d.armed = true;
    f.regs[SEG + 0x061a] = 0x02u; /* previous DCC bit */
    f.regs[SEG + 0x05e7] = 0x03u;
    f.regs[SEG + 0x060a] = 0x11000000u;
    memcpy(saved, f.regs, sizeof(saved));
    f.fail_read_at = 8u; /* snapshot before any writes */
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_IO);
    assert(f.successful_writes == 0u);
    assert(memcmp(saved, f.regs, sizeof(saved)) == 0);
    f.fail_read_at = 0;
    f.read_calls = 0;
    f.fail_write_at = 5u;
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_IO);
    assert(memcmp(saved, f.regs, sizeof(saved)) == 0);
    setup(&f, &d);
    d.armed = true;
    f.fail_write_at = 3u;
    f.fail_write_again_at = 4u; /* compensation itself fails */
    assert(dcn321_prepare_linear_plane(&d, 0, &m, &s, DCN321_ARGB8888) ==
           DCN321_ROLLBACK);
}
static void test_frame_watch(void) {
    struct fake f;
    struct dcn321_device d;
    setup(&f, &d);
    assert(dcn321_wait_frame_progress(&d, 0, 3u, 1000u) ==
           DCN321_PRECONDITION);
    f.regs[SEG + 0x1b41] = 1u;
    assert(dcn321_wait_frame_progress(&d, 0, 3u, 1000u) ==
           DCN321_TIMEOUT);
    assert(f.delay_calls == 3u);
    f.advance_frames = true;
    f.regs[SEG + 0x1b4c] = 0x00ffffffu;
    assert(dcn321_wait_frame_progress(&d, 0, 3u, 1000u) == DCN321_OK);
    assert(f.delay_calls == 4u);
    assert(dcn321_wait_frame_progress(&d, 0, 0u, 1000u) == DCN321_ARG);
    assert(dcn321_wait_frame_progress(&d, 0, 3u, 0u) == DCN321_ARG);
    assert(dcn321_wait_frame_progress(&d, 4, 3u, 1000u) == DCN321_ARG);
    assert(f.successful_writes == 0u);
}
static void test_timing_transaction(void) {
    struct fake f;
    struct dcn321_device d;
    uint32_t saved[REG_COUNT];
    const struct dcn321_mode m = mode_1080p();
    setup(&f, &d);
    d.armed = true;
    f.regs[SEG + 0x1b2a] = 0x80000000u;
    f.regs[SEG + 0x1b2b] = 0x00001234u;
    f.regs[SEG + 0x1b41] = 0x8000u; /* preserve unrelated OTG bits */
    memcpy(saved, f.regs, sizeof(saved));
    f.fail_read_at = 4u; /* initial precondition read + edit preflight */
    assert(dcn321_program_otg_timing(&d, 0, &m) == DCN321_IO);
    assert(f.successful_writes == 0u);
    assert(memcmp(saved, f.regs, sizeof(saved)) == 0);
    f.fail_read_at = 0u;
    f.read_calls = 0u;
    f.fail_write_at = 8u;
    assert(dcn321_program_otg_timing(&d, 0, &m) == DCN321_IO);
    assert(memcmp(saved, f.regs, sizeof(saved)) == 0);
    setup(&f, &d);
    d.armed = true;
    f.fail_write_at = 2u;
    f.fail_write_again_at = 3u;
    assert(dcn321_program_otg_timing(&d, 0, &m) == DCN321_ROLLBACK);
    setup(&f, &d);
    d.armed = true;
    f.regs[SEG + 0x1b41] = 1u; /* active OTG */
    assert(dcn321_program_otg_timing(&d, 0, &m) == DCN321_PRECONDITION);
    assert(f.successful_writes == 0u);
    f.regs[SEG + 0x1b41] = 0u;
    f.regs[SEG + 0x1b2a] = 0x80000000u;
    assert(dcn321_program_otg_timing(&d, 0, &m) == DCN321_OK);
    assert(f.successful_writes == 12u);
    assert(f.regs[SEG + 0x1b2a] == (0x80000000u | 2199u));
    assert(f.regs[SEG + 0x1b2f] == 1124u);
    assert(f.regs[SEG + 0x1b41] == 0u); /* never enable OTG */
}

static void configure_active_linear(struct fake *f) {
    f->regs[SEG + 0x1b41] = 1u; /* active OTG */
    f->regs[SEG + 0x1b85] = 0x100u; /* clock on */
    f->regs[SEG + 0x0607] = 1919u;
    f->regs[SEG + 0x05e5] = 8u;
    f->regs[SEG + 0x05ea] = (1080u << 16u) | 1920u;
    f->regs[SEG + 0x060a] = 0x10090000u;
}
static void test_flip_preflight_and_rollback(void) {
    struct fake f;
    struct dcn321_device d;
    struct dcn321_scanout s = surface_1080p();
    uint32_t saved[REG_COUNT];
    setup(&f, &d);
    d.armed = true;
    configure_active_linear(&f);
    f.regs[SEG + 0x061b] = 0x100u;
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_PRECONDITION);
    assert(f.successful_writes == 0u);
    f.regs[SEG + 0x061b] = 0u;
    f.regs[SEG + 0x0607] = 1918u;
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_PRECONDITION);
    f.regs[SEG + 0x0607] = 1919u;
    f.regs[SEG + 0x061a] = 0x02u; /* DCC incompatible */
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_PRECONDITION);
    f.regs[SEG + 0x061a] = 0u;
    f.regs[SEG + 0x05e7] = 2u; /* not linear */
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_PRECONDITION);
    f.regs[SEG + 0x05e7] = 0u;
    f.regs[SEG + 0x061a] = 0x01u; /* trusted memory zone cannot be mixed */
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_PRECONDITION);
    f.regs[SEG + 0x061a] = 0u;
    f.regs[SEG + 0x05e5] = 8u | (1u << 8u); /* rotated surface */
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_PRECONDITION);
    f.regs[SEG + 0x05e5] = 8u;
    f.regs[SEG + 0x05ea] = 0u; /* missing viewport */
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_PRECONDITION);
    f.regs[SEG + 0x05ea] = (1080u << 16u) | 1920u;
    assert(f.successful_writes == 0u);

    memcpy(saved, f.regs, sizeof(saved));
    f.fail_read_at = f.read_calls + 5u;
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_IO);
    assert(f.successful_writes == 0u);
    assert(memcmp(saved, f.regs, sizeof(saved)) == 0);
    f.fail_read_at = 0u;

    f.fail_write_at = 2u; /* high write succeeds, low latch fails */
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_IO);
    assert(memcmp(saved, f.regs, sizeof(saved)) == 0);
    f.fail_write_at = 0u;
    f.write_calls = 0u;
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_OK);
    assert(f.regs[SEG + 0x060a] == 0x10010000u);

    setup(&f, &d);
    d.armed = true;
    configure_active_linear(&f);
    f.fail_write_at = 2u;
    f.fail_write_again_at = 3u; /* cannot restore old high */
    assert(dcn321_flip_primary(&d, 0, &s) == DCN321_ROLLBACK);
}

int main(void) {
    test_linear_plane();
    test_guards();
    test_rollback();
    test_frame_watch();
    test_timing_transaction();
    test_flip_preflight_and_rollback();
    puts("DCN321 linear plane / MMIO failure / frame-progress tests passed (mock only)");
    return 0;
}
