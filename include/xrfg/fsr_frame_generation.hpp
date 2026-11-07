#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace xrfg {
void fsr_diagnostic(const char* message, std::uint64_t a=0, std::uint64_t b=0,
    std::uint64_t c=0, std::uint64_t d=0) noexcept;
// Provider diagnostics: producers only try to enqueue a bounded metadata event.
// The existing background monitor owns disk writes, independently of F9.
void set_fsr_background_diagnostics(bool enabled) noexcept;
void flush_fsr_background_diagnostics(const std::filesystem::path& root) noexcept;
bool append_bounded_fsr_diagnostics(const std::filesystem::path& path, std::string_view text) noexcept;

enum class FsrFrameGenerationStatus : std::uint32_t {
    disabled,
    waiting_for_guides,
    runtime_missing,
    initialization_failed,
    dispatch_failed,
    used,
    stereo_unavailable,
    stereo_recovering,
};

struct FsrFrameGenerationStatistics {
    std::uint64_t attempts{};
    std::uint64_t used{};
    std::uint64_t failures{};
    FsrFrameGenerationStatus status{FsrFrameGenerationStatus::disabled};
};

void report_fsr_frame_generation_status(
    FsrFrameGenerationStatus status) noexcept;
void report_fsr_frame_generation_attempt(bool used) noexcept;
FsrFrameGenerationStatistics fsr_frame_generation_statistics() noexcept;

}  // namespace xrfg
