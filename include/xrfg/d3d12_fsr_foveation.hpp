#pragma once
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include <memory>
namespace xrfg {
struct D3D12FsrEyeInput;
// One instance per OFXR work slot: descriptors/textures are reused only after
// that slot's fence. Inputs remain pinned by the slot until submission retires.
class D3D12FsrFoveation {
public:
    D3D12FsrFoveation();
    ~D3D12FsrFoveation();
    HRESULT convert(ID3D12GraphicsCommandList* list, unsigned eye,
        const DlssMotionVectorFrame& current, const DlssMotionVectorFrame& previous,
        const D3D12FieldOfView& current_fov,const D3D12FieldOfView& previous_fov,
        D3D12FsrEyeInput& input) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
