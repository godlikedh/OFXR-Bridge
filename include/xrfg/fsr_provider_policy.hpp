#pragma once
#include <string_view>

namespace xrfg {
enum class FsrProviderRequest { automatic, analytical, ml };
enum class FsrProviderChoice { analytical, ml, unavailable };
// IDs are opaque. Classify AMD's version names, never the DLL file version or
// the position in the enumeration (both have changed between SDK releases).
inline bool fsr_ml_version(std::string_view name) noexcept {
    for (std::size_t i = 0; i + 4 < name.size(); ++i) {
        if ((i == 0 || name[i - 1] < '0' || name[i - 1] > '9') &&
            name[i] == '4' && name[i+1] == '.' && name[i+2] >= '0' &&
            name[i+2] <= '9' && name[i+3] == '.' && name[i+4] >= '0' && name[i+4] <= '9') return true;
    }
    return false;
}
inline FsrProviderChoice choose_fsr_provider(FsrProviderRequest request,
    bool windows11, bool shader_model66, bool amd_adapter, bool scene_camera,
    bool enumerated_ml) noexcept {
    if (request == FsrProviderRequest::analytical) return FsrProviderChoice::analytical;
    if (windows11 && shader_model66 && amd_adapter && scene_camera && enumerated_ml)
        return FsrProviderChoice::ml;
    return request == FsrProviderRequest::ml ? FsrProviderChoice::unavailable : FsrProviderChoice::analytical;
}
}
