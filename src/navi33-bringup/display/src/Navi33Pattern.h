// Opt-in, CPU-written diagnostic stripe at top of *existing* GOP scanout.
// Pure C++17 pattern math for host unit tests; never issues GPU commands.
// Does not assert the display controller is still scanning this memory.
#pragma once
#include <stdint.h>
namespace navi33fb { namespace pattern {
constexpr uint32_t kStripeRows=32u;
constexpr uint32_t kRed=0xffe53333u;
constexpr uint32_t kGreen=0xff33cc55u;
constexpr uint32_t kBlue=0xff3399eeu;
constexpr uint32_t kWhite=0xffffffffu;
constexpr uint32_t at(uint32_t x,uint32_t width) {
    if(width==0u)return 0u;
    const uint64_t quartile=(static_cast<uint64_t>(x)*4u)/width;
    return quartile==0u?kRed:quartile==1u?kGreen:
           quartile==2u?kBlue:kWhite;
}
} } // namespace navi33fb::pattern
