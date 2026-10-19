// SPDX-License-Identifier: MIT
// Firmware built into the Kext for the boot-time bring-up (Navi33Autostart.h).
// The definitions are generated at build time by tools/embed-firmware.py, which
// refuses any file that does not match its pinned SHA-256.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace navi33 { namespace embedded {
extern const uint8_t kIpd[];   extern const size_t kIpdBytes;   // RX 7600 IP Discovery
extern const uint8_t kSos[];   extern const size_t kSosBytes;   // psp_13_0_7_sos.bin
extern const uint8_t kIpFw[];  extern const size_t kIpFwBytes;  // the nine IP firmware files, File order
} }
