#include "xrfg/fsr_frame_generation.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/autonomous_control.hpp"
#include <windows.h>
#include <filesystem>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <atomic>
#include <array>
#include <vector>
#include <cstring>
#include <sstream>
#include <locale>
#include <algorithm>

namespace xrfg {
namespace {
struct BackgroundEvent {
    std::uint64_t tick{}, a{}, b{}, c{}, d{};
    char message[96]{};
};
std::atomic_bool background_enabled{};
std::atomic_uint64_t background_dropped{};
std::mutex background_mutex;
std::array<BackgroundEvent,8192> background_events;
std::size_t background_count{};
}
void set_fsr_background_diagnostics(bool enabled) noexcept {
    background_enabled.store(enabled,std::memory_order_release);
}
bool append_bounded_fsr_diagnostics(const std::filesystem::path& path, std::string_view text) noexcept {
    try {
        std::filesystem::create_directories(path.parent_path());
        std::error_code error;
        const auto bytes=std::filesystem::file_size(path,error);
        if (!error && bytes>=4*1024*1024) {
            const auto previous=path.parent_path()/(path.stem().wstring()+L".previous"+path.extension().wstring());
            std::filesystem::remove(previous,error);error.clear();
            std::filesystem::rename(path,previous,error);
            if(error)return false;
        }
        auto* file=_wfsopen(path.c_str(),L"ab",_SH_DENYWR);
        if(!file)return false;
        const bool written=std::fwrite(text.data(),1,text.size(),file)==text.size();
        return std::fclose(file)==0 && written;
    } catch (...) { return false; }
}
void flush_fsr_background_diagnostics(const std::filesystem::path& root) noexcept {
    if(!background_enabled.load(std::memory_order_acquire))return;
    try {
        std::vector<BackgroundEvent> events(background_events.size());
        std::size_t count{};
        {
            std::scoped_lock lock(background_mutex);
            count=background_count;
            std::copy_n(background_events.begin(),count,events.begin());
            background_count=0;
        }
        const auto dropped=background_dropped.exchange(0);
        if(!count && !dropped)return;
        std::ostringstream out;out.imbue(std::locale::classic());
        for(std::size_t i=0;i<count;++i) {
            const auto& e=events[i];
            out<<e.tick<<' '<<e.message<<" a="<<e.a<<" b="<<e.b<<" c="<<e.c<<" d="<<e.d<<'\n';
        }
        if(dropped)out<<GetTickCount64()<<" fsr_background_dropped a="<<dropped<<" b=0 c=0 d=0\n";
        if(!append_bounded_fsr_diagnostics(root/(L"provider-events-"+std::to_wstring(GetCurrentProcessId())+L".log"),out.str()))
            background_dropped.fetch_add(count+dropped);
    } catch (...) { background_dropped.fetch_add(1); }
}
void fsr_diagnostic(const char* message, std::uint64_t a, std::uint64_t b,
    std::uint64_t c, std::uint64_t d) noexcept {
    if(background_enabled.load(std::memory_order_acquire)) {
        std::unique_lock lock(background_mutex,std::try_to_lock);
        if(!lock.owns_lock() || background_count==background_events.size()) {
            background_dropped.fetch_add(1,std::memory_order_relaxed);return;
        }
        auto& e=background_events[background_count++];
        e.tick=GetTickCount64();e.a=a;e.b=b;e.c=c;e.d=d;
        const auto length=(std::min)(std::strlen(message),sizeof(e.message)-1);
        std::memcpy(e.message,message,length);e.message[length]=0;
        return;
    }
    if (!bridge_flight_logger().enabled()) return;
    try {
        static std::mutex mutex;
        std::scoped_lock lock(mutex);
        static FILE* file=[]() -> FILE* {
            wchar_t root[32768]{};
            const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",root,32768);
            if (!n || n>=32768) return nullptr;
            auto folder=autonomous::global_configuration_path().parent_path();
            std::filesystem::create_directories(folder);
            auto path=folder/(L"fsr-"+std::to_wstring(GetCurrentProcessId())+L".log");
            // Keep one writer, but permit the diagnostic collector to read
            // while the game remains open. _wfopen_s denies shared access.
            return _wfsopen(path.c_str(),L"w",_SH_DENYWR);
        }();
        if (file && ftell(file)<8*1024*1024) {
            std::fprintf(file,"%llu %s a=%llu b=%llu c=%llu d=%llu\n",
                static_cast<unsigned long long>(GetTickCount64()),message,
                static_cast<unsigned long long>(a),static_cast<unsigned long long>(b),
                static_cast<unsigned long long>(c),static_cast<unsigned long long>(d));
            std::fflush(file);
        }
    } catch (...) {}
}
namespace {

std::mutex g_mutex;
FsrFrameGenerationStatistics g_statistics;

}  // namespace

void report_fsr_frame_generation_status(
    FsrFrameGenerationStatus status) noexcept {
    std::scoped_lock lock(g_mutex);
    g_statistics.status = status;
}

void report_fsr_frame_generation_attempt(bool used) noexcept {
    std::scoped_lock lock(g_mutex);
    ++g_statistics.attempts;
    if (used) {
        ++g_statistics.used;
        g_statistics.status = FsrFrameGenerationStatus::used;
    } else {
        ++g_statistics.failures;
    }
}

FsrFrameGenerationStatistics fsr_frame_generation_statistics() noexcept {
    std::scoped_lock lock(g_mutex);
    return g_statistics;
}

}  // namespace xrfg
