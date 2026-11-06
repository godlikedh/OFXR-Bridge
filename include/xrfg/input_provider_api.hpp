#pragma once

#include <d3d12.h>
#include <cstdint>
#include <type_traits>

// Process-local, versioned provider ABI. No STL containers, allocation ownership,
// exceptions or C++ objects cross the DLL boundary. All callbacks use the x64
// Windows calling convention and must return without throwing.
inline constexpr std::uint32_t OFXR_INPUT_PROVIDER_ABI = 1;
inline constexpr std::uint32_t OFXR_GUIDE_ABI = 3;
inline constexpr std::uint32_t OFXR_WHOLE_DEPTH_RESOURCE = UINT32_MAX;

struct OFXR_CameraV3 {
    float position[3]{}, right[3]{}, up[3]{}, forward[3]{};
    float view_to_clip[16]{}, clip_to_view[16]{};
    float near_z{}, far_z{}, fov_vertical{};
    std::uint64_t world{};
    std::uint32_t frame_token{}, matrices_valid{}, frame_token_valid{};
    // Supported layouts: 3 = left/right; 15 = peripheral left/right followed
    // by focus left/right. The provider proves each view's native association.
    std::uint32_t source_view_mask{};
};

struct OFXR_GuidePublicationV3 {
    std::uint32_t struct_size{sizeof(OFXR_GuidePublicationV3)}, api_version{OFXR_GUIDE_ABI};
    std::uint64_t stream{};
    ID3D12Resource* output{};
    ID3D12Resource* motion_vectors{};
    ID3D12Resource* depth{};
    ID3D12CommandQueue* producer_queue{};
    ID3D12GraphicsCommandList* producer_command_list{};
    std::uint32_t output_x{}, output_y{}, output_width{}, output_height{};
    std::uint32_t motion_x{}, motion_y{}, motion_width{}, motion_height{};
    std::uint32_t depth_x{}, depth_y{}, depth_width{}, depth_height{};
    float scale_x{1}, scale_y{1}, jitter_x{}, jitter_y{};
    std::uint32_t resource_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    std::uint32_t depth_resource_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    float frame_time_delta_ms{16.6667F}, camera_near{.1F}, camera_far{1000};
    float camera_fov_vertical{}, view_space_to_meters{1};
    std::uint32_t jittered{}, reset{}, depth_inverted{}, depth_infinite{};
    std::uint32_t camera_data_valid{}, camera_pending{}, fsr_upscale_source{}, direct_output_receipt{};
    std::uint32_t depth_source_subresource{OFXR_WHOLE_DEPTH_RESOURCE};
    OFXR_CameraV3 camera{};
};

struct OFXR_GuideReceiptV3 {
    std::uint32_t struct_size{sizeof(OFXR_GuideReceiptV3)};
    std::uint64_t publication{}, serial{}, previous_serial{}, epoch{};
};

struct OFXR_ProjectionReceiptV1 {
    std::uint32_t struct_size{sizeof(OFXR_ProjectionReceiptV1)};
    ID3D12Resource* color{};
    std::uint64_t swapchain{}, eye{}, display_time{}, publication{};
    std::uint32_t slice{}, width{}, height{};
    std::int32_t x{}, y{};
};

struct OFXR_ProviderStatusV1 {
    std::uint32_t struct_size{sizeof(OFXR_ProviderStatusV1)};
    std::uint32_t enabled{}, frame_generation{}, afw_only{}, logging_enabled{}, connected{};
    std::uint64_t revision{}, applied_revision{}, heartbeat_ms{};
    std::uint32_t generation_state{}, logging_active{}, fg_status{};
    std::uint64_t fg_attempts{}, fg_dispatches{}, fg_failures{};
    std::uint64_t guide_publications{}, guide_matches{}, guide_uses{}, telemetry_tick_ms{};
    std::uint32_t rates_valid{}, gpu_valid{}, gpu_source{}, stages_valid{}, game_time_valid{}, ofxr_time_valid{};
    double game_fps{}, generated_fps{}, output_fps{}, display_hz{}, gpu_render_ms{};
    double game_time_ms{}, generation_time_ms{}, delivery_time_ms{}, ofxr_time_ms{};
    std::uint32_t snapshot_memory_valid{}, snapshot_slots{}, retired_snapshot_slots{};
    std::uint64_t snapshot_bytes{};
};

struct OFXR_InputProviderHostV1 {
    std::uint32_t struct_size{sizeof(OFXR_InputProviderHostV1)}, abi_version{OFXR_INPUT_PROVIDER_ABI};
    // Records owned snapshots on producer_command_list. The caller keeps the
    // original resources alive through execution, just as for the game's work.
    // OFXR retains its snapshots and tracks actual Execute/Reset and GPU fences.
    int (*publish)(const OFXR_GuidePublicationV3*, OFXR_GuideReceiptV3*) noexcept{};
    int (*attach_camera)(std::uint64_t, std::uint64_t, const OFXR_CameraV3*) noexcept{};
    void (*invalidate_stream)(std::uint64_t) noexcept{};
    void (*retire_stream)(std::uint64_t) noexcept{};
    void (*observe_command_list)(ID3D12GraphicsCommandList*) noexcept{};
    // Only an exact evaluation/tagged-resource camera can be returned.
    int (*query_camera)(const OFXR_GuidePublicationV3*, OFXR_CameraV3*) noexcept{};
    int (*get_status)(OFXR_ProviderStatusV1*) noexcept{};
    void (*log)(const char*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept{};
};

struct OFXR_InputProviderV1 {
    std::uint32_t struct_size{sizeof(OFXR_InputProviderV1)}, abi_version{OFXR_INPUT_PROVIDER_ABI};
    char name[64]{};
    void (*configure)(ID3D12CommandQueue*, int) noexcept{};
    void (*stop_queue)(ID3D12CommandQueue*) noexcept{};
    void (*poll)() noexcept{};
    // Retire callback occurs at the same boundary as existing guide capture:
    // before actual Execute (true), or when a recording is abandoned (false).
    void (*retire_recording)(ID3D12GraphicsCommandList*, int) noexcept{};
    // Executed callbacks run after the actual native ExecuteCommandLists call.
    void (*submitted)(ID3D12CommandQueue*, unsigned, ID3D12CommandList* const*, ID3D12CommandList* const*) noexcept{};
    void (*projection)(const OFXR_ProjectionReceiptV1*) noexcept{};
};

// Export OFXR_InitializeInputProviderV1 from NAME.ofxr-provider.dll. Called once,
// outside DllMain, before forwarding xrCreateInstance. Return zero without
// installing hooks when the host ABI/process is unsupported. Successful modules
// remain loaded until process exit; configure(false)/stop_queue disarm capture.
using PFN_OFXR_InitializeInputProviderV1 = int (*)(const OFXR_InputProviderHostV1*, OFXR_InputProviderV1*) noexcept;

static_assert(std::is_standard_layout_v<OFXR_GuidePublicationV3>);
static_assert(std::is_trivially_copyable_v<OFXR_GuidePublicationV3>);
