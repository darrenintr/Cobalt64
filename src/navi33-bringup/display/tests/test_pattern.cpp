#include "../src/Navi33Pattern.h"
#include <cassert>
#include <cstdio>
using namespace navi33fb::pattern;
static_assert(at(0,1920)==kRed,"first quarter red");
static_assert(at(480,1920)==kGreen,"second quarter green");
static_assert(at(960,1920)==kBlue,"third quarter blue");
static_assert(at(1440,1920)==kWhite,"final quarter white");
int main(){
    assert(kStripeRows==32u);
    assert(at(0,0u)==0u);
    assert(at(1919u,1920u)==kWhite);
    assert(at(479u,1920u)==kRed);
    assert(at(959u,1920u)==kGreen);
    assert(at(1439u,1920u)==kBlue);
    for(unsigned x=0;x<1920u;++x)
        assert(at(x,1920u)!=0u);
    std::puts("Navi33 GOP CPU test-pattern colours/bounds PASS; no GPU");
}
