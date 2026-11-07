#pragma once
#include "input_provider_api.hpp"

namespace xrfg {
bool input_providers_observe_projection() noexcept;
void prepare_input_providers(const wchar_t* layer_directory) noexcept;
void poll_input_providers() noexcept;
void configure_input_providers(ID3D12CommandQueue*, bool) noexcept;
void stop_input_providers(ID3D12CommandQueue*) noexcept;
void retire_provider_recording(ID3D12GraphicsCommandList*, bool) noexcept;
void report_provider_submission(ID3D12CommandQueue*, unsigned, ID3D12CommandList* const*, ID3D12CommandList* const*) noexcept;
void report_provider_projection(const OFXR_ProjectionReceiptV1&) noexcept;
const OFXR_InputProviderHostV1& input_provider_host() noexcept;
}
