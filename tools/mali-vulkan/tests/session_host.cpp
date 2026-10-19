/* Host fixture links the actual complete Gamescope rendervulkan.cpp TU.
 * Unrelated backend/effect/process services abort if reached. No renderer code
 * or Vulkan operations are replaced here; the actual loader/ICD/broker is used.
 */
#include <cassert>
#include <chrono>
#include "vblankmanager.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include "rendervulkan.hpp"
#include "reshade_effect_manager.hpp"
#include "vulkan_renderer_test.hpp"
#include "Utils/Process.h"
struct LogConVar_t {};
namespace reshadefx { struct module {}; }
LogScope::LogScope(std::string_view name, LogPriority level) : LogScope(name, name, level) {}
LogScope::LogScope(std::string_view name, std::string_view prefix, LogPriority level) : m_psvName(name), m_psvPrefix(prefix), m_eMaxPriority(level) {}
LogScope::~LogScope() = default;
bool LogScope::Enabled(LogPriority level) const { return level <= m_eMaxPriority; }
void LogScope::vlogf(LogPriority, const char *f, va_list a) { vfprintf(stderr, f, a); fputc('\n', stderr); }
#define TEST_LOG(n) void LogScope::n(const char *f, ...) { va_list a; va_start(a, f); vlogf(LOG_INFO, f, a); va_end(a); }
TEST_LOG(infof) TEST_LOG(errorf) TEST_LOG(warnf) TEST_LOG(debugf) TEST_LOG(errorf_errno)
LogScope console_log("test");
gamescope::ConCommand::ConCommand(std::string_view n, std::string_view d, ConCommandFunc f, bool) : m_pszName(n), m_pszDescription(d), m_Func(f) {}
gamescope::ConCommand::~ConCommand() = default;
static void forbidden() { std::fputs("FORBIDDEN backend/effect service reached\n", stderr); std::abort(); }

ReshadeEffectManager::ReshadeEffectManager() = default;
ReshadeEffectPipeline::~ReshadeEffectPipeline() = default;
ReshadeEffectManager g_reshadeManager;
void ReshadeEffectManager::init(CVulkanDevice *) { forbidden(); }
void ReshadeEffectManager::clear() { forbidden(); }
ReshadeEffectPipeline *ReshadeEffectManager::pipeline(const ReshadeEffectKey &) { forbidden(); return nullptr; }
uint64_t ReshadeEffectPipeline::execute(gamescope::Rc<CVulkanTexture>, gamescope::Rc<CVulkanTexture> *) { forbidden(); return 0; }
bool gamescope::Process::HasCapSysNice() { forbidden(); return false; }
bool env_to_bool(const char *e) { return e && !strcmp(e, "1"); }
uint32_t currentOutputWidth = 256, currentOutputHeight = 256;
uint32_t g_nOutputWidth = 256, g_nOutputHeight = 256;
uint32_t g_uOutputRotation = 0;
bool g_bSteamIsActiveWindow = false;
uint32_t g_preferVendorID = 0, g_preferDeviceID = 0;
EStreamColorspace g_ForcedNV12ColorSpace = k_EStreamColorspace_Unknown;
std::string g_reshade_effect;
uint32_t g_reshade_technique_idx = 0;
uint64_t get_time_in_nanos() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

int g_nPreferredOutputWidth=256, g_nPreferredOutputHeight=256;
int g_nNestedRefresh=30000;
int g_nOutputRefresh=30000;
bool g_bAllowDeferredBackend=false;
void wlserver_lock() { forbidden(); }
void wlserver_unlock(bool) { forbidden(); }
bool wlsession_init() { forbidden(); return false; }
void sleep_until_nanos(uint64_t) { forbidden(); }
gamescope::CVBlankTimer &GetVBlankTimer() { forbidden(); __builtin_unreachable(); }
gamescope::VBlankScheduleTime gamescope::CVBlankTimer::CalcNextWakeupTime(bool) { forbidden(); return {}; }
void gamescope::CVBlankTimer::MarkVBlank(uint64_t, bool) { forbidden(); }
void mangoapp_output_update(uint64_t) { forbidden(); }
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--mali-shm-client")) return mali_wayland_client_main(argv[2], std::strtoul(argv[3], nullptr, 10));
    return vulkan_mali_session_test(argc == 2 && !strcmp(argv[1], "wayland"));
}
