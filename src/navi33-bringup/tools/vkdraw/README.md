# RX 7600 Vulkan graphics draw

`vkdraw /absolute/path/to/libvulkan_radeon.dylib` opens the explicit ICD and
requires exactly one discrete AMD device `1002:7480`. It submits a vertex and
fragment shader on a graphics queue into a 64×64 BGRA8 image. A scissor limits
the red triangle to the central 32×32 square; the surrounding pixels are
cleared opaque black. All 4096 pixels are checked after a two-second fence
wait and image-to-buffer copy. Three submissions reuse the resources, with
CPU poison before each submission to detect stale readback.

This is graphics execution evidence only. It does not establish Metal,
WindowServer composition, or display presentation. A timed-out submission
is not replayed and resources are not explicitly destroyed after uncertain
completion; the kernel must retain/quarantine the failed context.

Build with `make VK_INCLUDE=/path/to/Vulkan-Headers/include`. On macOS use
`make CC=clang LDLIBS=`. The shader arrays are committed; `make shaders`
regenerates them using `glslc` and Python. CI packages the macOS binary as
`n33-vkdraw` alongside the matching kext. No GPU test runs in CI.

Synchronization follows the Khronos contracts for
[image memory barriers](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html)
and [image-to-buffer copies](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html).
