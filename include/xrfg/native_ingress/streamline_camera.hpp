#pragma once
#include <array>
#include <cstdint>
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
namespace xrfg::native_ingress::streamline_camera {
struct Camera {
    std::uint32_t frame{}, viewport{};
    bool valid{}, inverted{}, ambiguous{};
    // Diagnostic only: projection=1, inverse=2, position=4, basis=8,
    // near=16, far=32, FOV=64, no evaluation scope=256,
    // no exact successful constants=512, ambiguous viewport=1024.
    std::uint32_t rejection{256};
    bool observed{};
    std::array<float,16> projection{}, inverse{}, previous{}, to_current{};
    std::array<float,3> position{}, right{}, up{}, forward{};
    float near_z{}, far_z{}, fov{};
    int camera_motion{-1};
};
// Luke may reconstruct a missing/invalid FOV only from this exact packet's
// normalized perspective and mutually inverse matrices. Other callers retain
// the original SDK scalar and admission decision.
Camera current(bool luke_projection_receipt=false) noexcept;
bool hooked() noexcept;
// Native FSR can execute outside slEvaluateFeature. Accept only fresh
// frame-tagged depth AND motion on this exact recording list, never latest
// constants by time/order. Tags are single-use and retired on Execute/Reset.
void enable_fsr_tags(bool enabled) noexcept;
void retire_fsr_tags(ID3D12GraphicsCommandList* list) noexcept;
Camera take_fsr_camera(ID3D12GraphicsCommandList* list, ID3D12Resource* depth,
    ID3D12Resource* motion, unsigned depth_width, unsigned depth_height,
    unsigned motion_width, unsigned motion_height) noexcept;
struct FsrCameraStatistics { std::uint64_t constants{}, tags{}, matched{}, rejected{}; };
FsrCameraStatistics fsr_statistics() noexcept;
}
