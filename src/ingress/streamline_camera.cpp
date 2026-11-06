#include "xrfg/native_ingress/streamline_camera.hpp"
#include "xrfg/native_ingress/DeadSpaceCameraSource.h"
#include "xrfg/d3d12_native_binding.hpp"
#include <sl.h>
#include "xrfg/native_ingress/streamline1/sl1.h"
#include <safetyhook.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <atomic>
namespace xrfg::native_ingress::streamline_camera {
namespace {
using Microsoft::WRL::ComPtr;
struct TaggedCamera {
    ComPtr<IUnknown> list, depth, motion;
    sl::Extent depth_rect{}, motion_rect{};
    uint32_t frame{}, viewport{};
    bool used{};
};
struct Hooks {
    std::mutex mutex;
    std::array<Camera,16> cameras{};
    std::size_t cursor{};
    SafetyHookInline constants,evaluate,tags;
    PFun_slSetTagForFrame* tag{};
    std::array<TaggedCamera,32> tagged{};
    std::size_t tag_cursor{};
    bool fsr_enabled{};
    FsrCameraStatistics stats{};
    PFun_slSetConstants* set{};
    PFun_slEvaluateFeature* eval{};
    // Exact declarations from NVIDIA Streamline 1.3, retained by shared OptiScaler.
    using Set1=decltype(&sl1::slSetConstants);
    using Eval1=decltype(&sl1::slEvaluateFeature);
    Set1 set1{};
    Eval1 eval1{};
};
Hooks& hooks(){static auto* value=new Hooks;return *value;}
thread_local Camera production;
std::atomic_bool fsr_tags_enabled{};
template<std::size_t N> bool finite(const std::array<float,N>& values) {
    for(float v:values)if(!std::isfinite(v))return false;
    return true;
}
bool basis(const Camera& c) {
    const auto dot=[](auto& a,auto& b){float n{};for(unsigned i=0;i<3;++i)n+=a[i]*b[i];return n;};
    return finite(c.up)&&finite(c.right)&&finite(c.forward)&&
        std::abs(dot(c.up,c.up)-1)<.05F&&std::abs(dot(c.right,c.right)-1)<.05F&&
        std::abs(dot(c.forward,c.forward)-1)<.05F&&std::abs(dot(c.right,c.up))<.05F&&
        std::abs(dot(c.up,c.forward))<.05F&&std::abs(dot(c.forward,c.right))<.05F;
}
template<class Constants> Camera decode(const Constants& v,uint32_t frame,uint32_t viewport) {
    Camera c{};c.frame=frame;c.viewport=viewport;c.observed=true;
    std::copy_n(reinterpret_cast<const float*>(&v.cameraViewToClip),16,c.projection.begin());
    std::copy_n(reinterpret_cast<const float*>(&v.clipToCameraView),16,c.inverse.begin());
    std::copy_n(reinterpret_cast<const float*>(&v.clipToPrevClip),16,c.previous.begin());
    std::copy_n(reinterpret_cast<const float*>(&v.prevClipToClip),16,c.to_current.begin());
    std::copy_n(reinterpret_cast<const float*>(&v.cameraPos),3,c.position.begin());
    std::copy_n(reinterpret_cast<const float*>(&v.cameraUp),3,c.up.begin());
    std::copy_n(reinterpret_cast<const float*>(&v.cameraRight),3,c.right.begin());
    std::copy_n(reinterpret_cast<const float*>(&v.cameraFwd),3,c.forward.begin());
    c.near_z=v.cameraNear;c.far_z=v.cameraFar;c.fov=v.cameraFOV;
    c.inverted=static_cast<int>(v.depthInverted)==1;
    c.camera_motion=static_cast<int>(v.cameraMotionIncluded)==1?1:
        static_cast<int>(v.cameraMotionIncluded)==0?0:-1;
    c.rejection=(!finite(c.projection)?1U:0U)|(!finite(c.inverse)?2U:0U)|
        (!finite(c.position)?4U:0U)|(!basis(c)?8U:0U)|
        (!(std::isfinite(c.near_z)&&c.near_z>0)?16U:0U)|
        (!(std::isfinite(c.far_z)&&(c.far_z==0||c.far_z>c.near_z))?32U:0U)|
        (!(std::isfinite(c.fov)&&c.fov>0&&c.fov<3.1416F)?64U:0U);
    c.valid=c.rejection==0;
    return c;
}
void remember(const Camera& c) {
    auto& s=hooks();std::scoped_lock lock(s.mutex);
    ++s.stats.constants;
    auto it=std::find_if(s.cameras.begin(),s.cameras.end(),[&](const auto& entry){
        return entry.observed&&entry.frame==c.frame&&entry.viewport==c.viewport;});
    if(it==s.cameras.end())it=s.cameras.begin()+(s.cursor++%s.cameras.size());
    *it=c;
}
ComPtr<IUnknown> identity(IUnknown* value) {
    IUnknown* native{};ComPtr<IUnknown> result;
    if(value&&d3d_native::get_borrowed(value,&native))native->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
sl::Result tag_for_frame(const sl::FrameToken& frame,const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* buffer) {
    const auto result=hooks().tag(frame,viewport,tags,count,buffer);
    try {
        auto& s=hooks();std::scoped_lock lock(s.mutex);
        if(!s.fsr_enabled)return result;
        ++s.stats.tags;
        const auto f=uint32_t(frame),v=uint32_t(viewport);
        // Failed/null/volatile retags invalidate this viewport's old receipt.
        for(auto& t:s.tagged)if(t.viewport==v&&t.frame!=f)t={};
        if(result!=sl::Result::eOk||!buffer||!tags||count>64) {
            for(auto& t:s.tagged)if(t.viewport==v)t={};return result;
        }
        ComPtr<ID3D12GraphicsCommandList> list;
        if(FAILED(reinterpret_cast<IUnknown*>(buffer)->QueryInterface(IID_PPV_ARGS(&list))))return result;
        const auto key=identity(list.Get());if(!key)return result;
        auto it=std::find_if(s.tagged.begin(),s.tagged.end(),[&](const auto& t){
            return t.list==key&&t.frame==f&&t.viewport==v;});
        if(it==s.tagged.end()) {it=s.tagged.begin()+(s.tag_cursor++%s.tagged.size());*it={};it->list=key;it->frame=f;it->viewport=v;}
        // A consumed frame cannot be resurrected by retagging recycled textures.
        if(it->used)return result;
        for(uint32_t i=0;i<count;++i) {
            const auto& t=tags[i];
            if(t.type!=sl::kBufferTypeDepth&&t.type!=sl::kBufferTypeMotionVectors)continue;
            ComPtr<IUnknown> resource;
            if(t.resource&&t.resource->native&&t.resource->type==sl::ResourceType::eTex2d&&
                (t.lifecycle==sl::ResourceLifecycle::eValidUntilPresent||t.lifecycle==sl::ResourceLifecycle::eValidUntilEvaluate)) {
                ComPtr<ID3D12Resource> r;
                if(SUCCEEDED(reinterpret_cast<IUnknown*>(t.resource->native)->QueryInterface(IID_PPV_ARGS(&r))))resource=identity(r.Get());
            }
            if(t.type==sl::kBufferTypeDepth){it->depth=resource;it->depth_rect=t.extent;}
            else{it->motion=resource;it->motion_rect=t.extent;}
        }
    }catch(...) {}
    return result;
}
sl::Result set_constants(const sl::Constants& v,const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport) {
    auto c=decode(v,static_cast<uint32_t>(frame),static_cast<uint32_t>(viewport));
    const auto result=hooks().set(v,frame,viewport);
    if(result!=sl::Result::eOk){c.valid=false;c.rejection|=512;}remember(c);
    return result;
}
bool set_constants1(const sl1::Constants& v,uint32_t frame,uint32_t viewport) {
    dead_space_camera_source::poll(true);
    auto c=decode(v,frame,viewport);
    const auto result=hooks().set1(v,frame,viewport);
    if(result){static_cast<void>(dead_space_camera_source::correct(c));remember(c);}
    return result;
}
Camera select(uint32_t frame,bool has_viewport,uint32_t viewport,bool ambiguous=false) {
    Camera selected{};selected.frame=frame;selected.viewport=viewport;selected.rejection=512;selected.ambiguous=ambiguous;
    auto& s=hooks();std::scoped_lock lock(s.mutex);
    for(const auto& c:s.cameras)if(c.valid&&c.frame==frame&&(!has_viewport||c.viewport==viewport)) {
        if(selected.valid){selected.ambiguous=true;break;}
        const bool ambiguity=selected.ambiguous;selected=c;selected.ambiguous|=ambiguity;
    }
    // Preserve the unique exact rejected packet only to report its original
    // field bits. It remains invalid and cannot authorize capture or AFW.
    if(!selected.valid&&!selected.ambiguous)for(const auto& c:s.cameras)
        if(c.observed&&!c.valid&&c.frame==frame&&(!has_viewport||c.viewport==viewport)) {
            if(selected.observed){selected.ambiguous=true;break;}
            selected=c;
        }
    if(selected.ambiguous){selected.valid=false;selected.rejection|=1024;}
    return selected;
}
struct Restore {Camera previous;~Restore(){production=previous;}};
bool projection_fov(Camera& camera) noexcept {
    const auto& p=camera.projection;const auto& inverse=camera.inverse;
    if(!camera.observed||camera.ambiguous||camera.rejection!=64||!finite(p)||!finite(inverse))return false;
    // Accepted OptiScaler derives vertical FOV from projection[1][1].
    // Require a normalized perspective (both SDK matrix orientations), not
    // an orthographic/identity matrix or an unrelated finite inverse.
    constexpr float epsilon=1e-4F;
    if(!(p[0]>0&&p[5]>0)||std::abs(p[15])>epsilon||
        std::abs(p[1])>epsilon||std::abs(p[4])>epsilon||
        std::abs(p[3])>epsilon||std::abs(p[7])>epsilon||
        std::abs(p[12])>epsilon||std::abs(p[13])>epsilon||
        !(std::abs(std::abs(p[11])-1)<epsilon||std::abs(std::abs(p[14])-1)<epsilon))return false;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c) {
        double a{},b{};for(unsigned k=0;k<4;++k) {
            a+=double(p[r*4+k])*inverse[k*4+c];b+=double(inverse[r*4+k])*p[k*4+c];
        }
        const double expected=r==c?1:0;
        if(std::abs(a-expected)>.002||std::abs(b-expected)>.002)return false;
    }
    const float fov=2.F*std::atan(1.F/p[5]);
    if(!(std::isfinite(fov)&&fov>0&&fov<3.1416F))return false;
    camera.fov=fov;camera.rejection=0;camera.valid=true;return true;
}
bool evaluate1(sl1::CommandBuffer* list,sl1::Feature feature,uint32_t frame,uint32_t viewport) {
    Restore restore{production};production=select(frame,true,viewport);
    return hooks().eval1(list,feature,frame,viewport);
}
sl::Result evaluate(sl::Feature feature,const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,uint32_t count,sl::CommandBuffer* list) {
    Camera selected{};bool has_viewport=false;uint32_t viewport{};
    if(inputs)for(uint32_t i=0;i<count;++i)if(inputs[i]&&
        inputs[i]->structType==sl::ViewportHandle::s_structType) {
        auto next=static_cast<uint32_t>(*static_cast<const sl::ViewportHandle*>(inputs[i]));
        if(has_viewport&&next!=viewport)selected.ambiguous=true;
        has_viewport=true;viewport=next;
    }
    selected=select(static_cast<uint32_t>(frame),has_viewport,viewport,selected.ambiguous);
    Restore restore{production};production=selected;
    return hooks().eval(feature,frame,inputs,count,list);
}

// A positive loaded-image version and the 2.x frame-token export must agree.
// Unknown/mismatched ABIs stay unmodified; colour-only optical flow still works.
unsigned major_version(HMODULE module) {
    auto resource=FindResourceW(module,MAKEINTRESOURCEW(VS_VERSION_INFO),MAKEINTRESOURCEW(16));
    auto loaded=resource?LoadResource(module,resource):nullptr;
    auto* bytes=loaded?LockResource(loaded):nullptr;
    VS_FIXEDFILEINFO* info{};UINT length{};
    if(!bytes||!VerQueryValueW(bytes,L"\\",reinterpret_cast<void**>(&info),&length)||
        length<sizeof(*info)||info->dwSignature!=0xfeef04bd)return 0;
    return HIWORD(info->dwFileVersionMS);
}
}
Camera current(bool luke_projection_receipt) noexcept {
    auto camera=production;if(luke_projection_receipt)static_cast<void>(projection_fov(camera));return camera;
}
void enable_fsr_tags(bool enabled) noexcept {
    auto& s=hooks();std::scoped_lock lock(s.mutex);
    if(s.fsr_enabled!=enabled)s.tagged={};s.fsr_enabled=enabled;fsr_tags_enabled.store(enabled,std::memory_order_relaxed);
}
void retire_fsr_tags(ID3D12GraphicsCommandList* list) noexcept {
    if(!fsr_tags_enabled.load(std::memory_order_relaxed))return;
    try {const auto key=identity(list);auto& s=hooks();std::scoped_lock lock(s.mutex);
        for(auto& t:s.tagged)if(t.list==key)t={};
    }catch(...) {}
}
Camera take_fsr_camera(ID3D12GraphicsCommandList* list,ID3D12Resource* depth,
    ID3D12Resource* motion,unsigned dw,unsigned dh,unsigned mw,unsigned mh) noexcept {
    try {
        const auto l=identity(list),d=identity(depth),m=identity(motion);
        auto& s=hooks();std::scoped_lock lock(s.mutex);Camera result{};
        if(!s.fsr_enabled||!l||!d||!m)return result;
        const auto fits=[](const sl::Extent& r,unsigned w,unsigned h,ID3D12Resource* resource) {
            if(r.left||r.top)return false;
            if(r.width||r.height)return r.width==w&&r.height==h;
            const auto desc=resource->GetDesc();return desc.Width==w&&desc.Height==h&&desc.DepthOrArraySize==1;
        };
        unsigned matches{};
        for(auto& t:s.tagged)if(!t.used&&t.list==l&&t.depth==d&&t.motion==m&&
            fits(t.depth_rect,dw,dh,depth)&&fits(t.motion_rect,mw,mh,motion)) {
            ++matches;t.used=true;
            for(const auto& c:s.cameras)if(c.observed&&c.frame==t.frame&&c.viewport==t.viewport)result=c;
        }
        if(matches!=1||!result.valid){++s.stats.rejected;return {};}
        ++s.stats.matched;return result;
    }catch(...) {return {};}
}
FsrCameraStatistics fsr_statistics() noexcept {
    auto& s=hooks();std::scoped_lock lock(s.mutex);return s.stats;
}
bool hooked() noexcept {auto& s=hooks();std::scoped_lock lock(s.mutex);return bool(s.constants)&&bool(s.evaluate);}
void poll() noexcept {
    dead_space_camera_source::poll();
    try {
        auto& s=hooks();std::scoped_lock lock(s.mutex);if(s.constants&&s.evaluate)return;
        const auto module=GetModuleHandleW(L"sl.interposer.dll");if(!module)return;
        auto c=GetProcAddress(module,"slSetConstants"),e=GetProcAddress(module,"slEvaluateFeature");
        if(!c||!e)return;
        const unsigned major=major_version(module);
        const bool token_api=GetProcAddress(module,"slGetNewFrameToken")!=nullptr;
        if(!((major==1&&!token_api)||(major==2&&token_api)))return;
        const bool legacy=major==1;
        s.constants=safetyhook::create_inline(c,legacy?reinterpret_cast<void*>(&set_constants1):
            reinterpret_cast<void*>(&set_constants),SafetyHookInline::StartDisabled);
        s.evaluate=safetyhook::create_inline(e,legacy?reinterpret_cast<void*>(&evaluate1):
            reinterpret_cast<void*>(&evaluate),SafetyHookInline::StartDisabled);
        if(!s.constants||!s.evaluate){s.constants={};s.evaluate={};return;}
        if(legacy){s.set1=s.constants.original<Hooks::Set1>();s.eval1=s.evaluate.original<Hooks::Eval1>();}
        else{s.set=s.constants.original<PFun_slSetConstants*>();s.eval=s.evaluate.original<PFun_slEvaluateFeature*>();}
        HMODULE pinned{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(c),&pinned)){s.constants={};s.evaluate={};return;}
        if(!legacy)if(const auto entry=GetProcAddress(module,"slSetTagForFrame")) {
            s.tags=safetyhook::create_inline(entry,reinterpret_cast<void*>(&tag_for_frame),SafetyHookInline::StartDisabled);
            if(s.tags){s.tag=s.tags.original<PFun_slSetTagForFrame*>();if(!s.tags.enable()){s.tags={};s.tag=nullptr;}}
        }
        const auto constants=s.constants.enable();const auto evaluation=s.evaluate.enable();
        if(!constants||!evaluation){s.constants={};s.evaluate={};}
    }catch(...) {}
}
}
namespace xrfg::native_ingress {void poll_streamline() noexcept {streamline_camera::poll();}}
