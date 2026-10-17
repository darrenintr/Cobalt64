#include "../src/Navi33Sha256.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace navi33::sha256;
static bool is(const Digest &d,const char *hex) {
    char s[65];
    for(unsigned i=0;i<32u;++i)std::snprintf(s+2*i,3,"%02x",d.b[i]);
    return std::strcmp(s,hex)==0;
}
static Digest of(const char *text) {
    return hash(reinterpret_cast<const uint8_t *>(text),std::strlen(text));
}
int main() {
    // FIPS 180-4 / NIST CSRC examples: one block, two-block padding, empty.
    assert(is(of("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(is(of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    assert(is(of(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    // 64 bytes exactly: the padding gets a block of its own.
    assert(is(of("0123456789012345678901234567890123456789012345678901234567890123"),
        "9674d9e078535b7cec43284387a6ee39956188e735a85452b0050b55341cda56"));
    std::vector<uint8_t> million(1000000u,'a');
    assert(is(hash(million.data(),million.size()),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    Digest a=of("abc"),b=a;
    assert(equal(a,b));
    b.b[31]^=1u;
    assert(!equal(a,b));
    std::puts("SHA-256 FIPS 180-4 vectors passed");
    return 0;
}
