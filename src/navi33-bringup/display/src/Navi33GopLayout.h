// Copyright (c) 2026 Navi33 contributors.
// BSD-3-Clause-influenced framebuffer-adoption design; see ../LICENSE-RDNA4FB.txt.
// Pure CPU-side GOP scanout validation: NO kernel calls or hardware access.
#pragma once
#include <stdint.h>

namespace navi33fb {

enum class LayoutError : uint8_t {
    Ok = 0, NullFramebuffer, InvalidDimension, UnsupportedDepth,
    InvalidPitch, InvalidLength, Overflow
};

struct Layout {
    LayoutError error;
    uint64_t physicalBase, length;
    uint32_t width, height, rowBytes, depth;
};

constexpr uint32_t kMaxDimension = 8192u;
constexpr uint64_t kMaxAperture = 256ull * 1024ull * 1024ull;

constexpr Layout inspectGop(uint64_t rawBase, uint64_t length,
                            uint32_t width, uint32_t height,
                            uint32_t rowBytes, uint32_t depth) {
    const uint64_t base = rawBase & ~0xfffull; // PE_Video low 12 bits can be flags
    const uint64_t minPitch = static_cast<uint64_t>(width) * 4ull;
    const uint64_t minLength = static_cast<uint64_t>(rowBytes) * height;
    const uint64_t effective = length ? length : minLength;
    const LayoutError status =
        !base ? LayoutError::NullFramebuffer :
        !width || !height || width > kMaxDimension || height > kMaxDimension
            ? LayoutError::InvalidDimension :
        depth != 32u ? LayoutError::UnsupportedDepth :
        rowBytes < minPitch || (rowBytes & 3u) != 0u ? LayoutError::InvalidPitch :
        effective < minLength || effective > kMaxAperture
            ? LayoutError::InvalidLength :
        base > UINT64_MAX - effective ? LayoutError::Overflow :
        LayoutError::Ok;
    return {status, base, effective, width, height, rowBytes, depth};
}

constexpr bool valid(const Layout &layout) {
    return layout.error == LayoutError::Ok;
}

// The six diagnostic channel permutations in RDNA4FB, with 0 = normal BGRA.
struct Channels {
    uint8_t red, green, blue;
};
constexpr Channels channelMapping(uint32_t id) {
    return id == 1u ? Channels{1u, 0u, 2u} :
           id == 2u ? Channels{0u, 1u, 2u} :
           id == 3u ? Channels{2u, 0u, 1u} :
           id == 4u ? Channels{1u, 2u, 0u} :
           id == 5u ? Channels{0u, 2u, 1u} :
                      Channels{2u, 1u, 0u};
}
constexpr uint32_t channelMask(uint8_t byte) {
    return 0xffu << (static_cast<uint32_t>(byte) * 8u);
}

} // namespace navi33fb
