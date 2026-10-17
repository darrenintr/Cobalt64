// Offline Navi33 IP map / DCN3.2.1 read-offset plan. NO GPU ACCESS.
#include "../src/Navi33HardwarePlan.h"
#include "../src/Navi33Dcn321.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <vector>

int main(int argc,char **argv) {
    if(argc!=3 || argv[2][0]=='-') {
        std::fprintf(stderr,
          "usage: navi33-backend-report <full-ROM-or-TMR-snapshot> <assigned-BAR5-bytes>\n");
        return 2;
    }
    char *end=nullptr;
    errno=0;
    const unsigned long long aperture=std::strtoull(argv[2],&end,0);
    if(errno || end==argv[2] || *end!='\0' || !aperture){
        std::fprintf(stderr,"invalid BAR5 size (decimal or 0xhex)\n");
        return 2;
    }
    std::FILE *f=std::fopen(argv[1],"rb");
    if(!f){std::perror("open");return 2;}
    if(std::fseek(f,0,SEEK_END)!=0){std::fclose(f);return 2;}
    const long len=std::ftell(f);
    if(len<12 || static_cast<unsigned long>(len)>navi33::IPDiscovery::kMaxBlob ||
       std::fseek(f,0,SEEK_SET)!=0){
       std::fclose(f);std::fprintf(stderr,"invalid snapshot length\n");return 2;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    const bool got=std::fread(bytes.data(),1,bytes.size(),f)==bytes.size();
    std::fclose(f);
    if(!got){std::fprintf(stderr,"incomplete snapshot read\n");return 2;}

    navi33::IPDiscovery disc;
    if(!disc.parse(bytes.data(),bytes.size())){
        std::fprintf(stderr,"invalid AMD IP Discovery checksum/offset/records\n");
        return 1;
    }
    const navi33::InventoryReport v=navi33::inventory(disc);
    std::printf("Navi33 engine gate: code=%u matched=%u (expected 5)\n",
                static_cast<unsigned>(v.code),v.matchedEngines);
    for(const auto &spec:navi33::kNavi33Engines) {
        navi33::IPEntry e{};
        const bool present=disc.find(spec.hwid,spec.instance,e);
        std::printf("  %s hwid=%u expected=%u.%u.%u",
                    spec.name,spec.hwid,spec.major,spec.minor,spec.revision);
        if(present)
            std::printf(" observed=%u.%u.%u segments=%u\n",
                        e.major,e.minor,e.revision,e.numBases);
        else std::puts(" MISSING or DUPLICATE");
    }
    if(v.code!=navi33::InventoryCode::Pass){
        std::fprintf(stderr,"generation mismatch: never attempt hardware init\n");
        return 1;
    }
    const auto plan=navi33::dcn321::buildReadPlan(disc,static_cast<uint64_t>(aperture));
    if(plan.code!=navi33::AddressCode::Pass) {
        std::fprintf(stderr,"BAR5 DCN read plan refused: code=%u resolved=%u\n",
                    static_cast<unsigned>(plan.code),plan.resolved);
        return 1;
    }
    std::printf("DCN3.2.1 register offsets (BAR5-relative bytes, NOT actual reads):\n");
    for(size_t i=0;i<navi33::dcn321::kReadOnlyCount;++i)
        std::printf("  %-50s 0x%llx\n",
                    navi33::dcn321::kReadOnlyRegisters[i].linuxName,
                    static_cast<unsigned long long>(plan.offsets[i]));
    std::puts("METADATA READY. Firmware, GPU queues, DCN mode setting, Vulkan and Metal: NOT IMPLEMENTED.");
    return 0;
}
