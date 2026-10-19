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
#include "mali_normal.hpp"
#include "edid.h"
#include "steamcompmgr.hpp"
int g_nNestedWidth=320, g_nNestedHeight=180;
bool g_bFullscreen=false, g_bBorderlessOutputWindow=false;
int main(int argc, char **argv) {
 if (argc == 2 && !strcmp(argv[1], "--mali-interactive-client")) return mali_interactive_main();
 return mali_normal_main(argc, argv);
}
#include "wlserver.hpp"
#include "waitable.h"
#include "Utils/TempFiles.h"
LogScope g_WaitableLog("waitable");
std::atomic<bool> hasRepaint=false;
bool g_bForceRelativeMouse=false, g_bColorSliderInUse=false, g_bOutputHDREnabled=false, g_bHDRItmEnable=false, g_bGrabbed=false, g_bForceHDR10OutputDebug=false;
GamescopeUpscaleFilter g_wantedUpscaleFilter=GamescopeUpscaleFilter::LINEAR;
int g_upscaleFilterSharpness=2;
gamescope::ConVar<bool> cv_composite_force("composite",false), cv_adaptive_sync("adaptive",false), cv_hdr_enabled("hdr",false);
gamescope::VBlankTime g_SteamCompMgrVBlankTime{};
void force_repaint() { hasRepaint=true; }
void nudge_steamcompmgr() {} // The normal runner owns its event loop, no X11 manager.
void gamescope::CVBlankTimer::UpdateWasCompositing(bool) { forbidden(); }
void gamescope::CVBlankTimer::UpdateLastDrawTime(uint64_t) { forbidden(); }
std::vector<uint8_t> gamescope::GenerateSimpleEdid(uint32_t,uint32_t) { forbidden(); return {}; }
int gamescope::MakeTempFile(char (&)[4096], const char *, bool) { forbidden(); return -1; }
gamescope::CScreenshotManager &gamescope::CScreenshotManager::Get() { forbidden(); __builtin_unreachable(); }
void wlserver_key(uint32_t,bool,uint32_t) { forbidden(); }
void wlserver_touchdown(double,double,int32_t,uint32_t,gamescope::IBackendConnector*) { forbidden(); }
void wlserver_touchup(int32_t,uint32_t) { forbidden(); }
void wlserver_touchmotion(double,double,int32_t,uint32_t,bool,gamescope::IBackendConnector*) { forbidden(); }
void wlserver_mousebutton(int,bool,uint32_t) { forbidden(); }
void wlserver_mousewheel(double,double,uint32_t) { forbidden(); }
void wlserver_mousemotion(double,double,uint32_t) { forbidden(); }
#include "Utils/Algorithm.h"
void gamescope::WritePatchedEdid(std::span<const uint8_t>,const gamescope::BackendConnectorHDRInfo &,bool) {}
namespace gamescope { std::shared_ptr<INestedHints::CursorInfo> GetX11HostCursor() { forbidden(); return {}; } }
