// SPDX-License-Identifier: MIT
// Host-only GFX11 PM4 packet framing. No GPU ring, MMIO or firmware submission.
// Reference: Linux amdgpu nvd.h PACKET3 encoding and gfx_v11_0.c.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace navi33 { namespace gfx11 {
enum class Error : uint8_t { Ok, Null, Capacity, InvalidOpcode, Length, NotReady };
constexpr uint32_t kType3=3u, kNop=0x10u, kWriteData=0x37u, kReleaseMem=0x49u;
constexpr uint32_t packet3(uint32_t op,uint32_t count) {
  return (3u<<30u)|((count&0x3fffu)<<16u)|((op&0xffu)<<8u);
}
constexpr uint32_t nop() { return packet3(kNop,0x3fffu); }
struct Frame { uint32_t header; uint32_t opcode; uint32_t payload_words; };
inline Error inspect(const uint32_t *words,size_t length,Frame *result) {
  if(!words||!result) return Error::Null;
  if(length==0) return Error::Length;
  const uint32_t header=words[0];
  if((header>>30u)!=3u || (header&0xffu)) return Error::InvalidOpcode;
  const uint32_t op=(header>>8u)&0xffu, count=(header>>16u)&0x3fffu;
  if(op==kNop && count==0x3fffu && length==1u) {
    *result={header,op,0u}; return Error::Ok;
  }
  if(op!=kNop && op!=kWriteData && op!=kReleaseMem) return Error::InvalidOpcode;
  if(count==0x3fffu || static_cast<size_t>(count)+2u!=length) return Error::Length;
  *result={header,op,count+1u}; return Error::Ok;
}
inline Error write_nop(uint32_t *target,size_t words) {
  if(!target) return Error::Null;
  if(words<1u) return Error::Capacity;
  target[0]=nop();
  return Error::Ok;
}
struct Readiness { bool firmware,vm,ring,fences; };
constexpr bool hardwareReady(Readiness s) {
  return s.firmware && s.vm && s.ring && s.fences;
}
inline Error validate_submission(Readiness s,const uint32_t *words,size_t count) {
  if(!hardwareReady(s)) return Error::NotReady;
  Frame frame{};
  return inspect(words,count,&frame);
}
// This module intentionally has NO method to submit a packet to physical GPU.
constexpr bool kHardwareSubmissionImplemented=false;
static_assert(!kHardwareSubmissionImplemented,"Never submit mock GFX11 packets to GPU");
}}
