#pragma once
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include <memory>
namespace xrfg {
struct D3D12FsrEyeInput;
// One instance per work slot: descriptors and resource references remain live
// until its fence. Scratch may be shared only by serial recordings submitted
// in order on ONE queue; consume scratch within the same recording as conversion.
class D3D12FsrFoveation {
public:
    struct Scratch;
    static std::shared_ptr<Scratch> make_scratch();
    explicit D3D12FsrFoveation(std::shared_ptr<Scratch> scratch = {});
    ~D3D12FsrFoveation();
    HRESULT convert(ID3D12GraphicsCommandList* list, unsigned eye,
        const DlssMotionVectorFrame& current, const DlssMotionVectorFrame& previous,
        const D3D12FieldOfView& current_fov,const D3D12FieldOfView& previous_fov,
        D3D12FsrEyeInput& input) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
