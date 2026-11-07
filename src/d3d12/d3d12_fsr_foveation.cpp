#include "xrfg/d3d12_fsr_foveation.hpp"
#include "xrfg/fsr_foveation.hpp"
#include "d3d12_fsr_frame_generator.hpp"
#include "xrfg/fsr_frame_generation.hpp"
#include "fsr_foveation_shader.hpp"
#include <algorithm>
#include <cmath>
namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
struct Params {
    fsr_foveation::Map current[2],previous[2];
    std::array<float,4> scale[2],jitter[2];
    UINT size[2],padding[2]{};
};
static_assert(sizeof(Params)==36*4);
fsr_foveation::Field field(const DlssMotionVectorFrame& f) {
    auto g=dlss_view_geometry(f);return {g.tan_left,g.tan_right,g.tan_up,g.tan_down};
}
fsr_foveation::Field field(const D3D12FieldOfView& f) {
    return {std::tan(f.angle_left),std::tan(f.angle_right),std::tan(f.angle_up),std::tan(f.angle_down)};
}
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
}
bool compatible(const DlssMotionVectorFrame& a,const DlssMotionVectorFrame& b) {
    return a.stream==b.stream&&a.epoch==b.epoch&&a.serial==b.serial+1&&
        a.source_view_mask==b.source_view_mask&&
        a.camera_frame_token_valid&&b.camera_frame_token_valid&&
        uint32_t(a.camera_frame_token-b.camera_frame_token)==1&&
        a.depth_width==b.depth_width&&a.depth_height==b.depth_height&&
        a.motion_width==b.motion_width&&a.motion_height==b.motion_height&&
        a.depth_inverted==b.depth_inverted&&a.depth_infinite==b.depth_infinite&&
        a.camera_near==b.camera_near&&a.camera_far==b.camera_far&&
        a.jittered==b.jittered&&a.view_space_to_meters==b.view_space_to_meters&&!a.reset&&!b.reset;
}
}
struct D3D12FsrFoveation::Scratch {
    struct Eye {ComPtr<ID3D12Resource> depth,motion;UINT width{},height{};};
    ComPtr<ID3D12Device> device;
    std::array<Eye,2> eyes;
};
struct D3D12FsrFoveation::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;UINT stride{};
    std::shared_ptr<Scratch> scratch;
    // Pin the allocation referenced by this slot's descriptors even when a
    // later recording replaces the shared allocation after a resolution change.
    std::array<Scratch::Eye,2> eyes;
    HRESULT initialize(ID3D12GraphicsCommandList* list) {
        if(root)return S_OK;
        HRESULT hr=list->GetDevice(IID_PPV_ARGS(&device));if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0}};
        D3D12_ROOT_PARAMETER params[3]{};
        for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,sizeof(Params)/4};
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=3;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);if(FAILED(hr))return hr;
        ComPtr<ID3D12RootSignature> candidate;
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&candidate));if(FAILED(hr))return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=candidate.Get();pso.CS={g_xrfg_fsr_foveation_shader,sizeof(g_xrfg_fsr_foveation_shader)};
        hr=device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline));if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=12;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap));if(FAILED(hr))return hr;
        stride=device->GetDescriptorHandleIncrementSize(hd.Type);root=std::move(candidate);return S_OK;
    }
    HRESULT textures(unsigned eye,UINT w,UINT h) {
        if(scratch->device && scratch->device.Get()!=device.Get())return E_INVALIDARG;
        scratch->device=device;
        auto& e=scratch->eyes[eye];
        if(e.depth&&e.motion&&e.width==w&&e.height==h)return S_OK;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Scratch::Eye next;next.width=w;next.height=h;d.Format=DXGI_FORMAT_R32_FLOAT;
        auto hr=device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&next.depth));if(FAILED(hr))return hr;
        d.Format=DXGI_FORMAT_R32G32_FLOAT;
        hr=device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&next.motion));if(FAILED(hr))return hr;
        UINT64 bytes{};
        for(auto* resource:{next.depth.Get(),next.motion.Get()}) {
            const auto description=resource->GetDesc();
            bytes+=device->GetResourceAllocationInfo(0,1,&description).SizeInBytes;
        }
        fsr_diagnostic("fsr_perf_scratch_memory",reinterpret_cast<UINT64>(scratch.get()),eye,
            bytes,(UINT64(w)<<32)|h);
        e=std::move(next);return S_OK;
    }
};
std::shared_ptr<D3D12FsrFoveation::Scratch> D3D12FsrFoveation::make_scratch() {
    return std::make_shared<Scratch>();
}
D3D12FsrFoveation::D3D12FsrFoveation(std::shared_ptr<Scratch> scratch):impl_(std::make_unique<Impl>()) {
    impl_->scratch=scratch?std::move(scratch):make_scratch();
}
D3D12FsrFoveation::~D3D12FsrFoveation()=default;
HRESULT D3D12FsrFoveation::convert(ID3D12GraphicsCommandList* list,unsigned eye,
    const DlssMotionVectorFrame& c,const DlssMotionVectorFrame& old,
    const D3D12FieldOfView& fov,const D3D12FieldOfView& previous_fov,D3D12FsrEyeInput& input) noexcept {
    try {
        const bool stereo=c.source_view_mask==3 && old.source_view_mask==3;
        if(!list||eye>=2||!compatible(c,old))return E_INVALIDARG;
        if(stereo) {if(c.fsr_inset||old.fsr_inset)return E_INVALIDARG;}
        else {
            if(!c.fsr_inset||!old.fsr_inset||!compatible(*c.fsr_inset,*old.fsr_inset))return E_INVALIDARG;
            if(c.camera_frame_token!=c.fsr_inset->camera_frame_token||old.camera_frame_token!=old.fsr_inset->camera_frame_token)return E_INVALIDARG;
        }
        if(!input.color_rect.width||!input.color_rect.height)return E_INVALIDARG;
        const auto target=field(fov),previous_target=field(previous_fov);
        if(!fsr_foveation::valid(target)||!fsr_foveation::valid(previous_target))return E_INVALIDARG;
        Params p{};p.padding[0]=stereo?0:1; // HasInset in the shader
        std::array<const DlssMotionVectorFrame*,2> current{&c,stereo?&c:c.fsr_inset.get()},previous{&old,stereo?&old:old.fsr_inset.get()};
        float density=0;
        for(unsigned i=0;i<2;++i) {
            const auto& a=*current[i];const auto& b=*previous[i];
            if(!a.depth||!a.motion_vectors||a.motion_x||a.motion_y||a.depth_x||a.depth_y||
                !a.motion_width||!a.motion_height||!a.depth_width||!a.depth_height)return E_INVALIDARG;
            const auto a_field=field(a),b_field=field(b);
            if(!fsr_foveation::valid(a_field)||!fsr_foveation::valid(b_field))return E_INVALIDARG;
            p.current[i]=fsr_foveation::mapping(a_field,target);p.previous[i]=fsr_foveation::mapping(b_field,previous_target);
            if(i==0&&(!fsr_foveation::covers(p.current[i])||!fsr_foveation::covers(p.previous[i])))return E_INVALIDARG;
            if(i==1&&!stereo&&(!fsr_foveation::inset(p.current[i])||!fsr_foveation::inset(p.previous[i])))return E_INVALIDARG;
            p.scale[i]={a.scale_x/a.motion_width,a.scale_y/a.motion_height,
                a.jittered?(b.jitter_x-a.jitter_x)/a.motion_width:0,a.jittered?(b.jitter_y-a.jitter_y)/a.motion_height:0};
            p.jitter[i]={a.jitter_x/a.depth_width,a.jitter_y/a.depth_height,0,0};
            if(a.motion_width==a.depth_width&&a.motion_height==a.depth_height){p.jitter[i][2]=p.jitter[i][0];p.jitter[i][3]=p.jitter[i][1];}
            density=std::max({density,a.depth_width/(p.current[i][2]*input.color_rect.width),a.depth_height/(p.current[i][3]*input.color_rect.height)});
        }
        if(!std::isfinite(density)||density<=0||!input.color_rect.width||!input.color_rect.height)return E_INVALIDARG;
        density=std::min(density,1.F);
        // Quantize a density, not independent dimensions: maintain aspect and
        // avoid context churn from tiny floating-point projection differences.
        density=std::min(1.F,std::ceil(density*64.F-0.001F)/64.F);
        p.size[0]=std::max(1u,UINT(std::ceil(input.color_rect.width*density)));
        p.size[1]=std::max(1u,UINT(std::ceil(input.color_rect.height*density)));
        auto& s=*impl_;auto hr=s.initialize(list);if(FAILED(hr))return hr;
        hr=s.textures(eye,p.size[0],p.size[1]);if(FAILED(hr))return hr;
        auto& e=s.eyes[eye];e=s.scratch->eyes[eye];
        auto cpu=s.heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(eye*6)*s.stride;
        std::array<D3D12_RESOURCE_BARRIER,6> barriers{};unsigned count{};
        const auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
            if(before!=after)barriers[count++]=barrier(resource,before,after);
        };
        for(unsigned i=0;i<2;++i)for(unsigned kind=0;kind<2;++kind) {
            auto* resource=kind?current[i]->motion_vectors.Get():current[i]->depth.Get();const auto d=resource->GetDesc();
            if(d.DepthOrArraySize!=1||d.MipLevels!=1)return E_INVALIDARG;
            if(d.Width!=(kind?current[i]->motion_width:current[i]->depth_width)||
                d.Height!=(kind?current[i]->motion_height:current[i]->depth_height))return E_INVALIDARG;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Format=d.Format==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_R32_FLOAT:d.Format;
            s.device->CreateShaderResourceView(resource,&srv,cpu);cpu.ptr+=s.stride;
            // Stereo binds valid duplicate descriptors for the unused inset
            // slots, but transitions each real resource exactly once.
            if(i==0||!stereo)transition(resource,kind?current[i]->resource_state:current[i]->depth_resource_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        for(auto* resource:{e.depth.Get(),e.motion.Get()}) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;uav.Format=resource->GetDesc().Format;
            s.device->CreateUnorderedAccessView(resource,nullptr,&uav,cpu);cpu.ptr+=s.stride;
            transition(resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        list->ResourceBarrier(count,barriers.data());ID3D12DescriptorHeap* heaps[]{s.heap.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootSignature(s.root.Get());list->SetPipelineState(s.pipeline.Get());
        auto gpu=s.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(eye*6)*s.stride;list->SetComputeRootDescriptorTable(0,gpu);
        gpu.ptr+=UINT64(4)*s.stride;list->SetComputeRootDescriptorTable(1,gpu);
        list->SetComputeRoot32BitConstants(2,sizeof(Params)/4,&p,0);list->Dispatch((p.size[0]+7)/8,(p.size[1]+7)/8,1);
        // Return to original/COMMON states even if the enclosing recording is
        // later discarded; no speculative resource-state bookkeeping survives.
        for(unsigned i=0;i<count;++i)std::swap(barriers[i].Transition.StateBefore,barriers[i].Transition.StateAfter);
        list->ResourceBarrier(count,barriers.data());
        input.depth=e.depth.Get();input.motion_vectors=e.motion.Get();input.depth_state=input.motion_state=D3D12_RESOURCE_STATE_COMMON;
        input.depth_rect=input.motion_rect={0,0,p.size[0],p.size[1]};input.motion_scale_x=float(p.size[0]);input.motion_scale_y=float(p.size[1]);
        input.jitter_x=input.jitter_y=0;input.motion_vectors_jittered=false;
        input.borrow_owned_guides=true;
        input.camera_fov_vertical=fov.angle_up-fov.angle_down;
        return S_OK;
    }catch(...) {return E_FAIL;}
}
}
