// Offline-only AMD IP Discovery inspector: no PCI, MMIO or hardware access.
#include "../src/Navi33Discovery.h"
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: navi33-discovery-dump <full-ROM-or-TMR-snapshot>\n");
        return 2;
    }
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("open"); return 2; }
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return 2; }
    const long n = std::ftell(f);
    if (n <= 0 || static_cast<unsigned long>(n) > navi33::IPDiscovery::kMaxBlob ||
        std::fseek(f, 0, SEEK_SET) != 0) {
        std::fprintf(stderr, "invalid snapshot size (must be 1..4 MiB)\n");
        std::fclose(f); return 2;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    const bool ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "could not read entire snapshot\n"); return 2; }
    navi33::IPDiscovery parser;
    if (!parser.parse(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "No valid checksum-verified AMD IP Discovery table found\n");
        return 1;
    }
    std::printf("Discovery at 0x%zx; %u IP blocks; %s base addresses\n",
                parser.binaryOffset(), parser.count(), parser.is64BitBase()?"64-bit":"32-bit");
    navi33::IPEntry e;
    for (uint16_t i = 0; i < parser.count(); ++i) {
        if (!parser.entryAt(i, e)) return 1;
        std::printf("hwid=%u instance=%u version=%u.%u.%u bases=%u\n",
                    e.hwId,e.instance,e.major,e.minor,e.revision,e.numBases);
    }
    std::printf("Navi33 target match: %s (identification only; NOT driver-ready)\n",
                parser.isNavi33Candidate()?"YES":"NO");
    return 0;
}
