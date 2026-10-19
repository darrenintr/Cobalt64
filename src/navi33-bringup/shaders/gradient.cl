// SPDX-License-Identifier: MIT
// First GPU-calculated frame for Navi33 (gfx1102): every work-item computes
// one XRGB8888 pixel of a gradient. Built offline by shaders/build.sh.
//
// No device libraries and no HSA dispatch packet: the position comes from
// the workgroup and work-item IDs alone, so a PM4 DISPATCH_DIRECT with the
// kernarg pointer in user SGPRs is enough. Fixed 8x8 workgroups.
typedef unsigned int uint32_t;

__kernel __attribute__((reqd_work_group_size(8, 8, 1)))
void cobalt_gradient(__global uint32_t *pixels, uint32_t pitch_pixels,
                     uint32_t width, uint32_t height, uint32_t frame) {
    const uint32_t x = __builtin_amdgcn_workgroup_id_x() * 8u + __builtin_amdgcn_workitem_id_x();
    const uint32_t y = __builtin_amdgcn_workgroup_id_y() * 8u + __builtin_amdgcn_workitem_id_y();
    if (x >= width || y >= height)
        return;
    const uint32_t r = (x * 255u) / width;
    const uint32_t g = (y * 255u) / height;
    const uint32_t b = ((x ^ y) + frame) & 0xffu;
    pixels[y * pitch_pixels + x] = (r << 16) | (g << 8) | b;
}
