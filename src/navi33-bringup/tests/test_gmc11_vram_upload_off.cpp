// Default build must never execute GMC11 BAR0 root uploads.
#include "../src/Navi33Gmc11VramUpload.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace navi33;
    static_assert(!vram_upload::kWritePathCompiled,
                  "Unsafe BAR0 upload must not be present in default build");
    vram_upload::Gate g{};
    vram_geometry::Geometry geometry{};
    vram_bo::Arena arena{};
    mmhub11::Table table{};
    vram_upload::IO io{};
    assert(vram_upload::uploadRoot(
        g,geometry,arena,nullptr,{},table,io).code==
        vram_upload::Code::Disabled);
    std::puts("Navi33 shipping build has no physical BAR0 upload path");
}
