// SPDX-License-Identifier: MIT
// FIPS 180-4 SHA-256, host-testable and freestanding (no libkern KPI), so
// the Kext and the host tests pin firmware with the same code.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace sha256 {
struct Digest { uint8_t b[32]; };
namespace detail {
constexpr uint32_t kK[64]={
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
inline uint32_t ror(uint32_t x,unsigned n) { return (x>>n)|(x<<(32u-n)); }
inline void block(uint32_t h[8],const uint8_t *p) {
    uint32_t w[64];
    for(unsigned i=0;i<16u;++i)
        w[i]=(static_cast<uint32_t>(p[4*i])<<24u)|
             (static_cast<uint32_t>(p[4*i+1])<<16u)|
             (static_cast<uint32_t>(p[4*i+2])<<8u)|p[4*i+3];
    for(unsigned i=16;i<64u;++i) {
        const uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3u);
        const uint32_t s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10u);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],k=h[7];
    for(unsigned i=0;i<64u;++i) {
        const uint32_t t1=k+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^(~e&g))+
                          kK[i]+w[i];
        const uint32_t t2=(ror(a,2)^ror(a,13)^ror(a,22))+((a&b)^(a&c)^(b&c));
        k=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=k;
}
} // namespace detail
inline Digest hash(const uint8_t *data,size_t len) {
    uint32_t h[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                   0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    size_t i=0;
    for(;len-i>=64u;i+=64u)detail::block(h,data+i);
    uint8_t tail[128]={};
    const size_t rest=len-i;
    for(size_t j=0;j<rest;++j)tail[j]=data[i+j];
    tail[rest]=0x80u;
    const size_t total=rest<56u ? 64u : 128u;
    const uint64_t bits=static_cast<uint64_t>(len)*8u;
    for(unsigned j=0;j<8u;++j)
        tail[total-1u-j]=static_cast<uint8_t>(bits>>(8u*j));
    detail::block(h,tail);
    if(total==128u)detail::block(h,tail+64);
    Digest out{};
    for(unsigned j=0;j<8u;++j)
        for(unsigned k=0;k<4u;++k)
            out.b[4*j+k]=static_cast<uint8_t>(h[j]>>(24u-8u*k));
    return out;
}
inline bool equal(const Digest &a,const Digest &b) {
    uint8_t diff=0u;
    for(unsigned i=0;i<32u;++i)diff|=static_cast<uint8_t>(a.b[i]^b.b[i]);
    return diff==0u;
}
} } // namespace navi33::sha256
