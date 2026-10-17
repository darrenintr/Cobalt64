#include "../src/Navi33Identity.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

using navi33::IdentityStatus;
using navi33::inspectPci;

static_assert(inspectPci(0x74801002u, 0x030000c0u).status == IdentityStatus::Accepted,
              "RX 7600 must be detected at build time");
static_assert(inspectPci(0x75501002u, 0x03000001u).status == IdentityStatus::UnsupportedDevice,
              "Never bind Navi 48 using the RDNA3 driver");
static_assert(inspectPci(0x74808086u, 0x03000001u).status == IdentityStatus::WrongVendor,
              "Never bind another vendor's PCI device");

int main() {
    const auto rx7600 = inspectPci(0x74801002u, 0x030000c0u);
    assert(navi33::isSupported(rx7600));
    assert(rx7600.vendorId == 0x1002u && rx7600.deviceId == 0x7480u);
    assert(rx7600.revisionId == 0xc0u);
    assert(rx7600.baseClass == 3u && rx7600.subClass == 0u && rx7600.progIf == 0u);

    const auto otherAmd = inspectPci(0x75511002u, 0x03000011u);
    assert(otherAmd.status == IdentityStatus::UnsupportedDevice);
    assert(!navi33::isSupported(otherAmd));

    const auto nvidia = inspectPci(0x748010deu, 0x03000011u);
    assert(nvidia.status == IdentityStatus::WrongVendor);
    assert(!navi33::isSupported(nvidia));

    const auto wrongClass = inspectPci(0x74801002u, 0x02000011u);
    assert(wrongClass.status == IdentityStatus::NotDisplayController);
    assert(!navi33::isSupported(wrongClass));

    const auto displayOtherSubclass = inspectPci(0x74801002u, 0x0380007fu);
    assert(displayOtherSubclass.status == IdentityStatus::Accepted);
    assert(displayOtherSubclass.subClass == 0x80u);
    assert(navi33::isSupported(displayOtherSubclass));

    const auto empty = inspectPci(0xffffffffu, 0xffffffffu);
    assert(!navi33::isSupported(empty));

    std::puts("Navi33 PCI identity: 7 cases passed; no hardware accessed");
    return 0;
}
