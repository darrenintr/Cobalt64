#include "../src/Navi33NativeABI.h"
#include "../src/Navi33Gfx11Command.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace navi33::gfx11;
int main(){
  n33_capabilities c{};
  c.abi_version=N33_ABI_VERSION;c.size=sizeof(c);c.vendor_id=N33_PCI_VENDOR;
  c.device_id=N33_PCI_DEVICE;c.gfx_ip_version=N33_GFX_IP_VERSION;c.family=N33_FAMILY_GFX1102;
  c.capabilities=N33_CAP_PCI_INVENTORY;
  assert(n33_is_reserved_clean(&c));
  assert(!n33_can_submit(&c));
  c.capabilities|=N33_CAP_HW_GFX;
  assert(!n33_can_submit(&c));
  c.capabilities|=N33_CAP_HW_VM;
  assert(n33_can_submit(&c));
  c.reserved0=1;assert(!n33_is_reserved_clean(&c));c.reserved0=0;
  c.device_id=0x7550;assert(!n33_can_submit(&c));c.device_id=N33_PCI_DEVICE;
  c.gfx_ip_version=0x000c0001;assert(!n33_can_submit(&c));
  uint32_t words[3]={0xdeadbeefu,0xcafebabeu,0xaabbccddu};
  assert(write_nop(words,0)==Error::Capacity && words[0]==0xdeadbeefu);
  assert(write_nop(words,3)==Error::Ok && words[0]==0xffff1000u);
  Frame f{};
  assert(inspect(words,1,&f)==Error::Ok && f.opcode==kNop && f.payload_words==0);
  words[0]=packet3(kWriteData,3);
  assert(inspect(words,3,&f)==Error::Length);
  assert(inspect(words,5,&f)==Error::Ok && f.payload_words==4);
  words[0]=packet3(0xff,1);
  assert(inspect(words,3,&f)==Error::InvalidOpcode);
  words[0]=packet3(kReleaseMem,6);
  assert(validate_submission({false,true,true,true},words,8)==Error::NotReady);
  assert(validate_submission({true,true,true,true},words,8)==Error::Ok);
  assert(!kHardwareSubmissionImplemented);
  std::cout<<"Navi33 ABI and offline gfx11 framing tests passed; no hardware submission\n";
}
