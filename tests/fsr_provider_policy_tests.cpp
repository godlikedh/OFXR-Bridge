#include "xrfg/fsr_provider_policy.hpp"
#include <cstdlib>
#include <iostream>
int main() {
    using namespace xrfg;
    const auto require=[](bool ok) { if (!ok) std::abort(); };
    require(fsr_ml_version("4.0.2"));
    require(fsr_ml_version("AMD FSR Frame Generation 4.0.2"));
    require(!fsr_ml_version("3.1.6")); require(!fsr_ml_version("1.1.3"));
    require(!fsr_ml_version("14.0.2")); require(!fsr_ml_version("4.0"));
    require(choose_fsr_provider(FsrProviderRequest::automatic,true,true,true,true,true)==FsrProviderChoice::ml);
    for (unsigned missing=0;missing<5;++missing) {
        const bool flags[]={missing!=0,missing!=1,missing!=2,missing!=3,missing!=4};
        require(choose_fsr_provider(FsrProviderRequest::automatic,flags[0],flags[1],flags[2],flags[3],flags[4])==FsrProviderChoice::analytical);
        require(choose_fsr_provider(FsrProviderRequest::ml,flags[0],flags[1],flags[2],flags[3],flags[4])==FsrProviderChoice::unavailable);
    }
    require(choose_fsr_provider(FsrProviderRequest::analytical,true,true,true,true,true)==FsrProviderChoice::analytical);
    std::cout << "Provider selection: OS, SM, vendor, scene camera and enumerated ML required\n";
}
