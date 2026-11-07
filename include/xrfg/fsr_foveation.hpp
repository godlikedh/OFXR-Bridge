#pragma once
// Projection-domain arithmetic shared by the GPU composer and CPU regressions.
// UV is top-left origin; field is {left,right,up,down} in view-space tangents.
#include <array>
#include <cmath>
#include <algorithm>
namespace xrfg::fsr_foveation {
using Field=std::array<float,4>;
using Map=std::array<float,4>; // source UV -> destination UV: xy offset, zw scale
inline bool valid(const Field& f) noexcept {
    for(float x:f)if(!std::isfinite(x))return false;
    return f[1]>f[0]&&f[2]>f[3];
}
inline Map mapping(const Field& source,const Field& target) noexcept {
    return {(source[0]-target[0])/(target[1]-target[0]),
        (target[2]-source[2])/(target[2]-target[3]),
        (source[1]-source[0])/(target[1]-target[0]),
        (source[2]-source[3])/(target[2]-target[3])};
}
inline bool covers(const Map& m,float tolerance=0.0001F) noexcept {
    return m[0]<=tolerance&&m[1]<=tolerance&&m[0]+m[2]>=1-tolerance&&m[1]+m[3]>=1-tolerance;
}
inline bool inset(const Map& m) noexcept {
    return m[2]>0&&m[3]>0&&m[2]<1&&m[3]<1&&
        m[0]>=-0.0001F&&m[1]>=-0.0001F&&m[0]+m[2]<=1.0001F&&m[1]+m[3]<=1.0001F;
}
inline std::array<float,2> motion(const Map& current,const Map& previous,
    std::array<float,2> source_uv,std::array<float,2> source_motion) noexcept {
    return {previous[0]+(source_uv[0]+source_motion[0])*previous[2]-(current[0]+source_uv[0]*current[2]),
        previous[1]+(source_uv[1]+source_motion[1])*previous[3]-(current[1]+source_uv[1]*current[3])};
}
}
