#include "xrfg/fsr_foveation.hpp"
#include <iostream>
#include <limits>
using namespace xrfg::fsr_foveation;
int main() {
    unsigned failures{};
    const auto check=[&](bool value,const char* why){if(!value){std::cerr<<why<<'\n';++failures;}};
    const Field eye{-1,1,1,-1},old_inset{-.6F,.4F,.5F,-.5F},new_inset{-.4F,.6F,.5F,-.5F};
    const auto a=mapping(new_inset,eye),b=mapping(old_inset,eye);
    check(covers(mapping(eye,eye))&&!covers(a)&&inset(a),"coverage/inset distinction");
    const auto stationary=motion(a,b,{.5F,.5F},{.2F,0});
    check(std::abs(stationary[0])<1e-6F&&stationary[1]==0,"stationary scene under moving gaze");
    const auto object=motion(a,b,{.5F,.5F},{.3F,-.1F});
    check(std::abs(object[0]-.05F)<1e-6F&&std::abs(object[1]+.05F)<1e-6F,"moving object under moving gaze");
    const Field asymmetric{-.8F,1.2F,.9F,-1.1F};
    const auto identity=motion(mapping(asymmetric,asymmetric),mapping(asymmetric,asymmetric),{.2F,.9F},{.1F,-.2F});
    check(std::abs(identity[0]-.1F)<1e-6F&&std::abs(identity[1]+.2F)<1e-6F,"asymmetric eye signs");
    check(!valid({0,0,1,-1})&&!valid({0,1,std::numeric_limits<float>::quiet_NaN(),-1}),"invalid projection refused");
    return failures?1:0;
}
