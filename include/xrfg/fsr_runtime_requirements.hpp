#pragma once
#include <windows.h>
#include <winternl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace xrfg {
struct FsrRuntimeRequirements { unsigned build{}, vendor{}, device{}; bool windows11{}, shader_model66{}, amd{}; };
inline FsrRuntimeRequirements fsr_runtime_requirements(ID3D12Device* device) noexcept {
    FsrRuntimeRequirements result;
    // GetVersionEx is manifest-dependent inside an injected host.
    using Version = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto version = reinterpret_cast<Version>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW os{}; os.dwOSVersionInfoSize = sizeof(os);
    if (version && version(&os) == 0) { result.build = os.dwBuildNumber; result.windows11 = os.dwMajorVersion >= 10 && os.dwBuildNumber >= 22000; }
    if (!device) return result;
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    result.shader_model66 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory; Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) { result.vendor = desc.VendorId; result.device = desc.DeviceId; result.amd = desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE); }
    }
    // RDNA4 support is finally proved by AMD's per-device provider query,
    // rather than a guessed PCI device range.
    return result;
}
}
