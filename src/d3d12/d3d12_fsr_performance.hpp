#pragma once
#include "xrfg/fsr_frame_generation.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <limits>

namespace xrfg {
// Sampled diagnostics on existing lists and retirement fences. This object
// never submits work, signals a fence, or waits for the GPU. One object per
// work slot prevents recycling timestamp storage while it is still in use.
class D3D12FsrPerformance {
public:
    static constexpr UINT max_eyes = 2;
    static constexpr UINT max_points = 3 + 4 * max_eyes;
    struct Sample {
        UINT points{};
        UINT64 total_us{};
        std::array<UINT64, max_points> gpu_us{}, cpu_us{};
    };
private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    UINT64 gpu_hz_{}, cpu_hz_{}, gpu_origin_{}, cpu_origin_{};
    UINT64 id_{}, serial_{}, submitted_{}, recorded_{};
    UINT eyes_{}, view_mask_{}, points_{};
    std::array<UINT64, max_points> cpu_{};
    bool active_{}, resolved_{}, pending_{}, calibrated_{};
    inline static std::atomic_uint64_t next_id_{};

    static UINT64 now() noexcept {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return static_cast<UINT64>(value.QuadPart);
    }
    static UINT64 micros(UINT64 ticks, UINT64 hz) noexcept {
        return hz ? static_cast<UINT64>(static_cast<long double>(ticks) * 1000000 / hz) : 0;
    }
    void error(HRESULT result) noexcept {
        fsr_diagnostic("fsr_perf_error", id_, static_cast<UINT>(result), points_);
    }
public:
    void initialize(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
        if (!device || !queue || readback_) return;
        LARGE_INTEGER hz{};
        if (!QueryPerformanceFrequency(&hz) || hz.QuadPart <= 0) return;
        cpu_hz_ = static_cast<UINT64>(hz.QuadPart);
        HRESULT result = queue->GetTimestampFrequency(&gpu_hz_);
        if (FAILED(result) || !gpu_hz_) { error(FAILED(result) ? result : E_FAIL); return; }
        D3D12_QUERY_HEAP_DESC desc{};
        desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        desc.Count = max_points;
        desc.NodeMask = queue->GetDesc().NodeMask;
        result = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap_));
        if (FAILED(result)) { error(result); return; }
        D3D12_HEAP_PROPERTIES memory{};
        memory.Type = D3D12_HEAP_TYPE_READBACK;
        memory.CreationNodeMask = memory.VisibleNodeMask = desc.NodeMask ? desc.NodeMask : 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = max_points * sizeof(UINT64);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_));
        if (FAILED(result)) { error(result); heap_.Reset(); }
    }
    bool begin(ID3D12GraphicsCommandList* commands, ID3D12CommandQueue* queue,
               UINT64 serial, UINT eyes, UINT view_mask) noexcept {
        if (pending_ || !readback_ || !commands || !queue || !eyes || eyes > max_eyes) return false;
        active_ = true;
        resolved_ = false;
        points_ = 0;
        id_ = ++next_id_;
        serial_ = serial;
        eyes_ = eyes;
        view_mask_ = view_mask;
        // Refresh each sample: GPU and CPU clock correlation can drift when idle.
        calibrated_ = SUCCEEDED(queue->GetClockCalibration(&gpu_origin_, &cpu_origin_));
        mark(commands);
        return true;
    }
    void mark(ID3D12GraphicsCommandList* commands) noexcept {
        if (!active_ || !commands) return;
        if (points_ >= max_points) { active_ = false; error(E_BOUNDS); return; }
        cpu_[points_] = now();
        commands->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, points_++);
    }
    void input(UINT eye, UINT width, UINT height, UINT guide_width, UINT guide_height) noexcept {
        if (active_) fsr_diagnostic("fsr_perf_input", id_, eye,
            (UINT64(width) << 32) | height, (UINT64(guide_width) << 32) | guide_height);
    }
    void end_recording(ID3D12GraphicsCommandList* commands, HRESULT result) noexcept {
        if (!active_) return;
        if (SUCCEEDED(result)) mark(commands); // Composition and real-frame copy finished.
        recorded_ = now();
        fsr_diagnostic("fsr_perf_frame", id_, serial_, eyes_, view_mask_);
        fsr_diagnostic("fsr_perf_record", id_, static_cast<UINT>(result),
            micros(recorded_ - cpu_[0], cpu_hz_), points_);
        resolved_ = SUCCEEDED(result) && points_ == 3 + 4 * eyes_;
        if (resolved_) {
            commands->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                0, points_, readback_.Get(), 0);
        } else if (SUCCEEDED(result)) error(E_UNEXPECTED);
        active_ = false;
    }
    // Called immediately before the existing ExecuteCommandLists. Harvest
    // additionally requires that submission's existing fence to be complete.
    void submitted() noexcept {
        if (!resolved_) return;
        submitted_ = now();
        pending_ = true;
        resolved_ = false;
    }
    void discard_recording() noexcept {
        active_ = resolved_ = false;
    }
    bool harvest(ID3D12Fence* fence, UINT64 value, Sample* sample = nullptr) noexcept {
        if (!pending_ || !fence || !value) return false;
        const auto completed = fence->GetCompletedValue();
        if (completed == std::numeric_limits<UINT64>::max()) {
            pending_ = false; error(DXGI_ERROR_DEVICE_REMOVED); return false;
        }
        if (completed < value) return false;
        pending_ = false;
        const D3D12_RANGE range{0, points_ * sizeof(UINT64)};
        void* mapped{};
        const auto result = readback_->Map(0, &range, &mapped);
        if (FAILED(result) || !mapped) { error(FAILED(result) ? result : E_FAIL); return false; }
        std::array<UINT64, max_points> gpu{};
        const auto* source = static_cast<const UINT64*>(mapped);
        for (UINT i = 0; i < points_; ++i) gpu[i] = source[i];
        const D3D12_RANGE empty{0, 0};
        readback_->Unmap(0, &empty);
        for (UINT i = 0; i < points_; ++i) {
            if (!gpu[i] || (i && (gpu[i] < gpu[i-1] || cpu_[i] < cpu_[i-1]))) {
                error(E_FAIL); return false;
            }
        }
        Sample measured{};
        measured.points = points_;
        measured.total_us = micros(gpu[points_-1] - gpu[0], gpu_hz_);
        fsr_diagnostic("fsr_perf_gpu", id_, measured.total_us, points_, gpu_hz_);
        for (UINT i = 1; i < points_; ++i) {
            measured.gpu_us[i] = micros(gpu[i] - gpu[i-1], gpu_hz_);
            measured.cpu_us[i] = micros(cpu_[i] - cpu_[i-1], cpu_hz_);
            fsr_diagnostic("fsr_perf_stage", id_, i, measured.cpu_us[i], measured.gpu_us[i]);
        }
        const long double gpu_begin_qpc = cpu_origin_ +
            (static_cast<long double>(gpu[0]) - gpu_origin_) * cpu_hz_ / gpu_hz_;
        const long double delay = gpu_begin_qpc - submitted_;
        const bool queue_valid = calibrated_ && delay >= 0 &&
            delay < static_cast<long double>(std::numeric_limits<UINT64>::max());
        fsr_diagnostic("fsr_perf_queue", id_, queue_valid ? micros(static_cast<UINT64>(delay), cpu_hz_) : 0,
            submitted_ >= recorded_ ? micros(submitted_ - recorded_, cpu_hz_) : 0, queue_valid);
        if (sample) *sample = measured;
        return true;
    }
};
} // namespace xrfg
