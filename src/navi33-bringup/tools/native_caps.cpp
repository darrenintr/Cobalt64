#include "../src/Navi33NativeABI.h"
#include <cstdio>
int main(){
  n33_capabilities c{};
  c.abi_version=N33_ABI_VERSION;c.size=sizeof(c);
  c.vendor_id=N33_PCI_VENDOR;c.device_id=N33_PCI_DEVICE;
  c.gfx_ip_version=N33_GFX_IP_VERSION;c.family=N33_FAMILY_GFX1102;
  c.capabilities=N33_CAP_PCI_INVENTORY;
  std::printf("N33N ABI %u PCI %04x:%04x gfx %x; hardware submit=%s; Metal=NO\n",
      c.abi_version,c.vendor_id,c.device_id,c.gfx_ip_version,
      n33_can_submit(&c)?"YES":"NO");
  return 0;
}
