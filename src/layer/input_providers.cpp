#include "xrfg/input_providers.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/fsr_frame_generation.hpp"
#include "xrfg/native_ingress.hpp"
#include "xrfg/native_ingress/streamline_camera.hpp"
#include "xrfg/autonomous_control.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <vector>

namespace xrfg {
namespace {
struct Provider { HMODULE module{}; OFXR_InputProviderV1 api{}; };
struct Registry { std::once_flag initialized; std::atomic_bool ready{}; std::vector<Provider> providers; };
Registry& registry() { static auto* value = new Registry; return *value; }

bool valid_camera(const OFXR_CameraV3& c) noexcept {
    for (auto* vector : {c.position, c.right, c.up, c.forward})
        for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(vector[i])) return false;
    const auto dot = [](const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    for (auto* axis : {c.right, c.up, c.forward})
        if (std::abs(dot(axis, axis)-1.F) > .05F) return false;
    if (std::abs(dot(c.right,c.up)) > .05F || std::abs(dot(c.right,c.forward)) > .05F ||
        std::abs(dot(c.up,c.forward)) > .05F) return false;
    if (c.matrices_valid) {
        for (unsigned i = 0; i < 16; ++i)
            if (!std::isfinite(c.view_to_clip[i]) || !std::isfinite(c.clip_to_view[i])) return false;
        if (c.view_to_clip[0] == 0 || c.view_to_clip[5] == 0) return false;
    }
    return c.source_view_mask == 0 || c.source_view_mask == 3 || c.source_view_mask == 15;
}

void camera_to_publication(const OFXR_CameraV3& c, DlssMotionVectorPublication& p) noexcept {
    std::copy_n(c.position,3,p.camera_position.begin()); std::copy_n(c.right,3,p.camera_right.begin());
    std::copy_n(c.up,3,p.camera_up.begin()); std::copy_n(c.forward,3,p.camera_forward.begin());
    std::copy_n(c.view_to_clip,16,p.camera_view_to_clip.begin());
    std::copy_n(c.clip_to_view,16,p.camera_clip_to_view.begin());
    p.camera_data_valid=true; p.camera_matrices_valid=c.matrices_valid!=0;
    p.source_view_mask=c.source_view_mask;
    p.camera_frame_token=c.frame_token; p.camera_frame_token_valid=c.frame_token_valid!=0;
}

int publish(const OFXR_GuidePublicationV3* in, OFXR_GuideReceiptV3* out) noexcept {
    if (!in || in->struct_size < sizeof(*in) || in->api_version != OFXR_GUIDE_ABI ||
        !out || out->struct_size < sizeof(*out) ||
        (in->camera_data_valid && !valid_camera(in->camera))) return 0;
    try {
        DlssMotionVectorPublication p{};
        p.stream=in->stream; p.output=in->output; p.motion_vectors=in->motion_vectors; p.depth=in->depth;
        p.producer_queue=in->producer_queue; p.producer_command_list=in->producer_command_list;
        p.output_x=in->output_x; p.output_y=in->output_y; p.output_width=in->output_width; p.output_height=in->output_height;
        p.motion_x=in->motion_x; p.motion_y=in->motion_y; p.motion_width=in->motion_width; p.motion_height=in->motion_height;
        p.depth_x=in->depth_x; p.depth_y=in->depth_y; p.depth_width=in->depth_width; p.depth_height=in->depth_height;
        p.scale_x=in->scale_x; p.scale_y=in->scale_y; p.jitter_x=in->jitter_x; p.jitter_y=in->jitter_y;
        p.resource_state=static_cast<D3D12_RESOURCE_STATES>(in->resource_state);
        p.depth_resource_state=static_cast<D3D12_RESOURCE_STATES>(in->depth_resource_state);
        p.frame_time_delta_ms=in->frame_time_delta_ms; p.camera_near=in->camera_near; p.camera_far=in->camera_far;
        p.camera_fov_vertical=in->camera_fov_vertical; p.view_space_to_meters=in->view_space_to_meters;
        p.jittered=in->jittered!=0; p.reset=in->reset!=0; p.depth_inverted=in->depth_inverted!=0;
        p.depth_infinite=in->depth_infinite!=0; p.camera_pending=in->camera_pending!=0;
        p.fsr_upscale_source=in->fsr_upscale_source!=0;
        p.direct_output_receipt=in->direct_output_receipt!=0;
        if(in->depth_source_subresource!=OFXR_WHOLE_DEPTH_RESOURCE)p.depth_source_subresource=in->depth_source_subresource;
        if(in->camera_data_valid)camera_to_publication(in->camera,p);
        native_ingress::observe_guide_list(p.producer_command_list);
        const auto frame=publish_dlss_motion_vectors(p);
        if(!frame)return 0;
        *out={};out->publication=frame->publication;out->serial=frame->serial;
        out->previous_serial=frame->previous_serial;out->epoch=frame->epoch;
        return 1;
    } catch (...) { return 0; }
}

int attach_camera(std::uint64_t stream,std::uint64_t publication,const OFXR_CameraV3* in) noexcept {
    if(!in || !in->frame_token_valid || !valid_camera(*in))return 0;
    DlssCameraAttachment c{};
    std::copy_n(in->position,3,c.position.begin());std::copy_n(in->right,3,c.right.begin());
    std::copy_n(in->up,3,c.up.begin());std::copy_n(in->forward,3,c.forward.begin());
    std::copy_n(in->view_to_clip,16,c.view_to_clip.begin());std::copy_n(in->clip_to_view,16,c.clip_to_view.begin());
    c.near_z=in->near_z;c.far_z=in->far_z;c.fov_vertical=in->fov_vertical;c.world=in->world;
    return attach_fsr_scene_camera(stream,publication,c,in->frame_token,in->matrices_valid!=0,in->source_view_mask)?1:0;
}

int query_camera(const OFXR_GuidePublicationV3* in,OFXR_CameraV3* out) noexcept {
    if(!in || !out || in->struct_size<sizeof(*in) || in->api_version!=OFXR_GUIDE_ABI)return 0;
    try {
        DlssMotionVectorPublication p{};
        p.producer_command_list=in->producer_command_list;
        if(native_ingress::apply_camera(p,in->producer_command_list)) {
            std::copy(p.camera_position.begin(),p.camera_position.end(),out->position);
            std::copy(p.camera_right.begin(),p.camera_right.end(),out->right);
            std::copy(p.camera_up.begin(),p.camera_up.end(),out->up);
            std::copy(p.camera_forward.begin(),p.camera_forward.end(),out->forward);
            std::copy(p.camera_view_to_clip.begin(),p.camera_view_to_clip.end(),out->view_to_clip);
            std::copy(p.camera_clip_to_view.begin(),p.camera_clip_to_view.end(),out->clip_to_view);
            out->matrices_valid=p.camera_matrices_valid;out->frame_token=p.camera_frame_token;
            out->frame_token_valid=p.camera_frame_token_valid;
            return valid_camera(*out)?1:0;
        }
        const auto c=native_ingress::streamline_camera::take_fsr_camera(in->producer_command_list,in->depth,
            in->motion_vectors,in->depth_width,in->depth_height,in->motion_width,in->motion_height);
        if(!c.valid || c.inverted!=(in->depth_inverted!=0))return 0;
        std::copy(c.position.begin(),c.position.end(),out->position);std::copy(c.right.begin(),c.right.end(),out->right);
        std::copy(c.up.begin(),c.up.end(),out->up);std::copy(c.forward.begin(),c.forward.end(),out->forward);
        std::copy(c.projection.begin(),c.projection.end(),out->view_to_clip);std::copy(c.inverse.begin(),c.inverse.end(),out->clip_to_view);
        out->frame_token=c.frame;out->frame_token_valid=out->matrices_valid=1;
        out->near_z=c.near_z;out->far_z=c.far_z;out->fov_vertical=c.fov;
        return valid_camera(*out)?1:0;
    }catch(...){return 0;}
}

int get_status(OFXR_ProviderStatusV1* out) noexcept {
    if(!out || out->struct_size<sizeof(*out))return 0;
    try {
        const auto s=autonomous::snapshot();const auto& r=s.runtime;const auto& t=r.telemetry;
        const auto f=fsr_frame_generation_statistics();const auto p=dlss_snapshot_memory();
        *out={};out->enabled=s.desired.enabled;out->frame_generation=s.desired.frame_generation;
        out->afw_only=s.desired.afw_only;out->logging_enabled=s.desired.diagnostics;out->connected=s.connected;
        out->revision=s.revision;out->applied_revision=r.applied_revision;out->heartbeat_ms=r.heartbeat_ms;
        out->generation_state=unsigned(r.generation_state);out->logging_active=r.logging_active;
        out->fg_status=unsigned(f.status);out->fg_attempts=f.attempts;out->fg_dispatches=f.used;out->fg_failures=f.failures;
        out->guide_publications=r.guide_publications;out->guide_matches=r.guide_matches;out->guide_uses=r.guide_uses;
        out->telemetry_tick_ms=t.updated_ms;out->rates_valid=t.rates_valid;out->gpu_valid=t.gpu_valid;
        out->gpu_source=unsigned(t.gpu_source);out->stages_valid=t.stages_valid;out->game_time_valid=t.game_time_valid;
        out->ofxr_time_valid=t.ofxr_time_valid;out->game_fps=t.game_fps;out->generated_fps=t.generated_fps;
        out->output_fps=t.output_fps;out->display_hz=t.display_hz;out->gpu_render_ms=t.gpu_render_ms;
        out->game_time_ms=t.game_time_ms;out->generation_time_ms=t.generation_time_ms;
        out->delivery_time_ms=t.delivery_time_ms;out->ofxr_time_ms=t.ofxr_time_ms;
        out->snapshot_memory_valid=p.valid;out->snapshot_slots=static_cast<unsigned>(p.slots);
        out->retired_snapshot_slots=static_cast<unsigned>(p.retired_slots);out->snapshot_bytes=p.bytes;
        return 1;
    }catch(...){return 0;}
}
}

// Optional dynamically resolved entry point for an already-loaded provider.
// Returning the table does not register or start any capture hooks.
extern "C" __declspec(dllexport) const OFXR_InputProviderHostV1*
OFXR_GetInputProviderHostV1(std::uint32_t abi) noexcept {
    return abi==OFXR_INPUT_PROVIDER_ABI?&xrfg::input_provider_host():nullptr;
}

const OFXR_InputProviderHostV1& input_provider_host() noexcept {
    static const OFXR_InputProviderHostV1 api{
        sizeof(OFXR_InputProviderHostV1),OFXR_INPUT_PROVIDER_ABI,&publish,&attach_camera,
        &invalidate_dlss_motion_vector_stream_history,&retire_dlss_motion_vector_stream,
        &native_ingress::observe_guide_list,&query_camera,&get_status,&fsr_diagnostic};
    return api;
}

void prepare_input_providers(const wchar_t* directory) noexcept {
    if(!directory || !*directory)return;
    try {
        std::call_once(registry().initialized,[&] {
            const auto folder=std::filesystem::path(directory)/L"providers";
            std::error_code error;
            if(!std::filesystem::is_directory(folder,error))return;
            std::vector<std::filesystem::path> paths;
            for(const auto& entry:std::filesystem::directory_iterator(folder))
                if(entry.is_regular_file() && entry.path().filename().wstring().ends_with(L".ofxr-provider.dll"))paths.push_back(entry.path());
            std::sort(paths.begin(),paths.end());
            for(const auto& path:paths) {
                if(registry().providers.size()==8)break;
                Provider provider;provider.module=LoadLibraryExW(path.c_str(),nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if(!provider.module)continue;
                const auto initialize=reinterpret_cast<PFN_OFXR_InitializeInputProviderV1>(
                    GetProcAddress(provider.module,"OFXR_InitializeInputProviderV1"));
                if(!initialize || !initialize(&input_provider_host(),&provider.api) ||
                    provider.api.abi_version!=OFXR_INPUT_PROVIDER_ABI || provider.api.struct_size<sizeof(provider.api)) {
                    // An initializer returning success may have installed detours.
                    // Keep its module alive even if it returned an invalid table.
                    // Modules are intentionally never unloaded during the process.
                    continue;
                }
                registry().providers.push_back(provider);
            }
            set_fsr_background_diagnostics(!registry().providers.empty());
            registry().ready.store(true,std::memory_order_release);
            native_ingress::set_fsr_retire_callback(&retire_provider_recording);
            native_ingress::set_fsr_submission_callback(&report_provider_submission);
        });
    }catch(...){fsr_diagnostic("provider_load_failed");}
}
void poll_input_providers() noexcept {
    // Initialization can also poll before NGX exists. Serialize that with the
    // monitor so providers may keep their background worker state local.
    static std::mutex mutex;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock())return;
    static std::uint64_t flushed{};
    const auto now=GetTickCount64();
    if(now-flushed>=1000) {
        flushed=now;
        try {flush_fsr_background_diagnostics(autonomous::global_configuration_path().parent_path());}catch(...){}
    }
    if(!registry().ready.load(std::memory_order_acquire))return;
    for(const auto& p:registry().providers)if(p.api.poll)p.api.poll();
}
void configure_input_providers(ID3D12CommandQueue* queue,bool enabled) noexcept {
    if(!registry().ready.load(std::memory_order_acquire))return;
    native_ingress::streamline_camera::enable_fsr_tags(enabled && queue && !registry().providers.empty());
    if(!registry().ready.load(std::memory_order_acquire))return;
    for(const auto& p:registry().providers)if(p.api.configure)p.api.configure(queue,enabled?1:0);
}
void stop_input_providers(ID3D12CommandQueue* queue) noexcept {
    if(!registry().ready.load(std::memory_order_acquire))return;
    for(const auto& p:registry().providers)if(p.api.stop_queue)p.api.stop_queue(queue);
    native_ingress::streamline_camera::enable_fsr_tags(false);
}
void retire_provider_recording(ID3D12GraphicsCommandList* list,bool submitting) noexcept {
    if(!registry().ready.load(std::memory_order_acquire))return;
    for(const auto& p:registry().providers)if(p.api.retire_recording)p.api.retire_recording(list,submitting?1:0);
}
void report_provider_submission(ID3D12CommandQueue* queue,unsigned count,ID3D12CommandList* const* original,ID3D12CommandList* const* native) noexcept {
    if(!registry().ready.load(std::memory_order_acquire))return;
    for(const auto& p:registry().providers)if(p.api.submitted)p.api.submitted(queue,count,original,native);
}
void report_provider_projection(const OFXR_ProjectionReceiptV1& receipt) noexcept {
    if(!registry().ready.load(std::memory_order_acquire))return;
    for(const auto& p:registry().providers)if(p.api.projection)p.api.projection(&receipt);
}
}
