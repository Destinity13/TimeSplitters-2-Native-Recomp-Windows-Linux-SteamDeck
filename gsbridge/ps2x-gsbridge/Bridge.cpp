#include <string_view>
// ps2x-gsbridge: the PCSX2 GS as a library for a native PS2 port (SotC Recomp, TS2 Recomp).
// Derived from pcsx2-gsrunner/Main.cpp (PCSX2 Dev Team, GPL-3.0+): host interface, configuration and the
// X11 window remain; instead of a dump file the port feeds packets live (GSDumpReplayer live mode).
// SPDX-License-Identifier: GPL-3.0+
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <dxgi1_6.h>
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/CocoaTools.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GSCapture.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/GSPerfMon.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/R3000A.h"
#include "pcsx2/IopHw.h"
#include "pcsx2/SPU2/spu2.h"
#include "pcsx2/SPU2/defs.h"

#include "svnrev.h"

// Down here because X11 has a lot of defines that can conflict
#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace GSRunner
{
	static void InitializeConsole();
	static bool InitializeConfig();
	static void SettingsOverride();
	static bool ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params);
	static void DumpStats();

	static bool CreatePlatformWindow();
	static void DestroyPlatformWindow();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void PumpPlatformMessages(bool forever = false);
	static void StopPlatformMessagePump();
} // namespace GSRunner

static constexpr u32 WINDOW_WIDTH = 1280;
static constexpr u32 WINDOW_HEIGHT = 960;
// PS2XGS_WINDOW=WxH (e.g. 1280x720 for showcase recordings): start size of the X11 window
static u32 ps2xFensterMass(bool hoehe)
{
	static const std::pair<u32, u32> m = [] { unsigned w = 0, h = 0; const char* e = std::getenv("PS2XGS_WINDOW");
		if (e && std::sscanf(e, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) return std::make_pair(static_cast<u32>(w), static_cast<u32>(h)); return std::make_pair(WINDOW_WIDTH, WINDOW_HEIGHT); }();
	return hoehe ? m.second : m.first;
}

static MemorySettingsInterface s_settings_interface;

static std::string s_output_prefix;
static s32 s_loop_count = 1;
static std::optional<bool> s_use_window;
static bool s_no_console = false;

// Owned by the GS thread.
static u32 s_dump_frame_number = 0;
static u32 s_loop_number = s_loop_count;
static double s_last_internal_draws = 0;
static double s_last_draws = 0;
static double s_last_render_passes = 0;
static double s_last_barriers = 0;
static double s_last_copies = 0;
static double s_last_uploads = 0;
static double s_last_readbacks = 0;
static double s_last_depth_copies_rov = 0;
static double s_last_draws_rov = 0;
static double s_last_barriers_rov = 0;
static u64 s_total_internal_draws = 0;
static u64 s_total_draws = 0;
static u64 s_total_render_passes = 0;
static u64 s_total_barriers = 0;
static u64 s_total_copies = 0;
static u64 s_total_uploads = 0;
static u64 s_total_readbacks = 0;
static u64 s_total_copies_rov = 0;
static u64 s_total_draws_rov = 0;
static u64 s_total_barriers_rov = 0;
static u32 s_total_frames = 0;
static u32 s_total_drawn_frames = 0;

static bool s_perf_enable = false;
static float s_perf_updates = 0.0f;
static float s_perf_sum_fps = 0.0f;
static float s_perf_sum_internal_fps = 0.0f;
static float s_perf_sum_cpu_thread_usage = 0.0f;
static float s_perf_sum_cpu_thread_time = 0.0f;
static float s_perf_sum_gs_thread_usage = 0.0f;
static float s_perf_sum_gs_thread_time = 0.0f;
static float s_perf_sum_gpu_time = 0.0f;
static float s_perf_sum_gpu_usage = 0.0f;

bool GSRunner::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	// ps2x: resources (shaders, fonts, GameDB) sit next to the library, not next to the game executable
	if (const char* root = std::getenv("PS2XGS_APPROOT"); root && *root)
		EmuFolders::AppRoot = root;
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;
#ifdef _WIN32
	// Own data directory instead of Documents\PCSX2: otherwise an installed PCSX2 shares its shader cache,
	// settings and resources with the port (different format, different version). PS2XGS_DATAROOT overrides it.
	{
		std::string dr;
		if (const char* e = std::getenv("PS2XGS_DATAROOT"); e && *e) dr = e;
		else if (const char* a = std::getenv("APPDATA"); a && *a)
		{
			const char* cn = std::getenv("PS2XGS_CFGNAME");
			dr = std::string(a) + "\\" + ((cn && *cn) ? cn : "ts2-recomp") + "\\pcsx2";
		}
		if (!dr.empty() && FileSystem::CreateDirectoryPath(dr.c_str(), false))
			EmuFolders::DataRoot = dr;
	}
#endif

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
		return false;

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);

		ImGuiManager::SetFonts(std::move(fonts));
	}

	// don't provide an ini path, or bother loading. we'll store everything in memory.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// not running any UI, so no settings requests will come in
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	// not running any UI, so no settings requests will come in
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
	// noop
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
	// noop
}

void Host::EndTextInput()
{
	// noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	if (s_loop_number == 0 && !s_output_prefix.empty())
	{
		// when we wrap around, don't race other files
		GSJoinSnapshotThreads();

		// queue dumping of this frame
		std::string dump_path(fmt::format("{}_frame{:05}.png", s_output_prefix, s_dump_frame_number));
		GSQueueSnapshot(dump_path);
	}

	if (GSIsHardwareRenderer())
	{
		const u32 last_draws = s_total_internal_draws;
		const u32 last_uploads = s_total_uploads;

		static constexpr auto update_stat = [](GSPerfMon::counter_t counter, u64& dst, double& last) {
			// perfmon resets every 30 frames to zero
			const double val = g_perfmon.GetCounter(counter);
			dst += static_cast<u64>((val < last) ? val : (val - last));
			last = val;
		};

		update_stat(GSPerfMon::Draw, s_total_internal_draws, s_last_internal_draws);
		update_stat(GSPerfMon::DrawCalls, s_total_draws, s_last_draws);
		update_stat(GSPerfMon::RenderPasses, s_total_render_passes, s_last_render_passes);
		update_stat(GSPerfMon::Barriers, s_total_barriers, s_last_barriers);
		update_stat(GSPerfMon::TextureCopies, s_total_copies, s_last_copies);
		update_stat(GSPerfMon::TextureUploads, s_total_uploads, s_last_uploads);
		update_stat(GSPerfMon::Readbacks, s_total_readbacks, s_last_readbacks);
		update_stat(GSPerfMon::TextureCopiesROV, s_total_copies_rov, s_last_depth_copies_rov);
		update_stat(GSPerfMon::DrawCallsROV, s_total_draws_rov, s_last_draws_rov);
		update_stat(GSPerfMon::BarriersROV, s_total_barriers_rov, s_last_barriers_rov);

		const bool idle_frame = s_total_frames && (last_draws == s_total_internal_draws && last_uploads == s_total_uploads);

		if (!idle_frame)
			s_total_drawn_frames++;

		s_total_frames++;

		// PS2XGS_STAT=1: print the PCSX2 counters every 50 frames (draws, render passes, barriers, copies, uploads, readbacks)
		static const bool statOn = [] { const char* e = std::getenv("PS2XGS_STAT"); return e && *e == '1'; }();
		if (statOn && (s_total_frames % 50) == 0)
			Console.WriteLn("[ps2xgs-stat] frames=%llu draws=%llu calls=%llu passes=%llu barriers=%llu copies=%llu uploads=%llu readbacks=%llu",
				(unsigned long long)s_total_frames, (unsigned long long)s_total_internal_draws, (unsigned long long)s_total_draws,
				(unsigned long long)s_total_render_passes, (unsigned long long)s_total_barriers, (unsigned long long)s_total_copies,
				(unsigned long long)s_total_uploads, (unsigned long long)s_total_readbacks);

		std::atomic_thread_fence(std::memory_order_release);
	}
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

namespace ps2xmenu { void autoWaechter(float vps); }
void Host::OnPerformanceMetricsUpdated()
{
	ps2xmenu::autoWaechter(PerformanceMetrics::GetFPS());
	// PS2XGS_PERFLOG=1: log the real game frame rate (as in the FPS display) and VBlanks/s
	static const bool perflog = std::getenv("PS2XGS_PERFLOG") != nullptr;
	if (perflog)
		Console.WriteLn(fmt::format("[ps2xperf] FPS {:.2f} ({}) | VPS {:.2f} | Bildzeit {:.2f} ms", PerformanceMetrics::GetInternalFPS(),
			PerformanceMetrics::GetInternalFPSMethod() == PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister ? "P" :
			PerformanceMetrics::GetInternalFPSMethod() == PerformanceMetrics::InternalFPSMethod::DISPFBBlit ? "B" : "-",
			PerformanceMetrics::GetFPS(), PerformanceMetrics::GetAverageFrameTime()));
	if (s_perf_enable)
	{
		s_perf_updates += 1.0f;
		s_perf_sum_fps += PerformanceMetrics::GetFPS();
		s_perf_sum_internal_fps += PerformanceMetrics::GetInternalFPS();
		s_perf_sum_cpu_thread_usage += PerformanceMetrics::GetCPUThreadUsage();
		s_perf_sum_cpu_thread_time += PerformanceMetrics::GetCPUThreadAverageTime();
		s_perf_sum_gs_thread_usage += PerformanceMetrics::GetGSThreadUsage();
		s_perf_sum_gs_thread_time += PerformanceMetrics::GetGSThreadAverageTime();
		s_perf_sum_gpu_time += PerformanceMetrics::GetGPUAverageTime();
		s_perf_sum_gpu_usage += PerformanceMetrics::GetGPUUsage();
	}
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	pxFailRel("Not implemented");
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	// noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	// noop
}

void Host::OnAchievementsRefreshed()
{
	// noop
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void PrintCommandLineVersion()
{
	std::fprintf(stderr, "PCSX2 GS Runner Version %s\n", GIT_REV);
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname)
{
	PrintCommandLineVersion();
	std::fprintf(stderr, "Usage: %s [parameters] [--] [filename]\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "  -dumpdir <dir>: Frame dump directory (will be dumped as filename_frameN.png).\n");
	std::fprintf(stderr, "  -dump [rt|tex|z|f|a|i|tr|ds|fs|hw]: Enabling dumping of render target, texture, z buffer, frame, "
		"alphas, and info (context, vertices, list of transfers), transfers images, draw stats, frame stats, HW config, respectively, per draw. Generates lots of data.\n");
	std::fprintf(stderr, "  -dumprange N[,L,B]: Start dumping from draw N (base 0), stops after L draws, and only "
		"those draws that are multiples of B (intersection of -dumprange and -dumprangef used)."
		"Defaults to 0,-1,1 (all draws). Only used if -dump used.\n");
	std::fprintf(stderr, "  -dumprangef NF[,LF,BF]: Start dumping from frame NF (base 0), stops after LF frames, "
		"and only those frames that are multiples of BF (intersection of -dumprange and -dumprangef used).\n"
		"Defaults to 0,-1,1 (all frames). Only used if -dump is used.\n");
	std::fprintf(stderr, "  -loop <count>: Loops dump playback N times. Defaults to 1. 0 will loop infinitely.\n");
	std::fprintf(stderr, "  -renderer <renderer>: Sets the graphics renderer. Defaults to Auto.\n");
	std::fprintf(stderr, "  -swthreads <threads>: Sets the number of threads for the software renderer.\n");
	std::fprintf(stderr, "  -window: Forces a window to be displayed.\n");
	std::fprintf(stderr, "  -surfaceless: Disables showing a window.\n");
	std::fprintf(stderr, "  -logfile <filename>: Writes emu log to filename.\n");
	std::fprintf(stderr, "  -noshadercache: Disables the shader cache (useful for parallel runs).\n");
	std::fprintf(stderr, "  -perf: Enable frame timing performance stats.\n");
	std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
						 "    parameters make up the filename. Use when the filename contains\n"
						 "    spaces or starts with a dash.\n");
	std::fprintf(stderr, "\n");
}

void GSRunner::InitializeConsole()
{
	const char* var = std::getenv("PCSX2_NOCONSOLE");
	s_no_console = (var && StringUtil::FromChars<bool>(var).value_or(false));
	if (!s_no_console)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

bool GSRunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	std::string dumpdir; // Save from argument -dumpdir for creating sub-directories
	bool no_more_args = false;
	for (int i = 1; i < argc; i++)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

			if (CHECK_ARG("-help"))
			{
				PrintCommandLineHelp(argv[0]);
				return false;
			}
			else if (CHECK_ARG("-version"))
			{
				PrintCommandLineVersion();
				return false;
			}
			else if (CHECK_ARG_PARAM("-dumpdir"))
			{
				dumpdir = s_output_prefix = StringUtil::StripWhitespace(argv[++i]);
				if (s_output_prefix.empty())
				{
					Console.Error("Invalid dump directory specified.");
					return false;
				}

				if (!FileSystem::DirectoryExists(s_output_prefix.c_str()) && !FileSystem::CreateDirectoryPath(s_output_prefix.c_str(), false))
				{
					Console.Error("Failed to create output directory");
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-dump"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "DumpGSData", true);

				if (str.find("rt") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveRT", true);
				if (str.find("f") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrame", true);
				if (str.find("tex") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTexture", true);
				if (str.find("z") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDepth", true);
				if (str.find("a") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveAlpha", true);
				if (str.find("i") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveInfo", true);
				if (str.find("tr") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTransferImages", true);
				if (str.find("ds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDrawStats", true);
				if (str.find("fs") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrameStats", true);
				if (str.find("hw") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveHWConfig", true);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprange"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprangef"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirhw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirsw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-loop"))
			{
				s_loop_count = StringUtil::FromChars<s32>(argv[++i]).value_or(0);
				Console.WriteLn("Looping dump playback %d times.", s_loop_count);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderer"))
			{
				const char* rname = argv[++i];

				GSRendererType type = GSRendererType::Auto;
				if (StringUtil::Strcasecmp(rname, "Auto") == 0)
					type = GSRendererType::Auto;
#ifdef _WIN32
				else if (StringUtil::Strcasecmp(rname, "dx11") == 0)
					type = GSRendererType::DX11;
				else if (StringUtil::Strcasecmp(rname, "dx12") == 0)
					type = GSRendererType::DX12;
#endif
#ifdef ENABLE_OPENGL
				else if (StringUtil::Strcasecmp(rname, "gl") == 0)
					type = GSRendererType::OGL;
#endif
#ifdef ENABLE_VULKAN
				else if (StringUtil::Strcasecmp(rname, "vulkan") == 0)
					type = GSRendererType::VK;
#endif
#ifdef __APPLE__
				else if (StringUtil::Strcasecmp(rname, "metal") == 0)
					type = GSRendererType::Metal;
#endif
				else if (StringUtil::Strcasecmp(rname, "sw") == 0)
					type = GSRendererType::SW;
				else
				{
					Console.Error("Unknown renderer '%s'", rname);
					return false;
				}

				Console.WriteLn("Using %s renderer.", Pcsx2Config::GSOptions::GetRendererName(type));
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(type));
				continue;
			}
			else if (CHECK_ARG_PARAM("-swthreads"))
			{
				const int swthreads = StringUtil::FromChars<int>(argv[++i]).value_or(0);
				if (swthreads < 0)
				{
					Console.WriteLn("Invalid number of software threads");
					return false;
				}
				
				Console.WriteLn(fmt::format("Setting number of software threads to {}", swthreads));
				s_settings_interface.SetIntValue("EmuCore/GS", "SWExtraThreads", swthreads);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderhacks"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);

				if (str.find("af") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_AutoFlushLevel", 1);
				if (str.find("cpufb") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_CPU_FB_Conversion", true);
				if (str.find("dds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", true);
				if (str.find("dpi") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisablePartialInvalidation", true);
				if (str.find("dsf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_Disable_Safe_Features", true);
				if (str.find("tinrt") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", 1);
				if (str.find("plf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", true);

				continue;
			}
			else if (CHECK_ARG_PARAM("-ini"))
			{
				std::string path = std::string(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::FileExists(path.c_str()))
				{
					Console.ErrorFmt("INI file {} does not exit.", path);
					return false;
				}

				INISettingsInterface si_ini(path);

				if (!si_ini.Load())
				{
					Console.ErrorFmt("Unable to load INI settings from {}.", path);
					return false;
				}

				for (const auto& [key, value] : si_ini.GetKeyValueList("EmuCore/GS"))
					s_settings_interface.SetStringValue("EmuCore/GS", key.c_str(), value.c_str());

				continue;
			}
			else if (CHECK_ARG_PARAM("-upscale"))
			{
				const float upscale = StringUtil::FromChars<float>(argv[++i]).value_or(0.0f);
				if (upscale < 0.5f)
				{
					Console.WriteLn("Invalid upscale multiplier");
					return false;
				}

				Console.WriteLn(fmt::format("Setting upscale multiplier to {}", upscale));
				s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
				continue;
			}
			else if (CHECK_ARG_PARAM("-logfile"))
			{
				const char* logfile = argv[++i];
				if (std::strlen(logfile) > 0)
				{
					// disable timestamps, since we want to be able to diff the logs
					Console.WriteLn("Logging to %s...", logfile);
					VMManager::Internal::SetFileLogPath(logfile);
					s_settings_interface.SetBoolValue("Logging", "EnableFileLogging", true);
					s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", false);
				}

				continue;
			}
			else if (CHECK_ARG("-noshadercache"))
			{
				Console.WriteLn("Disabling shader cache");
				s_settings_interface.SetBoolValue("EmuCore/GS", "DisableShaderCache", true);
				continue;
			}
			else if (CHECK_ARG("-window"))
			{
				Console.WriteLn("Creating window");
				s_use_window = true;
				continue;
			}
			else if (CHECK_ARG("-surfaceless"))
			{
				Console.WriteLn("Running surfaceless");
				s_use_window = false;
				continue;
			}
			else if (CHECK_ARG("-perf"))
			{
				Console.WriteLn("Enable performance stats");
				s_perf_enable = true;
				continue;
			}
			else if (CHECK_ARG("-debugdevice"))
			{
				Console.WriteLn("Enable debug device");
				s_settings_interface.SetBoolValue("EmuCore/GS", "UseDebugDevice", true);
				continue;
			}
			else if (CHECK_ARG("--"))
			{
				no_more_args = true;
				continue;
			}
			else if (argv[i][0] == '-')
			{
				Console.Error("Unknown parameter: '%s'", argv[i]);
				return false;
			}

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
		}

		if (!params.filename.empty())
			params.filename += ' ';
		params.filename += argv[i];
	}

	if (params.filename.empty())
	{
		Console.Error("No dump filename provided.");
		return false;
	}

	if (!VMManager::IsGSDumpFileName(params.filename))
	{
		Console.Error("Provided filename is not a GS dump.");
		return false;
	}

	if (s_settings_interface.GetBoolValue("EmuCore/GS", "DumpGSData") && !dumpdir.empty())
	{
		if (s_settings_interface.GetStringValue("EmuCore/GS", "HWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", dumpdir.c_str());
		if (s_settings_interface.GetStringValue("EmuCore/GS", "SWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", dumpdir.c_str());
		
		// Disable saving frames with SaveSnapshotToMemory()
		// Instead we save more "raw" snapshots when using -dump.
		s_output_prefix = "";
	}

	// set up the frame dump directory
	if (!s_output_prefix.empty())
	{
		// strip off all extensions
		std::string_view title(Path::GetFileTitle(params.filename));
		if (StringUtil::EndsWithNoCase(title, ".gs"))
			title = Path::GetFileTitle(title);

		s_output_prefix = Path::Combine(s_output_prefix, StringUtil::StripWhitespace(title));
		Console.WriteLn(fmt::format("Saving dumps as {}_frameN.png", s_output_prefix));
	}

	return true;
}

void GSRunner::SettingsOverride()
{
	// complete as quickly as possible
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", false);

	// Force screenshot quality settings to something more performant, overriding any defaults good for users.
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotQuality", 10);

	// ensure all input sources are disabled, we're not using them
	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);

	// we don't need any sound output
	s_settings_interface.SetStringValue("SPU2/Output", "OutputModule", "nullout");
	s_settings_interface.SetStringValue("SPU2/Output", "Backend", "Null"); // sound only in the real game (ps2xgs_start)

	// none of the bindings are going to resolve to anything
	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	// force logging
	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", !s_no_console);
	s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", true);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", true);

	// nothing of PCSX2's own on-screen display in the picture (FPS/resolution only when switched on in the menu, F9)
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowFPS", false);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowResolution", false);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowGSStats", false);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowSettings", false);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowInputs", false);

	// remove memory cards, so we don't have sharing violations
	for (u32 i = 0; i < 2; i++)
	{
		s_settings_interface.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
		s_settings_interface.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
	}
}

void GSRunner::DumpStats()
{
	std::atomic_thread_fence(std::memory_order_acquire);
	Console.WriteLn(fmt::format("======= HW STATISTICS FOR {} ({}) FRAMES ========", s_total_frames, s_total_drawn_frames));
	Console.WriteLn(fmt::format("@HWSTAT@ Draw Calls: {} (avg {})", s_total_draws, static_cast<u64>(std::ceil(s_total_draws / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Render Passes: {} (avg {})", s_total_render_passes, static_cast<u64>(std::ceil(s_total_render_passes / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers: {} (avg {})", s_total_barriers, static_cast<u64>(std::ceil(s_total_barriers / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies: {} (avg {})", s_total_copies, static_cast<u64>(std::ceil(s_total_copies / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Uploads: {} (avg {})", s_total_uploads, static_cast<u64>(std::ceil(s_total_uploads / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Readbacks: {} (avg {})", s_total_readbacks, static_cast<u64>(std::ceil(s_total_readbacks / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies (ROV): {} (avg {})", s_total_copies_rov, static_cast<u64>(std::ceil(s_total_copies_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Draws Calls (ROV): {} (avg {})", s_total_draws_rov, static_cast<u64>(std::ceil(s_total_draws_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers (ROV): {} (avg {})", s_total_barriers_rov, static_cast<u64>(std::ceil(s_total_barriers_rov / static_cast<double>(s_total_drawn_frames)))));
	if (s_perf_enable)
	{
		Console.WriteLn(fmt::format("@HWSTAT@ Minimum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMinimumFrameTime(), 1000.0f / PerformanceMetrics::GetMinimumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Average Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetAverageFrameTime(), 1000.0f / PerformanceMetrics::GetAverageFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Maximum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMaximumFrameTime(), 1000.0f / PerformanceMetrics::GetMaximumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ CPU Thread Usage: {:.3f} %", s_perf_sum_cpu_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GS Thread Usage: {:.3f} %", s_perf_sum_gs_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Usage: {:.3f} %", s_perf_sum_gpu_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average CPU Thread Time: {:.3f} ms", s_perf_sum_cpu_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GS Thread Time: {:.3f} ms", s_perf_sum_gs_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GPU Time: {:.3f} ms", s_perf_sum_gpu_time / s_perf_updates));
	}
	Console.WriteLn("============================================");
}

#ifdef _WIN32
// We can't handle unicode in filenames if we don't use wmain on Win32.
#define main real_main
#endif

static void CPUThreadMain(VMBootParameters* params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// apply new settings (e.g. pick up renderer change)
		VMManager::ApplySettings();
		GSDumpReplayer::SetIsDumpRunner(true);

		if (VMManager::Initialize(*params) == VMBootResult::StartupSuccess)
		{
			// run until end
			GSDumpReplayer::SetLoopCount(s_loop_count);
			VMManager::SetState(VMState::Running);
			if (s_perf_enable)
			{
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
				g_gs_device->SetGPUTimingEnabled(true);
			}
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute();
			VMManager::Shutdown(false);
			GSRunner::DumpStats();
			ret->store(EXIT_SUCCESS);
		}
	}

	VMManager::Internal::CPUThreadShutdown();
	GSRunner::StopPlatformMessagePump();
}

void Host::PumpMessagesOnCPUThread()
{
	// update GS thread copy of frame number
	MTGS::RunOnGSThread([frame_number = GSDumpReplayer::GetFrameNumber()]() { s_dump_frame_number = frame_number; });
	MTGS::RunOnGSThread([loop_number = GSDumpReplayer::GetLoopCount()]() { s_loop_number = loop_number; });
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

//////////////////////////////////////////////////////////////////////////
// Platform specific code
//////////////////////////////////////////////////////////////////////////

#if defined(__APPLE__)

static void* s_window;
static WindowInfo s_wi;

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window, "Tried to create window when there already was one!");
	s_window = CocoaTools::CreateWindow("PCSX2 GS Runner", WINDOW_WIDTH, WINDOW_HEIGHT);
	CocoaTools::GetWindowInfoFromWindow(&s_wi, s_window);
	PumpPlatformMessages();
	return s_window;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_window) {
		CocoaTools::DestroyWindow(s_window);
		s_window = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	CocoaTools::RunCocoaEventLoop(forever);
}

void GSRunner::StopPlatformMessagePump()
{
	CocoaTools::StopMainThreadEventLoop();
}

#elif defined(_WIN32) || defined(__linux__)
#ifdef _WIN32
// ps2x Windows: same game menu/input logic as on Linux, window through Win32 (own UI thread)
#include <windowsx.h>
#include <direct.h>
#include <sys/stat.h>
using Display = void;
using Window = HWND;
using KeySym = unsigned long;
#ifndef None
#define None 0
#endif
static Display* s_display = nullptr; // = window exists (points to the HWND)
static Window s_window = None;
static int ps2x_mkdir(const char* p, int) { return _mkdir(p); }
static int ps2x_setenv(const char* k, const char* v, int ow) { if (!ow && std::getenv(k)) return 0; return _putenv_s(k, v); }
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
// X11 key symbols (same values) so that the shared key logic stays unchanged
enum : KeySym { XK_BackSpace = 0xff08, XK_Tab = 0xff09, XK_Return = 0xff0d, XK_Escape = 0xff1b, XK_Left = 0xff51, XK_Up = 0xff52,
	XK_Right = 0xff53, XK_Down = 0xff54, XK_Print = 0xff61, XK_KP_Enter = 0xff8d, XK_F1 = 0xffbe, XK_F7 = 0xffc4, XK_F8 = 0xffc5, XK_F9 = 0xffc6, XK_F10 = 0xffc7,
	XK_F11 = 0xffc8, XK_F12 = 0xffc9, XK_Shift_L = 0xffe1, XK_Shift_R = 0xffe2, XK_space = 0x20,
	XK_A = 'A', XK_C = 'C', XK_D = 'D', XK_E = 'E', XK_F = 'F', XK_G = 'G', XK_I = 'I', XK_J = 'J', XK_K = 'K', XK_L = 'L', XK_Q = 'Q',
	XK_R = 'R', XK_S = 'S', XK_V = 'V', XK_W = 'W', XK_X = 'X', XK_Y = 'Y', XK_Z = 'Z',
	XK_a = 'a', XK_c = 'c', XK_d = 'd', XK_e = 'e', XK_f = 'f', XK_g = 'g', XK_i = 'i', XK_j = 'j', XK_k = 'k', XK_l = 'l', XK_q = 'q',
	XK_r = 'r', XK_s = 's', XK_v = 'v', XK_w = 'w', XK_x = 'x', XK_y = 'y', XK_z = 'z' };
#else
#include <sys/stat.h>
static Display* s_display = nullptr;
static Window s_window = None;
static int ps2x_mkdir(const char* p, int m) { return mkdir(p, static_cast<mode_t>(m)); }
static int ps2x_setenv(const char* k, const char* v, int ow) { return setenv(k, v, ow); }
#endif
static WindowInfo s_wi;
static std::atomic<bool> s_shutdown_requested{false};

// ---- ps2x: game menu (ImGui in the game window, gamepad/keyboard/mouse), settings in ~/.config/<name> ----
#include "imgui.h"
#include "common/HTTPDownloader.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#endif
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/GS/Renderers/HW/GSTextureReplacements.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include <filesystem>
#include <set>
#include <fstream>
#include <sstream>
#include <vector>
#include <fstream>
#include <sys/stat.h>
namespace ps2xosd { std::atomic<u32> btns{0}, axes{0x80808080u}, keyBtns{0}; std::atomic<bool> ego{false}, zoomAktiv{false}, mausDa{false};
	// Online+ impact: time of the last hit / kill reported by the game (steady_clock ms), counters to detect new events
	std::atomic<long long> trefferMs{0}, killMs{0}; std::atomic<int> trefferN{0}, killN{0};
	// Online+ arcade layer: damage edge flash, multi-kill / streak announcer
	std::atomic<long long> schadenMs{0}; std::atomic<int> schadenStaerke{0}, todN{0};
	std::mutex am; std::string ansText; long long ansMs = 0; int ansStufe = 0; int kette = 0, serie = 0; long long letzterKillMs = 0;
	long long jetztMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); } }
// Online+ fonts (SIL OFL, shipped in resources/fonts): Michroma = wide arcade headline face, Rajdhani = compact text face.
// Loaded lazily on the GUI thread; reloaded if the font atlas was rebuilt. nullptr -> default font.
namespace ps2xschrift
{
	ImFont* laden(const char* datei, ImFont*& f)
	{
		ImFontAtlas* at = ImGui::GetIO().Fonts;
		if (f) { for (ImFont* x : at->Fonts) if (x == f) return f; f = nullptr; }
		static int fehlversuche = 0;
		if (fehlversuche > 8) return nullptr;
		const std::string p = EmuFolders::GetOverridableResourcePath(std::string("fonts" FS_OSPATH_SEPARATOR_STR) + datei);
		ImFontConfig fc; fc.OversampleH = 2;
		f = at->AddFontFromFileTTF(p.c_str(), 0.0f, &fc);
		if (!f) ++fehlversuche;
		return f;
	}
	ImFont* kopf() { static ImFont* f = nullptr; return laden("Michroma-Regular.ttf", f); }
	ImFont* text() { static ImFont* f = nullptr; return laden("Rajdhani-SemiBold.ttf", f); }
	ImFont* fett() { static ImFont* f = nullptr; return laden("Rajdhani-Bold.ttf", f); }
}
// Online+ progression (local): XP for hits/kills, levels, saved in stats.ini next to the settings
namespace ps2xstats { void treffer(); void kill(); int level(); long long xp(); long long xpFuer(int lvl); int kills(); int hits(); } // input last delivered to the game
// Online+ ranks: one title per 5 levels (level 50 = Legend)
static const char* ps2xRang(int lv)
{
	static const char* r[] = {"RECRUIT", "PRIVATE", "CORPORAL", "SERGEANT", "LIEUTENANT", "CAPTAIN", "MAJOR", "COLONEL", "COMMANDER", "GENERAL", "LEGEND"};
	return r[std::clamp(lv / 5, 0, 10)];
}
static ImU32 ps2xRangFarbe(int lv)
{
	static const ImU32 f[] = {IM_COL32(170, 170, 170, 255), IM_COL32(150, 200, 120, 255), IM_COL32(110, 200, 170, 255), IM_COL32(100, 170, 255, 255), IM_COL32(150, 130, 255, 255),
		IM_COL32(220, 120, 255, 255), IM_COL32(255, 120, 170, 255), IM_COL32(255, 150, 80, 255), IM_COL32(255, 200, 60, 255), IM_COL32(255, 230, 120, 255), IM_COL32(255, 245, 200, 255)};
	return f[std::clamp(lv / 5, 0, 10)];
}
// Online+ killfeed / scoreboard data from the game (ps2xgs_text)
namespace ps2xfeed
{
	struct Zeile { std::string killer, opfer; int fl; long long ms; };
	std::mutex m; std::vector<Zeile> zeilen; std::string tafel;
	std::atomic<bool> tab{false};          // Tab held (keyboard) -> Online+ scoreboard
	std::atomic<long long> tafelMs{0};     // last scoreboard data from the game (= a match is running)
	bool imMatch() { return ps2xosd::jetztMs() - tafelMs.load() < 2000; }
}
// Screenshots at internal resolution: with the aspect ratio "fill the window" (Stretch) PCSX2 does not know the display
// ratio and writes the file as 4:3 - the wide picture came out squeezed. Hand over the window ratio for the file.
static void ps2xShotAspect()
{
	if (EmuConfig.GS.AspectRatio == AspectRatioType::Stretch && s_wi.surface_width && s_wi.surface_height)
		EmuConfig.CurrentCustomAspectRatio = static_cast<float>(s_wi.surface_width) / static_cast<float>(s_wi.surface_height);
	else
		EmuConfig.CurrentCustomAspectRatio = 0.0f;
}
// Hotkeys: F9 FPS-Anzeige, F10 Turbo (ohne Bildratenbegrenzung), F12 Screenshot; kurze Einblendung (Toast)
// ps2x: MTGS::RunOnGSThread writes the MTGS ring, which has exactly one producer: the CPU thread (live playback).
// Calls from other threads (screenshots from the port, hotkeys, recording start/stop) raced with the GIF packets of
// the CPU thread and corrupted the ring: garbage transfers (black picture) and jumps to invalid addresses (GS crash).
// gsCall() hands such work to the CPU thread, which passes it on in its hook (ps2xmenu::cpuHook).
namespace ps2xgsq
{
	std::mutex m;
	std::vector<std::function<void()>> q;
	std::atomic<bool> aktiv{false};
	std::atomic<std::thread::id> cpu{};
	void drain()
	{
		cpu.store(std::this_thread::get_id());
		aktiv.store(true);
		std::vector<std::function<void()>> v;
		{ std::lock_guard<std::mutex> lk(m); v.swap(q); }
		for (auto& f : v) MTGS::RunOnGSThread(std::move(f));
	}
}
static void gsCall(std::function<void()> f)
{
	if (!ps2xgsq::aktiv.load() || std::this_thread::get_id() == ps2xgsq::cpu.load())
	{
		MTGS::RunOnGSThread(std::move(f));
		return;
	}
	std::lock_guard<std::mutex> lk(ps2xgsq::m);
	ps2xgsq::q.push_back(std::move(f));
}

namespace ps2xhot
{
	std::atomic<bool> turbo{false};
	std::mutex tm;
	std::string toastText;
	std::chrono::steady_clock::time_point toastAt{};
	void toast(std::string t) { std::lock_guard<std::mutex> lk(tm); toastText = std::move(t); toastAt = std::chrono::steady_clock::now(); }
	// Video recording (PCSX2 GSCapture through ffmpeg): F8 on/off, PS2XGS_CAPTURE=<file.mp4> starts at the first frame.
	// Internal resolution, 60 frames/s, with sound. Codec/bitrate: PS2XGS_CAPTURE_CODEC (default libx264), PS2XGS_CAPTURE_KBPS (default 40000).
	// Free camera (dev mode): F7 on/off, PS2XGS_FREECAM=1 from the start. The runtime then flies the player through the
	// level with mouse + WASD (Space/Ctrl up/down, Shift faster); game input is frozen.
	std::atomic<bool> freecam{[] { const char* e = std::getenv("PS2XGS_FREECAM"); return e && *e == '1'; }()};
	std::atomic<bool> aufnahme{false};
	std::string aufnahmeDatei;
	void aufnahmeStart(std::string datei)
	{
		aufnahmeDatei = datei;
		aufnahme.store(true);
		// 59.94 Hz directly: in the bridge's live mode the GS state does not always know the TV mode (SMODE1 still empty),
		// then GSBeginCapture returns fps = 0 and the encoder refuses ("timebase not set").
		gsCall([datei]() {
			if (!g_gs_renderer) return;
			// Recording settings directly in GSConfig (the values from toSettings do not arrive here)
			const auto envOr = [](const char* k, const char* d) { const char* e = std::getenv(k); return (e && *e) ? e : d; };
			GSConfig.VideoCaptureCodec = envOr("PS2XGS_CAPTURE_CODEC", "libx264");
			GSConfig.VideoCaptureBitrate = std::atoi(envOr("PS2XGS_CAPTURE_KBPS", "40000"));
			GSConfig.EnableVideoCaptureParameters = true;
			GSConfig.VideoCaptureParameters = envOr("PS2XGS_CAPTURE_PARAMS", "crf=16:preset=veryfast");
			GSConfig.CaptureContainer = "mp4";
			GSConfig.EnableVideoCapture = true;
			// Sound: off in unpaced test runs (PS2XGS_CAPTURE_AUDIO=0) - otherwise the sound runs away from the picture, the audio
			// buffer blocks the game thread in DeliverAudioPacket and everything stalls (gdb stacks ts2_366)
			GSConfig.EnableAudioCapture = std::atoi(envOr("PS2XGS_CAPTURE_AUDIO", "1")) != 0;
			GSConfig.AudioCaptureCodec = "aac";
			GSConfig.AudioCaptureBitrate = 192;
#ifdef _WIN32
			// The FFmpeg DLLs are in <gs>\ffmpeg\ and NOT on the search path: DLLs of the same name in PATH could otherwise be
			// loaded by foreign modules in the process (overlays, capture hooks). Preload them here by full path; PCSX2 then finds them.
			{
				const std::string root = envOr("PS2XGS_APPROOT", ".");
				for (const char* n : {"avutil-60.dll", "swresample-6.dll", "swscale-9.dll", "avcodec-62.dll", "avformat-62.dll"})
				{
					const std::wstring w = StringUtil::UTF8StringToWideString(root + "\\ffmpeg\\" + n);
					LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				}
			}
#endif
			GSVector2i res = g_gs_renderer->GetInternalResolution();
			if (res.x <= 0 || res.y <= 0) // no frame yet: derive from scale x PS2 picture (640x448)
				res = GSVector2i(static_cast<int>(640 * EmuConfig.GS.UpscaleMultiplier), static_cast<int>(448 * EmuConfig.GS.UpscaleMultiplier));
			// Limit the recording height (default 1080 lines, PS2XGS_CAPTURE_HOEHE): at full 6x/8x resolution x264 cannot do
			// 60 frames/s; the game then waits for the encoder (low frame rate, and with sound the audio buffer blocks)
			const int maxH = std::max(448, std::atoi(envOr("PS2XGS_CAPTURE_HOEHE", "1080")));
			if (res.y > maxH) { res.x = static_cast<int>(static_cast<long long>(res.x) * maxH / res.y); res.y = maxH; }
			res.x &= ~1; res.y &= ~1;
			const float aspekt = (EmuConfig.GS.AspectRatio == AspectRatioType::R16_9) ? 16.0f / 9.0f : (EmuConfig.GS.AspectRatio == AspectRatioType::Stretch ? 0.0f : 4.0f / 3.0f);
			GSConfig.VideoCaptureAutoResolution = false;
			GSConfig.VideoCaptureWidth = res.x; GSConfig.VideoCaptureHeight = res.y;
			GSCapture::BeginCapture(60.0f / 1.001f, res, aspekt, datei);
		});
		toast("Recording: " + datei + "  (F8 stops)");
	}
	// On exit: finish a running recording cleanly (moov atom), otherwise the file is unreadable
	void aufnahmeBeenden()
	{
		if (!aufnahme.exchange(false)) return;
		// shared: if the wait times out, the call must not write into this (then gone) stack frame
		auto fertig = std::make_shared<std::atomic<bool>>(false);
		gsCall([fertig]() { GSCapture::EndCapture(); fertig->store(true); });
		for (int i = 0; i < 600 && !fertig->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	void aufnahmeStopp()
	{
		aufnahme.store(false);
		gsCall([]() { GSCapture::EndCapture(); });
		toast("Recording saved: " + aufnahmeDatei);
	}
	std::string shotDir()
	{
		if (const char* e = std::getenv("PS2XGS_SHOTDIR"); e && *e) return e;
#ifdef _WIN32
		const char* home = std::getenv("USERPROFILE");
		const std::string h = home ? home : ".";
#else
		const char* home = std::getenv("HOME");
		const std::string h = home ? home : "/tmp";
#endif
		struct stat sb{};
		for (const char* sub : {"/Pictures", "/Bilder"})
			if (stat((h + sub).c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) { const std::string d = h + sub + "/TimeSplitters 2"; ps2x_mkdir(d.c_str(), 0755); return d; }
		const std::string d = h + "/.local/share/ts2-recomp-screenshots"; ps2x_mkdir(d.c_str(), 0755); return d;
	}
}
// Detect the Steam Deck / SteamOS game mode (Steam sets SteamDeck=1; otherwise board vendor "Valve")
static bool ps2xAufSteamDeck()
{
	static const bool deck = [] {
		if (const char* e = std::getenv("SteamDeck"); e && *e == '1') return true;
#ifndef _WIN32
		if (std::FILE* f = std::fopen("/sys/devices/virtual/dmi/id/board_vendor", "r"))
		{
			char b[32] = {};
			const size_t n = std::fread(b, 1, sizeof(b) - 1, f);
			std::fclose(f);
			return n >= 5 && std::string_view(b, 5) == "Valve";
		}
#endif
		return false;
	}();
	return deck;
}

// Overlays inside the game picture (not the window): PCSX2 letter-/pillarboxes the picture; everything drawn
// through ps2xImSpiel uses the picture size and is shifted into it when the scope ends.
void ps2xSpielRect(float* out4);   // GSRenderer.cpp
struct ps2xImSpiel
{
	ImDrawList* dl; int v0; ImVec2 o, ds;
	explicit ps2xImSpiel(ImDrawList* d) : dl(d), v0(d->VtxBuffer.Size)
	{
		const ImGuiIO& io = ImGui::GetIO();
		float r[4] = {0, 0, 0, 0}; ps2xSpielRect(r);
		const float ww = g_gs_device ? static_cast<float>(g_gs_device->GetWindowWidth()) : io.DisplaySize.x;
		const float sk = ww > 0.0f ? io.DisplaySize.x / ww : 1.0f;
		if (r[2] - r[0] < 16.0f || r[3] - r[1] < 16.0f) { o = ImVec2(0, 0); ds = io.DisplaySize; return; }
		o = ImVec2(std::max(0.0f, r[0] * sk), std::max(0.0f, r[1] * sk));
		ds = ImVec2(std::min(io.DisplaySize.x - o.x, (r[2] - r[0]) * sk), std::min(io.DisplaySize.y - o.y, (r[3] - r[1]) * sk));
	}
	~ps2xImSpiel() { for (int i = v0; i < dl->VtxBuffer.Size; ++i) { dl->VtxBuffer[i].pos.x += o.x; dl->VtxBuffer[i].pos.y += o.y; } }
};
namespace ps2xmenu
{
	std::atomic<bool> open{false};
	std::atomic<bool> applyPending{false};
	std::atomic<bool> fsPending{false};
	std::atomic<u32> nav{0};          // held menu keys: 1 up 2 down 4 left 8 right 16 OK 32 back
	std::atomic<int> lernAktion{-1}, lernPlatz{0};   // Binding: next key/mouse button for this action
	std::atomic<double> lernAb{0.0};                  // ImGui time from which input counts (not the click itself)
	std::atomic<int> lernCode{0};                     // code captured by the window thread (0 = none yet, -1 = cancelled)
	std::atomic<int> mouseX{-1}, mouseY{-1};
	std::atomic<u32> mouseBtn{0};
	std::atomic<int> wheel{0};
	std::atomic<bool> latch{false};   // after closing: game input only after everything is released
	std::atomic<bool> gameWide{false};
	std::atomic<bool> active{false};   // menu/settings active (not in windowless test runs) // the game renders in its own 16:9 mode (set_screen_wide_and_progressive)

	// Game-specific texts through the environment (start script of each port), default: SotC
	const char* envOr(const char* k, const char* d) { const char* e = std::getenv(k); return (e && *e) ? e : d; }
	const char* spielTitel() { return envOr("PS2XGS_TITEL", "SHADOW OF THE COLOSSUS"); }
	const char* cfgName() { return envOr("PS2XGS_CFGNAME", "sotc-recomp"); }
	// Edition: "onlineplus" = TimeSplitters 2 Online+ (separate game: impact effects, progression, Online+ menu); otherwise original
	bool onlinePlus() { static const bool v = [] { const char* e = std::getenv("PS2XGS_EDITION"); const char* t = std::getenv("TS2_IMPACT"); return (e && std::string_view(e) == "onlineplus") || (t && *t == '1'); }(); return v; }
	std::atomic<bool> opOeffnen{false};   // the game's main menu item "Online+" was selected
	std::atomic<int> botAnfrage{0};       // quick bot match requested in the hub (code for the runtime, option 18)
	const char* startName() { return envOr("PS2XGS_STARTNAME", "the launcher"); }
	const char* speicherHinweis() { return envOr("PS2XGS_SPEICHERHINWEIS", "Saving works in the game as on the PS2."); }

	// ---- Menuesprache: Quelltexte englisch, deutsche Uebersetzung per Tabelle (Einstellung "sprache", -1 = automatisch) ----
	std::atomic<int> sprache{0}; // 0 English, 1 Deutsch
	int spracheAuto()
	{
#ifdef _WIN32
		return (GetUserDefaultUILanguage() & 0x3FF) == 0x07 ? 1 : 0;
#else
		const char* l = std::getenv("LC_ALL"); if (!l || !*l) l = std::getenv("LC_MESSAGES"); if (!l || !*l) l = std::getenv("LANG");
		return (l && l[0] == 'd' && l[1] == 'e') ? 1 : 0;
#endif
	}
	const char* tr(const char* en)
	{
		if (sprache.load() != 1) return en;
		static const std::unordered_map<std::string_view, const char*> de = {
			{"Settings", "Einstellungen"}, {"Gamepad", "Gamepad"}, {"Turn speed", "Drehgeschwindigkeit"}, {"Modern aiming (right stick turns the view, crosshair fixed in the centre)", "Modernes Zielen (rechter Stick dreht die Sicht, Fadenkreuz fest in der Mitte)"}, {"Off = the original PS2 controls. On: no auto-aim, L2 zooms, shots land on the crosshair.", "Aus = die Original-PS2-Steuerung. An: kein Auto-Aim, L2 zoomt, Schuesse treffen das Fadenkreuz."}, {"Unlock everything (levels, characters, cheats)", "Alles freischalten (Level, Charaktere, Cheats)"}, {"Only while this is on. Your own progress and save data stay untouched.", "Nur solange dies an ist. Eigener Fortschritt und Spielstand bleiben unveraendert."}, {"Saving works in the game as on the PS2.", "Gespeichert wird im Spiel wie auf der PS2."}, {"Saving works in the game as on the PS2 (profile, options and progress).", "Gespeichert wird im Spiel wie auf der PS2 (Profil, Optionen und Fortschritt)."}, {"Display", "Anzeige"}, {"Picture", "Bild"}, {"Audio", "Ton"}, {"Controls", "Steuerung"},
			{"Textures", "Texturen"}, {"Online", "Online"}, {"Save data", "Speicherstand"}, {"Language", "Sprache"},
			{"Preset", "Voreinstellung"}, {"Remaster (sharp)", "Remaster (scharf)"}, {"Original look", "Original-Look"}, {"Performance", "Leistung"},
			{"Internal resolution", "Interne Aufloesung"}, {"Aspect ratio", "Bildformat"}, {"Takes full effect after the next area change or restart.", "Wirkt ab dem naechsten Bereichswechsel bzw. Neustart voll."},
			{"Texture filtering", "Texturfilter"}, {"Anisotropic filtering", "Anisotrope Filterung"}, {"Trilinear filtering", "Trilineare Filterung"},
			{"Fullscreen", "Vollbild"}, {"V-Sync", "V-Sync"}, {"Show FPS", "FPS-Anzeige"}, {"Open this menu at start", "Menue beim Spielstart zeigen"}, {"Input overlay (debug)", "Eingabe-Anzeige (Debug)"},
			{"Resolution & filtering", "Aufloesung & Filter"}, {"Window", "Fenster"},
			{"Original frame blending (ghosting when turning)", "Nachzieheffekt beim Drehen (wie Original)"},
			{"The PS2 game blends the previous frame half over the new one. Off = clear picture.", "Das Original blendet das vorige Bild halb ueber das neue (Geisterbilder). Aus = klares Bild."},
			{"Anti-aliasing (FXAA)", "Kantenglaettung (FXAA)"}, {"Sharpening (AMD CAS)", "Schaerfe (AMD CAS)"}, {"Sharpening strength", "Schaerfe-Staerke"},
			{"PS2 dithering", "PS2-Dithering"}, {"TV / CRT filter", "Bildfilter (TV/CRT)"}, {"Colour", "Farbe"}, {"Colour adjustment", "Farbanpassung"},
			{"Brightness", "Helligkeit"}, {"Contrast", "Kontrast"}, {"Saturation", "Saettigung"}, {"Reset colours", "Farben zuruecksetzen"},
			{"Post effects (vkBasalt)", "Effekte (vkBasalt)"}, {"Applies at the next start. Home key toggles the effects in game. Needs vkBasalt installed.", "Wirkt beim naechsten Spielstart. Pos1 schaltet die Effekte im Spiel an/aus. Benoetigt vkBasalt."},
			{"Volume", "Lautstaerke"}, {"Original sound chip (SPU2) at 48 kHz. Sound is paused while this menu is open.", "Original-Soundchip (SPU2) mit 48 kHz. Im Menue ist der Ton angehalten."},
			{"Gamepad", "Gamepad"}, {"Rumble", "Vibration"}, {"Rumble strength", "Vibrationsstaerke"},
			{"Any modern gamepad is detected automatically (Xbox, PlayStation, Switch, Deck).", "Jedes moderne Gamepad wird automatisch erkannt (Xbox, PlayStation, Switch, Deck)."},
			{"Mouse", "Maus"}, {"Mouse aiming", "Maus-Zielen"}, {"Sensitivity", "Empfindlichkeit"}, {"Zoom sensitivity", "Zoom-Empfindlichkeit"},
			{"Invert vertical", "Oben/unten vertauscht"}, {"Always hold aim mode (original crosshair, slower walking)", "Zielmodus immer halten (Original-Fadenkreuz, langsameres Laufen)"}, {"Crosshair", "Fadenkreuz"}, {"Sniper scope", "Zielfernrohr"}, {"Impact effects (Online+)", "Treffer-Effekte (Online+)"}, {"Extra feedback on top of the original game. Off = the game exactly as on the PS2.", "Zusaetzliches Feedback zum Originalspiel. Aus = das Spiel genau wie auf der PS2."}, {"Hit and kill markers", "Treffer- und Kill-Marker"}, {"Play online", "Online spielen"}, {"Open server browser", "Serverbrowser oeffnen"}, {"Join a server, then pick Network Game in the game.", "Server beitreten, dann im Spiel Netzwerkspiel waehlen."}, {"Online play is not available in this start mode.", "Online-Spiel ist in diesem Startmodus nicht verfuegbar."}, {"Effects", "Effekte"}, {"BOT MATCH", "BOT-MATCH"}, {"* = has to be unlocked in the game first (Arcade League / Challenge).", "* = muss erst im Spiel freigeschaltet werden (Arcade-Liga / Challenge)."}, {"Mode", "Modus"}, {"Map", "Karte"}, {"START BOT MATCH", "BOT-MATCH STARTEN"}, {"Jump straight into a match against bots. Pick a mode and a map, the game sets everything up for you.", "Direkt in ein Match gegen Bots. Modus und Karte waehlen, das Spiel stellt alles fuer dich ein."}, {"Don't touch the controller for a few seconds while the menus run.", "Ein paar Sekunden nichts druecken, waehrend die Menues durchlaufen."}, {"Opponents", "Gegner"}, {"Next rank:", "Naechster Rang:"}, {"Recoil punch when firing", "Rueckstoss beim Schiessen"}, {"Multi-kill and streak announcer", "Ansager fuer Multikills und Serien"}, {"Red screen edge when hit", "Roter Bildschirmrand bei Treffern"}, {"PLAY ONLINE", "ONLINE SPIELEN"}, {"PROFILE & STATS", "PROFIL & STATS"}, {"SETTINGS", "EINSTELLUNGEN"}, {"Total XP", "XP gesamt"}, {"XP to next level", "XP bis zum naechsten Level"}, {"  Back to main menu  ", "  Zurueck zum Hauptmenue  "}, {"All other settings: ESC / F1 during the game.", "Alle weiteren Einstellungen: ESC / F1 im Spiel."}, {"TimeSplitters 2 Online+ - online play, progression and extra impact.", "TimeSplitters 2 Online+ - Online-Spiel, Fortschritt und mehr Wucht."}, {"Hit and kill sounds", "Treffer- und Kill-Sound"}, {"Progress", "Fortschritt"}, {"Kills", "Abschuesse"}, {"Hits", "Treffer"}, {"Hit +10 XP, kill +100 XP. Saved locally; online servers will track it later.", "Treffer +10 XP, Abschuss +100 XP. Lokal gespeichert; Online-Server zaehlen spaeter mit."}, {"Camera kick when hit", "Kamerastoss bei Treffern"}, {"Look up/down limit", "Blick hoch/runter bis"},
			{"Click into the game to capture the mouse, ESC or F1 releases it.", "Klick ins Spiel faengt die Maus, ESC/F1 gibt sie wieder frei."},
			{"Key bindings", "Tastenbelegung"}, {"Click a field, then press a key or mouse button. Delete clears, ESC cancels.", "Feld anklicken, dann Taste oder Maustaste druecken. Entf leert, ESC bricht ab."},
			{"Action", "Aktion"}, {"Primary", "Taste 1"}, {"Secondary", "Taste 2"}, {"... press a key ...", "... Taste druecken ..."}, {"Reset to defaults", "Standardbelegung"},
			{"HD text (own re-typesetting for text pages without a pack)", "HD-Text (eigener Neusatz fuer Textseiten ohne Paket)"},
			{"Texture packs (PCSX2 format)", "Texturpakete (PCSX2-Format)"}, {"no packs found", "keine Pakete gefunden"},
			{"New packs: put the archive into the project's 'texturpakete' folder, %s imports it.", "Neue Pakete: Archiv in den Ordner 'texturpakete' des Projekts legen, %s importiert es."},
			{"Or extracted to %s/<name>/", "Oder entpackt nach %s/<Name>/"}, {"Apply and reload textures", "Uebernehmen und Texturen neu laden"}, {"Reload textures", "Texturen neu laden"},
			{"%u files active", "%u Dateien aktiv"}, {"Large packs are preloaded into video memory (short pause).", "Grosse Pakete werden vorab in den Grafikspeicher geladen (kurze Pause)."},
			{"A backup copy of the save data is made at every start.", "Bei jedem Start wird automatisch eine Sicherungskopie angelegt."},
			{"Memory card: not set", "Speicherkarte: nicht gesetzt"}, {"Memory card: ", "Speicherkarte: "}, {" (still empty)", " (noch leer)"}, {"\nLast saved: ", "\nZuletzt gespeichert: "},
			{"  Back to game  ", "  Weiterspielen  "}, {"  Quit game  ", "  Spiel beenden  "},
			{"ESC / F1 / Guide: menu   F7: free camera   F8: record video   F9: FPS   F10: turbo   F11 or Alt+Enter: fullscreen   F12: screenshot", "ESC / F1 / Guide: Menue   F7: freie Kamera   F8: Video aufnehmen   F9: FPS   F10: Turbo   F11 oder Alt+Enter: Vollbild   F12: Screenshot"},
			{"Menu: ESC / F1   |   Gamepad: Select + Start (or Guide)", "Menue: ESC / F1   |   Gamepad: Select + Start (oder Guide)"},
			// Online
			{"Play online", "Online spielen"}, {"Pick a server and connect. Then confirm \"TS2 Online\" in the game: Start Game hosts, Join Game lists the games on the server.", "Server waehlen und Verbinden. Dann im Spiel \"TS2 Online\" bestaetigen: Start Game hostet, Join Game zeigt die Spiele auf dem Server."},
			{"Player name", "Spielername"}, {"(none)", "(keiner)"}, {"Searching ...", "Suche ..."}, {"Refresh", "Aktualisieren"}, {"not full", "nicht voll"},
			{"no password", "ohne Passwort"}, {"favourites only", "nur Favoriten"}, {"Server", "Server"}, {"Players", "Spieler"}, {"Ping", "Ping"}, {"PW", "PW"},
			{"Message", "Nachricht"}, {" (no answer)", " (keine Antwort)"}, {"  (other version)", "  (andere Version)"}, {"yes", "ja"}, {"  Connect  ", "  Verbinden  "},
			{"address (host:port)", "Adresse (host:port)"}, {"Connect to address", "Direkt verbinden"}, {"Add favourite", "Als Favorit"},
			{"Host your own: ts2-server (see server/ in the project). Permanent listing: server/servers.txt.", "Eigenen Server hosten: ts2-server (siehe server/ im Projekt). Dauerhaft in die Liste: server/servers.txt."},
			{"Selected: ", "Ausgewaehlt: "}, {" - now confirm \"TS2 Online\" in the game", " - jetzt im Spiel \"TS2 Online\" bestaetigen"},
			{"Password", "Passwort"}, {"Password for %s", "Passwort fuer %s"}, {"Cancel", "Abbrechen"},
			// Aktionen
			{"Forward", "Vorwaerts"}, {"Backward", "Rueckwaerts"}, {"Strafe left", "Seitwaerts links"}, {"Strafe right", "Seitwaerts rechts"},
			{"Fire (R2)", "Feuer (R2)"}, {"Secondary fire (R1)", "Zweitfeuer (R1)"}, {"Zoom (hold)", "Zoom (halten)"}, {"Use / action (Cross)", "Benutzen / Aktion (Kreuz)"},
			{"Reload (Triangle)", "Nachladen (Dreieck)"}, {"Crouch (L1)", "Ducken (L1)"}, {"Aim mode (L2)", "Zielmodus (L2)"},
			{"Next weapon (D-pad right)", "Naechste Waffe (Steuerkreuz rechts)"}, {"Previous weapon (D-pad left)", "Vorige Waffe (Steuerkreuz links)"},
			{"Zoom in (D-pad up)", "Heranzoomen (Steuerkreuz hoch)"}, {"Zoom out (D-pad down)", "Wegzoomen (Steuerkreuz runter)"},
			{"Circle", "Kreis"}, {"Square", "Quadrat"}, {"Start (pause / game menu)", "Start (Pause / Spielmenue)"}, {"Select", "Select"},
			{"Look up (right stick)", "Umsehen hoch (rechter Stick)"}, {"Look down", "Umsehen runter"}, {"Look left", "Umsehen links"}, {"Look right", "Umsehen rechts"},
			// Keys
			{"Mouse left", "Maus links"}, {"Mouse middle", "Maus Mitte"}, {"Mouse right", "Maus rechts"}, {"Wheel up", "Mausrad hoch"}, {"Wheel down", "Mausrad runter"},
			{"Mouse side 1", "Maus Seite 1"}, {"Mouse side 2", "Maus Seite 2"}, {"Mouse button {}", "Maustaste {}"}, {"Space", "Leertaste"}, {"Backspace", "Rueck"},
			{"Tab", "Tab"}, {"Enter", "Enter"}, {"Left", "Pfeil links"}, {"Up", "Pfeil hoch"}, {"Right", "Pfeil rechts"}, {"Down", "Pfeil runter"},
			{"Left Shift", "Shift links"}, {"Right Shift", "Shift rechts"}, {"Left Ctrl", "Strg links"}, {"Right Ctrl", "Strg rechts"}, {"Left Alt", "Alt links"}, {"Right Alt", "Alt rechts"},
			{"Caps Lock", "Feststell"}, {"Numpad Enter", "Enter (Ziffernblock)"}, {"Home", "Pos1"}, {"Page Up", "Bild hoch"}, {"Page Down", "Bild runter"}, {"End", "Ende"},
			{"Insert", "Einfg"}, {"Delete", "Entf"}, {"Esc", "Esc"}, {"Print", "Druck"}, {"Numpad +", "+ (Ziffernblock)"}, {"Numpad -", "- (Ziffernblock)"},
			{"Numpad *", "* (Ziffernblock)"}, {"Numpad /", "/ (Ziffernblock)"}, {"Numpad ,", ", (Ziffernblock)"}, {"Numpad {}", "{} (Ziffernblock)"}, {"Key 0x{:x}", "Taste 0x{:x}"},
		};
		const auto it = de.find(en);
		return it == de.end() ? en : it->second;
	}
	// ---- Free bindings: actions and their keys (keyboard = KeySym, mouse = kMaus + button) ----
	constexpr int kMaus = 0x10000000;  // + 1 links, 2 Mitte, 3 rechts, 4 Rad hoch, 5 Rad runter, 8/9 Seitentasten
	enum Wirkung : int { W_TASTE = 0, W_STICK = 1, W_ZOOM = 2 };
	struct Aktion { const char* name; int wirkung; unsigned wert; int std1, std2; };
	// PS2 bits as in libpad (1 = pressed): SEL 0x1, L3 0x2, R3 0x4, START 0x8, UP 0x10, RIGHT 0x20, DOWN 0x40,
	// LINKS 0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800, DREIECK 0x1000, KREIS 0x2000, KREUZ 0x4000, QUADRAT 0x8000.
	// Stick bits: 0-3 left stick (up down left right), 4-7 right stick. Default = TS2 PS2 layout for mouse + keyboard.
	static const Aktion kAktionen[] = {
		{"Forward", W_STICK, 1, 'w', 0}, {"Backward", W_STICK, 2, 's', 0},
		{"Strafe left", W_STICK, 4, 'a', 0}, {"Strafe right", W_STICK, 8, 'd', 0},
		{"Fire (R2)", W_TASTE, 0x0200, kMaus + 1, 0}, {"Secondary fire (R1)", W_TASTE, 0x0800, kMaus + 2, 'q'},
		{"Zoom (hold)", W_ZOOM, 0, kMaus + 3, 0},
		{"Use / action (Cross)", W_TASTE, 0x4000, 'e', ' '},
		{"Reload (Triangle)", W_TASTE, 0x1000, 'r', 0},
		{"Crouch (L1)", W_TASTE, 0x0400, 'c', 0xffe3},
		{"Aim mode (L2)", W_TASTE, 0x0100, 0xffe1, 0},
		{"Next weapon (D-pad right)", W_TASTE, 0x0020, kMaus + 4, 0xff53}, {"Previous weapon (D-pad left)", W_TASTE, 0x0080, kMaus + 5, 0xff51},
		{"Zoom in (D-pad up)", W_TASTE, 0x0010, 0xff52, 0}, {"Zoom out (D-pad down)", W_TASTE, 0x0040, 0xff54, 0},
		{"Circle", W_TASTE, 0x2000, 'x', 0xff08}, {"Square", W_TASTE, 0x8000, 'f', 0},
		{"Start (pause / game menu)", W_TASTE, 0x0008, 0xff0d, 'p'}, {"Select", W_TASTE, 0x0001, 0xff09, 0},
		{"L3", W_TASTE, 0x0002, 0xffe9, 0}, {"R3", W_TASTE, 0x0004, 'g', 0},
		{"Look up (right stick)", W_STICK, 16, 'i', 0}, {"Look down", W_STICK, 32, 'k', 0},
		{"Look left", W_STICK, 64, 'j', 0}, {"Look right", W_STICK, 128, 'l', 0},
	};
	constexpr int kBelegungVersion = 2; // increase when the default bindings change: saved older bindings are then replaced
	constexpr int kAnzAktionen = static_cast<int>(sizeof(kAktionen) / sizeof(kAktionen[0]));
	inline std::string tastenName(int c)
	{
		if (c == 0) return "-";
		if (c >= kMaus)
		{
			switch (c - kMaus) { case 1: return tr("Mouse left"); case 2: return tr("Mouse middle"); case 3: return tr("Mouse right");
				case 4: return tr("Wheel up"); case 5: return tr("Wheel down"); case 8: return tr("Mouse side 1"); case 9: return tr("Mouse side 2");
				default: return fmt::format(fmt::runtime(tr("Mouse button {}")), c - kMaus); }
		}
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return std::string(1, static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
		switch (c)
		{
			case ' ': return tr("Space"); case 0xff08: return tr("Backspace"); case 0xff09: return tr("Tab"); case 0xff0d: return tr("Enter");
			case 0xff51: return tr("Left"); case 0xff52: return tr("Up"); case 0xff53: return tr("Right"); case 0xff54: return tr("Down");
			case 0xffe1: return tr("Left Shift"); case 0xffe2: return tr("Right Shift"); case 0xffe3: return tr("Left Ctrl"); case 0xffe4: return tr("Right Ctrl");
			case 0xffe9: return tr("Left Alt"); case 0xffea: return tr("Right Alt"); case 0xffe5: return tr("Caps Lock"); case 0xff8d: return tr("Numpad Enter");
			case 0xff50: return tr("Home"); case 0xff55: return tr("Page Up"); case 0xff56: return tr("Page Down"); case 0xff57: return tr("End");
			case 0xff63: return tr("Insert"); case 0xffff: return tr("Delete"); case 0xff1b: return tr("Esc"); case 0xff61: return tr("Print");
			case 0xffab: return tr("Numpad +"); case 0xffad: return tr("Numpad -"); case 0xffaa: return tr("Numpad *");
			case 0xffaf: return tr("Numpad /"); case 0xffae: return tr("Numpad ,");
			default: break;
		}
		if (c >= 0xffbe && c <= 0xffc9) return fmt::format("F{}", c - 0xffbe + 1);
		if (c >= 0xffb0 && c <= 0xffb9) return fmt::format(fmt::runtime(tr("Numpad {}")), c - 0xffb0);
		if (c >= 0x21 && c < 0x7f) return std::string(1, static_cast<char>(c));
		if (c >= 0xa1 && c < 0x100) // Latin-1 (umlauts, ß etc.) as UTF-8
		{
			std::string u; u += static_cast<char>(0xC0 | (c >> 6)); u += static_cast<char>(0x80 | (c & 0x3F)); return u;
		}
		return fmt::format(fmt::runtime(tr("Key 0x{:x}")), c);
	}

	struct Cfg
	{
		int upscale = 9 /* auto: fits the screen, steps down when the PC cannot hold the frame rate */, aspect = 4, cas = 1, casSharp = 25, tv = 0, filter = 2, aniso = 16, dither = 0;
		int sbB = 50, sbC = 50, sbS = 50, rumblePct = 100, hz = 50, volume = 100, camBlur = 1, nachzieh = 0;
		bool fxaa = false, shadeboost = false, fullscreen = false, showFps = false, rumble = true, vsync = false, startMenu = false;   // start menu: the launcher already did the setup
		int tri = 2;             // Trilinear filtering: 0 automatic (PCSX2 default), 1 as PS2, 2 forced (uses the mipmaps of replacement textures)
		int fx = 0;              // Effects (vkBasalt): 0 off, 1 original-plus, 2 clear, 3 cinematic - applies on the next start
		bool hdText = true;      // Re-typeset text pages (TeX Gyre), takes precedence over packs
		bool showInput = false;  // Input display top left (debug), separate from the FPS display
		int lang = -1;           // Menuesprache: -1 automatisch, 0 English, 1 Deutsch
		bool maus = !ps2xAufSteamDeck(); // Mouse aiming: a click into the game window captures the mouse (ESC/F1 releases it); Steam Deck: off
		int mausSens = 30;       // Empfindlichkeit in Zehntel (1.0-10.0)
		bool mausInvert = false; // Mouse: up/down inverted
		bool mausZielmodus = false; // Mouse aiming: hold aim mode (L2) permanently - slows walking (~16 %), so off; zooming holds it by itself
		int zoomSens = 50;       // sensitivity while zooming in % of the normal one
		int scope = 1;          // sniper/zoom smear: 0 original, 1 clear, 2 extra clear
		int impact = 1;         // Online+ impact effects (hit/kill markers, damage kick); only in the Online+ edition
		int impactKick = 50;    // damage camera kick 0..100
		int impactMarker = 1;   // hit markers on/off (with impact on)
		int impactSound = 1;    // hit/kill sound on/off
		int impactRecoil = 40;  // recoil punch on own shots 0..100 (springs back)
		int impactAnsage = 1;   // multi-kill / streak announcer
		int impactRand = 1;     // red screen edge when hit
		int fadenkreuz = 0;      // own crosshair when playing with the mouse: 0 cross, 1 dot, 2 circle, 3 off
		int padSens = 100;       // TS2 modern gamepad aiming: turn speed in percent
		int padModern = 0;       // TS2: modern gamepad aiming (right stick turns the view, crosshair fixed in the centre)
		int unlockAll = 0;       // TS2: everything unlocked while the option is on (levels, characters, cheats)
		int nickMax = 40;        // Mouse: how far up/down you can look (degrees, original 40)
		int bel[32][2] = {};     // bindings per action (2 keys), defaults in kAktionen
		int belVersion = 0;      // version of the saved bindings (0 = none saved)
		Cfg() { for (int i = 0; i < kAnzAktionen; ++i) { bel[i][0] = kAktionen[i].std1; bel[i][1] = kAktionen[i].std2; } belVersion = kBelegungVersion; }
		std::string onlineServer, onlineName, onlinePw, onlineFavs; // Online (TS2): gewaehlter Server, Spielername, Passwort, Favoriten (Komma)
		std::string packs = "*"; // aktive Texturpakete ("*" = alle vorhandenen, Voreinstellung) (Ordnernamen in texturquellen/, durch Komma, Reihenfolge = Prioritaet)
	};
	std::mutex m;
	Cfg cfg;
	std::atomic<double> applyAt{0.0}; // time (ImGui time) from which changed settings are applied

	std::string dir()
	{
#ifdef _WIN32
		const char* xdg = std::getenv("APPDATA");
		const char* home = std::getenv("USERPROFILE");
#else
		const char* xdg = std::getenv("XDG_CONFIG_HOME");
		const char* home = std::getenv("HOME");
#endif
		std::string d = (xdg && *xdg) ? std::string(xdg) : std::string(home ? home : "/tmp") + "/.config";
		d += "/"; d += cfgName();
		ps2x_mkdir(d.c_str(), 0755);
		return d;
	}
	bool cfgEnglish();
	std::string iniPath()
	{
		// PS2XGS_CFG=<Datei>: andere Einstellungsdatei (Vergleichsbilder in Testlaeufen)
		if (const char* c = std::getenv("PS2XGS_CFG"); c && *c)
			return c;
		return dir() + (cfgEnglish() ? "/settings.ini" : "/einstellungen.ini");
	}
	// PS2XGS_CFG_EN=1 (TS2 packages): settings file settings.ini with English keys. Both spellings are always read;
	// if settings.ini is missing, the old einstellungen.ini is taken over once.
	bool cfgEnglish() { const char* e = std::getenv("PS2XGS_CFG_EN"); return e && *e == '1'; }
	static constexpr const char* kKeyMap[][2] = {
		{"aufloesung", "resolution"}, {"bildformat", "aspect"}, {"cas_staerke", "cas_strength"}, {"farbe", "color_adjust"},
		{"helligkeit", "brightness"}, {"kontrast", "contrast"}, {"saettigung", "saturation"}, {"texturfilter", "texture_filter"},
		{"anisotrop", "anisotropic"}, {"vollbild", "fullscreen"}, {"fps_anzeige", "show_fps"}, {"vibration_staerke", "vibration_strength"},
		{"startmenue", "start_menu"}, {"bildrate", "refresh_rate"}, {"lautstaerke", "volume"}, {"kamera_unschaerfe", "camera_blur"},
		{"nachzieheffekt", "motion_trail"}, {"maus_zielen", "mouse_aim"}, {"maus_empfindlichkeit", "mouse_sensitivity"},
		{"maus_invertiert", "mouse_invert"}, {"maus_zielmodus_dauer", "aim_mode_hold"}, {"sprache", "language"},
		{"eingabeanzeige", "show_input"}, {"zoom_empfindlichkeit", "zoom_sensitivity"}, {"belegung_version", "bindings_version"},
		{"effekte", "effects"}, {"alles_frei", "unlock_all"}, {"pad_modern", "modern_gamepad_aim"}, {"pad_tempo", "gamepad_aim_speed"}, {"fadenkreuz", "crosshair"}, {"zielfernrohr", "scope"}, {"online_plus_effekte", "impact_effects"}, {"schaden_stoss", "damage_kick"}, {"treffermarker", "hit_markers"}, {"treffer_klang", "hit_sound"}, {"blickwinkel_max", "pitch_limit"}, {"texturpakete", "texture_packs"}, {"online_passwort", "online_password"}, {"online_favoriten", "online_favorites"}};
	static constexpr const char* kFxMap[][2] = {{"aus", "off"}, {"klar", "clear"}, {"filmisch", "cinematic"}};
	// Line "key=value": translate the key (and the effects value) to the other language; nachEnglisch=false -> internal (German) names
	std::string cfgZeile(const std::string& line, bool nachEnglisch)
	{
		const auto eq = line.find('=');
		if (eq == std::string::npos || line.empty() || line[0] == '#') return line;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		const int von = nachEnglisch ? 0 : 1, nach = nachEnglisch ? 1 : 0;
		if (k.rfind(nachEnglisch ? "taste_" : "key_", 0) == 0) k = std::string(nachEnglisch ? "key_" : "taste_") + k.substr(nachEnglisch ? 6 : 4);
		else for (const auto& m : kKeyMap) if (k == m[von]) { k = m[nach]; break; }
		if (k == (nachEnglisch ? "effects" : "effekte")) for (const auto& m : kFxMap) if (v == m[von]) { v = m[nach]; break; }
		return k + "=" + v;
	}

	void belegungPruefen();
	void load(float envUpscale)
	{
		std::lock_guard<std::mutex> lk(m);
		(void)envUpscale; // Standard: Remaster-Preset (4x); ein gespeicherter Wert hat Vorrang
#ifdef __linux__
		{
			// Steam Deck (or another Valve device): weaker GPU -> 2x internal (1280x800 screen), FXAA off
			std::ifstream dv("/sys/devices/virtual/dmi/id/board_vendor");
			std::string vend; std::getline(dv, vend);
			const char* sd = std::getenv("SteamDeck");
			if (vend.rfind("Valve", 0) == 0 || (sd && *sd == '1'))
			{
				cfg.upscale = 2; cfg.fxaa = false;
				Console.WriteLn("[ps2xmenu] Steam Deck detected: default 2x internal, FXAA off (the settings file takes precedence)");
			}
		}
#endif
		bool aspektAlt = false, aspektNeu = false; // 1.0.x saved 16:9 as default -> once to "fits any screen" (no bars on 16:10)
		cfg.belVersion = 0; // if the version is not in the file, bindings saved there count as outdated
		std::ifstream f(iniPath());
		if (!f && cfgEnglish() && !std::getenv("PS2XGS_CFG")) { f.clear(); f.open(dir() + "/einstellungen.ini"); }
		std::string line;
		while (std::getline(f, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			line = cfgZeile(line, false);
			const auto eq = line.find('=');
			if (eq == std::string::npos || line[0] == '#')
				continue;
			const std::string k = line.substr(0, eq);
			const int v = std::atoi(line.c_str() + eq + 1);
			if (k == "aufloesung") cfg.upscale = std::clamp(v, 1, 9);   // 9 = auto (supersampling)
			else if (k == "bildformat") { cfg.aspect = std::clamp(v, 0, 4); aspektAlt = cfg.aspect == 0; }
			else if (k == "bildformat_v2") aspektNeu = true;
			else if (k == "fxaa") cfg.fxaa = v != 0;
			else if (k == "cas") cfg.cas = std::clamp(v, 0, 2);
			else if (k == "cas_staerke") cfg.casSharp = std::clamp(v, 0, 100);
			else if (k == "dithering") cfg.dither = std::clamp(v, 0, 2);
			else if (k == "tv_shader") cfg.tv = std::clamp(v, 0, 7);
			else if (k == "farbe") cfg.shadeboost = v != 0;
			else if (k == "helligkeit") cfg.sbB = std::clamp(v, 1, 100);
			else if (k == "kontrast") cfg.sbC = std::clamp(v, 1, 100);
			else if (k == "saettigung") cfg.sbS = std::clamp(v, 1, 100);
			else if (k == "texturfilter") cfg.filter = std::clamp(v, 0, 3);
			else if (k == "anisotrop") cfg.aniso = std::clamp(v, 0, 16);
			else if (k == "vollbild") cfg.fullscreen = v != 0;
			else if (k == "fps_anzeige") cfg.showFps = v != 0;
			else if (k == "vibration") cfg.rumble = v != 0;
			else if (k == "vibration_staerke") cfg.rumblePct = std::clamp(v, 0, 100);
			else if (k == "vsync") cfg.vsync = v != 0;
			else if (k == "startmenue") cfg.startMenu = v != 0;
			else if (k == "bildrate") cfg.hz = (v == 60 && std::getenv("SOTC_60HZ_EXPERIMENT")) ? 60 : 50; // 60 Hz: character physics still faulty
			else if (k == "lautstaerke") cfg.volume = std::clamp(v, 0, 200);
			else if (k == "kamera_unschaerfe") cfg.camBlur = v != 0 ? 1 : 0;
			else if (k == "nachzieheffekt") cfg.nachzieh = v != 0 ? 1 : 0;
			else if (k == "hd_text") cfg.hdText = v != 0;
			else if (k == "maus_zielen") cfg.maus = v != 0;
			else if (k == "maus_empfindlichkeit") cfg.mausSens = std::clamp(v, 1, 100);
			else if (k == "maus_invertiert") cfg.mausInvert = v != 0;
			else if (k == "maus_zielmodus_dauer") cfg.mausZielmodus = v != 0; // the old key maus_zielmodus (default on) is ignored on purpose
			else if (k == "sprache") cfg.lang = std::clamp(v, -1, 1);
			else if (k == "eingabeanzeige") cfg.showInput = v != 0;
			else if (k == "zoom_empfindlichkeit") cfg.zoomSens = std::clamp(v, 5, 200);
			else if (k == "fadenkreuz") cfg.fadenkreuz = std::clamp(v, 0, 3);
			else if (k == "zielfernrohr") cfg.scope = std::clamp(v, 0, 2);
			else if (k == "online_plus_effekte") cfg.impact = v != 0 ? 1 : 0;
			else if (k == "schaden_stoss") cfg.impactKick = std::clamp(v, 0, 100);
			else if (k == "treffermarker") cfg.impactMarker = v != 0 ? 1 : 0;
			else if (k == "treffer_klang") cfg.impactSound = v != 0 ? 1 : 0;
			else if (k == "rueckstoss") cfg.impactRecoil = std::clamp(v, 0, 100);
			else if (k == "ansager") cfg.impactAnsage = v != 0 ? 1 : 0;
			else if (k == "schaden_rand") cfg.impactRand = v != 0 ? 1 : 0;
			else if (k == "blickwinkel_max") cfg.nickMax = std::clamp(v, 40, 85);
			else if (k == "alles_frei") cfg.unlockAll = v != 0 ? 1 : 0;
			else if (k == "pad_modern") cfg.padModern = v != 0 ? 1 : 0;
			else if (k == "pad_tempo") cfg.padSens = std::clamp(v, 20, 300);
			else if (k == "belegung_version") cfg.belVersion = v;
			else if (k.rfind("taste_", 0) == 0)
			{
				const int id = std::atoi(k.c_str() + 6);
				if (id >= 0 && id < kAnzAktionen)
				{
					const std::string val = line.substr(eq + 1);
					const auto komma = val.find(',');
					cfg.bel[id][0] = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
					cfg.bel[id][1] = komma == std::string::npos ? 0 : static_cast<int>(std::strtol(val.c_str() + komma + 1, nullptr, 10));
				}
			}
			else if (k == "trilinear") cfg.tri = std::clamp(v, 0, 2);
			else if (k == "effekte") { const std::string val = line.substr(eq + 1); cfg.fx = val == "original-plus" ? 1 : val == "klar" ? 2 : val == "filmisch" ? 3 : 0; }
			else if (k == "texturpakete") cfg.packs = line.substr(eq + 1);
			else if (k == "online_server") cfg.onlineServer = line.substr(eq + 1);
			else if (k == "online_name") cfg.onlineName = line.substr(eq + 1);
			else if (k == "online_passwort") cfg.onlinePw = line.substr(eq + 1);
			else if (k == "online_favoriten") cfg.onlineFavs = line.substr(eq + 1);
		}
		if (aspektAlt && !aspektNeu) cfg.aspect = 4; // 1.0.x default 16:9 -> fit the screen (16:10 without bars)
		belegungPruefen();
	}
	void belegungPruefen()
	{
		// Call without the lock (load holds it): replace older/missing bindings with the current defaults
		if (cfg.belVersion < kBelegungVersion)
		{
			for (int i = 0; i < kAnzAktionen; ++i) { cfg.bel[i][0] = kAktionen[i].std1; cfg.bel[i][1] = kAktionen[i].std2; }
			cfg.belVersion = kBelegungVersion;
		}
	}
	void save()
	{
		Cfg c;
		{ std::lock_guard<std::mutex> lk(m); c = cfg; }
		std::ostringstream f;
		f << "# " << cfgName() << " - settings (changed in the game menu, ESC/F1 or the Guide button)\n"
		  << "aufloesung=" << c.upscale << "\nbildformat=" << c.aspect << "\nbildformat_v2=1\nfxaa=" << c.fxaa << "\ncas=" << c.cas
		  << "\ncas_staerke=" << c.casSharp << "\ndithering=" << c.dither << "\ntv_shader=" << c.tv << "\nfarbe=" << c.shadeboost << "\nhelligkeit=" << c.sbB
		  << "\nkontrast=" << c.sbC << "\nsaettigung=" << c.sbS << "\ntexturfilter=" << c.filter << "\nanisotrop=" << c.aniso
		  << "\nvollbild=" << c.fullscreen << "\nfps_anzeige=" << c.showFps << "\nvibration=" << c.rumble
		  << "\nvibration_staerke=" << c.rumblePct << "\nvsync=" << c.vsync << "\nbildrate=" << c.hz << "\nlautstaerke=" << c.volume << "\nkamera_unschaerfe=" << c.camBlur << "\nnachzieheffekt=" << c.nachzieh << "\nstartmenue=" << c.startMenu << "\neffekte=" << (c.fx == 1 ? "original-plus" : c.fx == 2 ? "klar" : c.fx == 3 ? "filmisch" : "aus") << "\ntrilinear=" << c.tri << "\nhd_text=" << c.hdText << "\nmaus_zielen=" << c.maus << "\nmaus_empfindlichkeit=" << c.mausSens << "\nmaus_invertiert=" << c.mausInvert << "\nmaus_zielmodus_dauer=" << c.mausZielmodus << "\nsprache=" << c.lang << "\neingabeanzeige=" << c.showInput << "\nbelegung_version=" << c.belVersion << "\nzoom_empfindlichkeit=" << c.zoomSens << "\nfadenkreuz=" << c.fadenkreuz << "\nzielfernrohr=" << c.scope << "\nonline_plus_effekte=" << c.impact << "\nschaden_stoss=" << c.impactKick << "\ntreffermarker=" << c.impactMarker << "\ntreffer_klang=" << c.impactSound << "\nrueckstoss=" << c.impactRecoil << "\nansager=" << c.impactAnsage << "\nschaden_rand=" << c.impactRand << "\nblickwinkel_max=" << c.nickMax << "\nalles_frei=" << c.unlockAll << "\npad_modern=" << c.padModern << "\npad_tempo=" << c.padSens << "\ntexturpakete=" << c.packs << "\nonline_server=" << c.onlineServer << "\nonline_name=" << c.onlineName << "\nonline_passwort=" << c.onlinePw << "\nonline_favoriten=" << c.onlineFavs << "\n";
		for (int i = 0; i < kAnzAktionen; ++i)
			f << "taste_" << i << "=" << c.bel[i][0] << "," << c.bel[i][1] << "   # " << kAktionen[i].name << "\n";
		std::ofstream aus(iniPath(), std::ios::trunc);
		if (!cfgEnglish()) { aus << f.str(); return; }
		aus << "# " << cfgName() << " - settings (changed in the game menu: ESC/F1 or the Guide button)\n";
		std::istringstream in(f.str());
		std::string z;
		std::getline(in, z); // deutsche Kopfzeile
		while (std::getline(in, z)) aus << cfgZeile(z, true) << "\n";
	}
	// ---------- Texturquellen (HD-Text + Pakete) -> zusammengesetzter Ersatzordner ----------
	// Sources: <dir>/texturquellen/<name>/... (hd-text = our re-typeset pages; packs are imported by the start script from
	// texturpakete/*.zip|7z|rar importiert). Ziel: <Textures>/SCES-53326/replacements als Verweise (Symlinks); spaetere
	// source overrides earlier ones, HD text last. PS2XGS_TEXDIR (test runs) turns the management off.
	// PS2XGS_TEXQUELLEN: directory with the texture sources (start script: <project>/build/texturquellen), otherwise ~/.config/...
	// Folder inside the settings directory. With English settings (PS2XGS_CFG_EN=1) the folder has an English name;
	// a folder left by an older version under the old name is renamed once.
	std::string cfgOrdner(const char* alt, const char* neu)
	{
		if (!cfgEnglish()) return dir() + "/" + alt;
		const std::string n = dir() + "/" + neu, a = dir() + "/" + alt;
		std::error_code ec;
		if (!std::filesystem::exists(n, ec) && std::filesystem::exists(a, ec)) { std::filesystem::rename(a, n, ec); if (ec) return a; }
		return n;
	}
	std::string quellenDir()
	{
		if (const char* q = std::getenv("PS2XGS_TEXQUELLEN"); q && *q) return q;
		return cfgOrdner("texturquellen", "texture_sources");
	}
	std::string texRoot()
	{
		if (const char* t = std::getenv("PS2XGS_TEXDIR"); t && *t) return t;
		return cfgOrdner("texturen", "textures");
	}
	bool texManaged() { const char* t = std::getenv("PS2XGS_TEXDIR"); return !(t && *t); }
	std::vector<std::string> listSources()
	{
		std::vector<std::string> out;
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(quellenDir(), ec))
			if (e.is_directory(ec) && e.path().filename() != "hd-text" && e.path().filename().string()[0] != '.')
				out.push_back(e.path().filename().string());
		std::sort(out.begin(), out.end());
		return out;
	}
	std::vector<std::string> listSources();
	std::vector<std::string> activePacks(const std::string& p)
	{
		if (p == "*") return listSources();
		std::vector<std::string> out; std::string cur;
		for (char ch : p + ",") { if (ch == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += ch; }
		return out;
	}
	void setPackActive(std::string& packs, const std::string& name, bool on)
	{
		auto v = activePacks(packs);
		v.erase(std::remove(v.begin(), v.end(), name), v.end());
		if (on) v.push_back(name);
		std::string j; for (const auto& x : v) { if (!j.empty()) j += ','; j += x; }
		packs = j;
	}
	bool sameCfg(const Cfg& a, const Cfg& b)
	{
		return a.upscale == b.upscale && a.aspect == b.aspect && a.cas == b.cas && a.casSharp == b.casSharp && a.tv == b.tv && a.filter == b.filter &&
		       a.aniso == b.aniso && a.dither == b.dither && a.sbB == b.sbB && a.sbC == b.sbC && a.sbS == b.sbS && a.rumblePct == b.rumblePct &&
		       a.hz == b.hz && a.volume == b.volume && a.camBlur == b.camBlur && a.nachzieh == b.nachzieh && a.fxaa == b.fxaa && a.shadeboost == b.shadeboost &&
		       a.fullscreen == b.fullscreen && a.showFps == b.showFps && a.rumble == b.rumble && a.vsync == b.vsync && a.startMenu == b.startMenu &&
		       a.hdText == b.hdText && a.unlockAll == b.unlockAll && a.padModern == b.padModern && a.padSens == b.padSens && a.maus == b.maus && a.mausSens == b.mausSens && a.mausInvert == b.mausInvert && a.mausZielmodus == b.mausZielmodus && a.fadenkreuz == b.fadenkreuz && a.scope == b.scope && a.impact == b.impact && a.impactKick == b.impactKick && a.impactMarker == b.impactMarker && a.impactSound == b.impactSound && a.impactRecoil == b.impactRecoil && a.impactAnsage == b.impactAnsage && a.impactRand == b.impactRand && a.nickMax == b.nickMax && a.lang == b.lang && a.showInput == b.showInput &&
		       a.zoomSens == b.zoomSens && std::memcmp(a.bel, b.bel, sizeof(a.bel)) == 0 && a.packs == b.packs && a.fx == b.fx && a.tri == b.tri &&
		       a.onlineServer == b.onlineServer && a.onlineName == b.onlineName && a.onlinePw == b.onlinePw && a.onlineFavs == b.onlineFavs;
	}
	// Key as PCSX2 builds it when loading: "<TEX0 hash>-<CLUT hash>-<bits>" without the old TCC bit (0x4000)
	// that older packs still carry in the name (RemoveUnusedBits). Same key = same texture.
	static std::string texKey(const std::string& file)
	{
		const auto dot = file.find('.');
		std::string base = file.substr(0, dot);
		const auto d = base.rfind('-');
		if (d == std::string::npos) return base;
		unsigned long bits = std::strtoul(base.c_str() + d + 1, nullptr, 16) & ~0x4000ul;
		char b[16]; std::snprintf(b, sizeof(b), "%08lx", bits);
		return base.substr(0, d + 1) + b;
	}
	// keys: textures already covered (packs first); a source with nurNeue=true only fills gaps.
	static void linkTree(const std::filesystem::path& src, const std::filesystem::path& dst, unsigned& n,
		std::set<std::string>& keys, bool nurNeue = false)
	{
		std::error_code ec;
		// <source>/.ausschluss.txt: file names that are not replaced (e.g. palette textures whose replacement forces PCSX2
		// onto the slow path: every palette variant converted separately -> draw calls x3..x8)
		std::set<std::string> aus;
		{
			std::ifstream f((src / ".ausschluss.txt").string());
			std::string l;
			while (std::getline(f, l))
				if (!l.empty() && l[0] != '#') { while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back(); aus.insert(l); }
		}
		for (const auto& e : std::filesystem::recursive_directory_iterator(src, std::filesystem::directory_options::follow_directory_symlink, ec))
		{
			if (!e.is_regular_file(ec)) continue;
			std::string ext = e.path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext != ".png" && ext != ".dds") continue;
			if (aus.count(e.path().filename().string())) continue;
			const std::string key = texKey(e.path().filename().string());
			if (nurNeue && keys.count(key)) continue;
			keys.insert(key);
			const auto to = dst / e.path().filename();
			// The alpha side file (tools/dds_alpha.py) is read through the real path, no link of its own needed
			std::filesystem::remove(to, ec);
			std::filesystem::create_symlink(std::filesystem::absolute(e.path()), to, ec);
			if (ec) { ec.clear(); std::filesystem::create_hard_link(e.path(), to, ec); }
			if (ec) { ec.clear(); std::filesystem::copy_file(e.path(), to, std::filesystem::copy_options::overwrite_existing, ec); }
			if (!ec) ++n;
		}
	}
	// Assemble; returns the number of files. May run before the GS starts and on the GS thread.
	unsigned assembleTextures()
	{
		if (!texManaged()) return 0;
		std::error_code ec;
		const std::filesystem::path root = texRoot();
		const auto dst = root / envOr("PS2XGS_SERIAL", "SCES-53326") / "replacements"; // PCSX2-Ordnername = Seriennummer
		const auto marker = dst / ".verwaltet";
		std::filesystem::create_directories(dst, ec);
		if (!std::filesystem::exists(marker, ec))
		{
			// Save existing files (copied by hand or by earlier start scripts) once as the source "alt"
			const auto alt = std::filesystem::path(quellenDir()) / "alt" / "replacements";
			bool any = false;
			for (const auto& e : std::filesystem::directory_iterator(dst, ec))
				if (e.is_regular_file(ec) && !e.is_symlink(ec)) { if (!any) { std::filesystem::create_directories(alt, ec); any = true; } std::filesystem::rename(e.path(), alt / e.path().filename(), ec); }
			std::ofstream(marker.string()) << "This folder is managed by the game menu (Textures) - it only holds links to the texture sources.\n";
		}
		// Everything in the managed folder is a link (Linux) or a hard link/copy (Windows, where links need extra rights):
		// clear it all except the marker, otherwise textures of a switched-off pack would stay active on Windows.
		{
			std::vector<std::filesystem::path> weg;
			for (const auto& e : std::filesystem::directory_iterator(dst, ec))
				if (e.path().filename() != ".verwaltet" && (e.is_symlink(ec) || e.is_regular_file(ec))) weg.push_back(e.path());
			for (const auto& w : weg) std::filesystem::remove(w, ec);
		}
		unsigned n = 0;
		const auto q = std::filesystem::path(quellenDir());
		std::string pk; bool hd; { std::lock_guard<std::mutex> lk(m); pk = cfg.packs; hd = cfg.hdText; }
		// Packs first: if they bring a text page themselves (e.g. Origami: all languages, original font),
		// the pack wins; our re-typeset pages only fill pages no active pack covers.
		std::set<std::string> keys;
		for (const auto& name : activePacks(pk)) linkTree(q / name, dst, n, keys);
		if (hd) linkTree(q / "hd-text", dst, n, keys, true);
		return n;
	}
	// transfer into the PCSX2 settings (CPU thread or before the start)
	// Internal resolution "auto" (9): about 1.5x the window height -> supersampling (smooth edges, sharp textures; better
	// than FXAA in the comparison aa720). 1080p -> 4x, 1440p -> 5x, 4K -> 6x (cap: 8x at 4K is too much for mid GPUs).
	std::atomic<int> autoStufe{0};
	std::atomic<int> autoAbzug{0};   // steps the frame rate guard took off the "auto" scale in this session
	int autoAufloesung()
	{
		const u32 h = s_wi.surface_height ? s_wi.surface_height : 1080u;
		// Steam Deck (Valve board): 2x like its default - battery and frame rate before supersampling
		static const bool deck = [] {
#ifdef __linux__
			std::ifstream dv("/sys/devices/virtual/dmi/id/board_vendor"); std::string v; std::getline(dv, v);
			const char* sd = std::getenv("SteamDeck"); return v.rfind("Valve", 0) == 0 || (sd && *sd == '1');
#else
			return false;
#endif
		}();
		if (deck) return 2;
		return std::clamp(std::clamp(static_cast<int>((h * 3u + 2u * 448u - 1u) / (2u * 448u)), 2, 6) - autoAbzug.load(), 2, 6);
	}
	// Frame rate guard for the "auto" resolution: if the game stays clearly below its normal rate for 10 seconds (GPU too
	// weak for the supersampling), auto goes down one step (not below 2x) and stays there for this session. A fixed
	// resolution chosen by the player is never touched. PS2XGS_AUTOGUARD_TEST=<vps> feeds a rate for test runs.
	void autoWaechter(float vps)
	{
		static const float testVps = [] { const char* e = std::getenv("PS2XGS_AUTOGUARD_TEST"); return e ? static_cast<float>(std::atof(e)) : 0.f; }();
		static const auto t0 = std::chrono::steady_clock::now();
		static double schlechtSeit = -1.0, letzte = -1000.0; static float beste = 0.f;
		const double jetzt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		int u; { std::lock_guard<std::mutex> lk(m); u = cfg.upscale; }
		if (u != 9 || autoStufe.load() <= 2 || open.load() || ps2xhot::turbo.load() || jetzt < 25.0) { schlechtSeit = -1.0; return; }
		if (testVps > 0.f) { vps = testVps; beste = std::max(beste, 60.f); }
		// nominal rate: the game runs at 50 (PAL) or 60 (NTSC) VBlanks per second; short catch-up bursts above that do not count
		// (taken over only after 6 measurements in a row at that rate)
		{
			static int n60 = 0, n50 = 0;
			n60 = std::fabs(vps - 60.f) < 1.5f ? n60 + 1 : 0; n50 = std::fabs(vps - 50.f) < 1.5f ? n50 + 1 : 0;
			if (n60 >= 6) beste = 60.f; else if (n50 >= 6) beste = 50.f;
		}
		// a PC that is too slow from the first frame never shows the nominal rate: assume 60 after 45 s, otherwise the
		// guard would never step down exactly where it is needed (GTX 1050 at 1080p: auto = 4x, about 20 fps)
		if (beste < 45.f && jetzt > 45.0) beste = 60.f;
		if (beste < 45.f || vps <= 1.f || vps >= 0.88f * beste) { schlechtSeit = -1.0; return; }
		if (schlechtSeit < 0.0) { schlechtSeit = jetzt; return; }
		if (jetzt - schlechtSeit < 10.0 || jetzt - letzte < 20.0) return;
		letzte = jetzt; schlechtSeit = -1.0;
		const int neu = autoAbzug.fetch_add(1) + 1;
		Console.WriteLn(fmt::format("[ps2xmenu] frame rate guard: {:.1f} of {:.1f} per second for 10 s - auto resolution one step down ({}x -> {}x)", vps, beste, autoStufe.load(), autoAufloesung()));
		(void)neu;
		applyPending.store(true);
	}
	// window size changed (fullscreen, resize): with "auto" re-apply the settings when the level changes
	void fensterGroesse()
	{
		int u;
		{ std::lock_guard<std::mutex> lk(m); u = cfg.upscale; }
		if (u == 9 && autoStufe.load() != 0 && autoAufloesung() != autoStufe.load())
			applyPending.store(true);
	}
	void toSettings(SettingsInterface& si)
	{
		Cfg c;
		{ std::lock_guard<std::mutex> lk(m); c = cfg; }
		if (c.upscale == 9)
		{
			const int a = autoAufloesung();
			autoStufe.store(a);
			Console.WriteLn(fmt::format("[ps2xmenu] internal resolution auto: {}x (window height {})", a, s_wi.surface_height));
			c.upscale = a;
		}
		// Aspect: 0 = 16:9 (the game's widescreen mode forced), 1 = as the game option, 2 = 4:3, 3 = fill the window
		const bool wide = (c.aspect == 0) || (c.aspect == 4) || (c.aspect == 1 && gameWide.load());
		si.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(c.upscale));
		si.SetStringValue("EmuCore/GS", "VideoCaptureCodec", envOr("PS2XGS_CAPTURE_CODEC", "libx264"));
		si.SetIntValue("EmuCore/GS", "VideoCaptureBitrate", std::atoi(envOr("PS2XGS_CAPTURE_KBPS", "40000")));
		// x264: constant quality instead of bitrate, fast preset (real time at 4K/60 on many cores); PS2XGS_CAPTURE_PARAMS overrides
		si.SetBoolValue("EmuCore/GS", "EnableVideoCaptureParameters", true);
		si.SetStringValue("EmuCore/GS", "VideoCaptureParameters", envOr("PS2XGS_CAPTURE_PARAMS", "crf=16:preset=veryfast"));
		si.SetStringValue("EmuCore/GS", "CaptureContainer", "mp4");
		si.SetBoolValue("EmuCore/GS", "EnableVideoCapture", true);
		// Record sound too (PS2XGS_CAPTURE_AUDIO=0 for test runs without audio output: otherwise the video waits for audio packets)
		si.SetBoolValue("EmuCore/GS", "EnableAudioCapture", std::atoi(envOr("PS2XGS_CAPTURE_AUDIO", "1")) != 0);
		si.SetIntValue("EmuCore/GS", "AudioCaptureBitrate", 192);
		si.SetStringValue("EmuCore/GS", "AudioCaptureCodec", "aac");
		// 4 = ultrawide: the game renders with the window's aspect ratio (ps2xgs_aspect_milli), the picture fills the window
		si.SetStringValue("EmuCore/GS", "AspectRatio", (c.aspect == 3 || c.aspect == 4) ? "Stretch" : (wide ? "16:9" : "4:3"));
		si.SetBoolValue("EmuCore/GS", "fxaa", c.fxaa);
		si.SetIntValue("EmuCore/GS", "CASMode", c.cas);
		si.SetIntValue("EmuCore/GS", "CASSharpness", c.casSharp);
		// PS2 dithering: upscaled it becomes a fine grid pattern on faces/walls -> off in the remaster preset
		si.SetIntValue("EmuCore/GS", "dithering_ps2", c.dither);
		// Oberste/unterste Bildzeile enthaelt Restdaten (bunte Pixel am Bildrand) -> je 2 Zeilen abschneiden
		si.SetIntValue("EmuCore/GS", "CropTop", 2);
		si.SetIntValue("EmuCore/GS", "CropBottom", 2);
		si.SetIntValue("EmuCore/GS", "TVShader", c.tv);
		si.SetBoolValue("EmuCore/GS", "ShadeBoost", c.shadeboost);
		si.SetIntValue("EmuCore/GS", "ShadeBoost_Brightness", c.sbB);
		si.SetIntValue("EmuCore/GS", "ShadeBoost_Contrast", c.sbC);
		si.SetIntValue("EmuCore/GS", "ShadeBoost_Saturation", c.sbS);
		si.SetIntValue("EmuCore/GS", "filter", c.filter);
		si.SetIntValue("EmuCore/GS", "MaxAnisotropy", c.aniso);
		si.SetIntValue("EmuCore/GS", "TriFilter", c.tri == 0 ? -1 : c.tri == 1 ? 1 : 2); // TriFiltering: Automatic/PS2/Forced
		static const bool showcase = std::getenv("PS2XGS_SHOWCASE") != nullptr;   // recordings: no FPS counter in the picture
		si.SetBoolValue("EmuCore/GS", "OsdShowFPS", c.showFps && !showcase);
		si.SetIntValue("EmuCore/GS", "OsdPerformancePos", (c.showFps && !showcase) ? 3 : 0);
		si.SetBoolValue("EmuCore/GS", "OsdShowResolution", c.showFps && !showcase);   // resolution line only together with the FPS counter
		si.SetBoolValue("EmuCore/GS", "OsdShowGSStats", false);
		si.SetIntValue("EmuCore/GS", "VsyncEnable", c.vsync ? 1 : 0);
		si.SetBoolValue("EmuCore/GS", "hw_mipmap", true);           // Mipmaps in hardware: no shimmering in the distance
		si.SetIntValue("EmuCore/GS", "linear_present_mode", 1);     // weiches Herunterrechnen = Supersampling-Effekt
	}

	// ---------- Zeichnen (GS-Thread) ----------
	u32 s_prevNav = 0;
	void feedInput()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		const u32 n = nav.load();
		const u32 ch = n ^ s_prevNav;
		static const ImGuiKey kKeys[6] = {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
			ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight};
		for (int i = 0; i < 6; ++i)
			if (ch & (1u << i))
				io.AddKeyEvent(kKeys[i], (n >> i) & 1u);
		s_prevNav = n;
		const int mx = mouseX.load(), my = mouseY.load();
		if (mx >= 0)
			io.AddMousePosEvent(static_cast<float>(mx), static_cast<float>(my));
		static u32 s_prevMb = 0;
		const u32 mb = mouseBtn.load();
		for (int i = 0; i < 3; ++i)
			if ((mb ^ s_prevMb) & (1u << i))
				io.AddMouseButtonEvent(i, (mb >> i) & 1u);
		s_prevMb = mb;
		if (const int w = wheel.exchange(0))
			io.AddMouseWheelEvent(0.0f, static_cast<float>(w));
	}

	void style()
	{
		// TimeSplitters 2: deep blue-black, cyan lines, green highlight, white text - like the game's menus
		ImGuiStyle& st = ImGui::GetStyle();
		st.WindowRounding = 4.0f; st.FrameRounding = 3.0f; st.GrabRounding = 3.0f; st.TabRounding = 3.0f; st.PopupRounding = 4.0f; st.ScrollbarRounding = 3.0f;
		st.WindowBorderSize = 1.0f; st.FrameBorderSize = 0.0f; st.TabBorderSize = 0.0f;
		st.WindowPadding = ImVec2(26, 20); st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 8); st.ItemInnerSpacing = ImVec2(8, 6); st.CellPadding = ImVec2(8, 5);
		ImVec4* c = st.Colors;
		const ImVec4 cyan(0.42f, 0.86f, 1.00f, 1.0f), green(0.50f, 1.00f, 0.42f, 1.0f);
		c[ImGuiCol_WindowBg] = ImVec4(0.025f, 0.045f, 0.10f, 0.95f);
		c[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.05f, 0.11f, 0.98f);
		c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_Border] = ImVec4(cyan.x, cyan.y, cyan.z, 0.45f);
		c[ImGuiCol_Text] = ImVec4(0.94f, 0.97f, 1.00f, 1.0f);
		c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.66f, 0.80f, 1.0f);
		c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.14f, 0.27f, 0.92f);
		c[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.24f, 0.43f, 0.95f);
		c[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.32f, 0.56f, 1.0f);
		c[ImGuiCol_Button] = ImVec4(0.10f, 0.20f, 0.37f, 0.95f);
		c[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.42f, 0.68f, 1.0f);
		c[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.62f, 0.92f, 1.0f);
		c[ImGuiCol_Header] = ImVec4(0.13f, 0.28f, 0.50f, 0.90f);
		c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.44f, 0.72f, 0.95f);
		c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.60f, 0.92f, 1.0f);
		c[ImGuiCol_Tab] = ImVec4(0.06f, 0.11f, 0.22f, 1.0f);
		c[ImGuiCol_TabHovered] = ImVec4(0.19f, 0.44f, 0.72f, 1.0f);
		c[ImGuiCol_TabSelected] = ImVec4(0.15f, 0.36f, 0.64f, 1.0f);
		c[ImGuiCol_TabSelectedOverline] = cyan;
		c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
		c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
		c[ImGuiCol_SliderGrab] = cyan;
		c[ImGuiCol_SliderGrabActive] = ImVec4(0.65f, 1.0f, 1.0f, 1.0f);
		c[ImGuiCol_CheckMark] = green;
		c[ImGuiCol_NavCursor] = green;
		c[ImGuiCol_Separator] = ImVec4(cyan.x, cyan.y, cyan.z, 0.35f);
		c[ImGuiCol_TableHeaderBg] = ImVec4(0.09f, 0.17f, 0.32f, 1.0f);
		c[ImGuiCol_TableBorderLight] = ImVec4(cyan.x, cyan.y, cyan.z, 0.15f);
		c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
		c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
		c[ImGuiCol_ScrollbarGrab] = ImVec4(0.17f, 0.42f, 0.68f, 0.8f);
	}

	std::string memcardInfo()
	{
		const char* mc = std::getenv("SOTC_MEMCARD");
		if (!mc || !*mc)
			return tr("Memory card: not set");
		struct stat sb{};
		if (stat(mc, &sb) != 0)
			return std::string(tr("Memory card: ")) + mc + tr(" (still empty)");
		char tbuf[64];
		std::strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M", std::localtime(&sb.st_mtime));
		return std::string(tr("Memory card: ")) + mc + tr("\nLast saved: ") + tbuf;
	}

	bool combo(const char* label, int* v, const char* const* items, int n)
	{
		return ImGui::Combo(label, v, items, n);
	}

static bool s_onlineTabWaehlen = false;
static bool s_opTabWaehlen = false;   // open the menu on the Online+ page
static bool s_hubModus = false;       // menu shows the Online+ hub (opened from the game's main menu item)
// Level order of the Arcade Custom list (index = how often the macro presses down)
static const char* kBotKarten[] = {"Mexican Mission", "Ice Station", "Hospital", "Training Ground", "Aztec", "Scrapyard *", "Nightclub *", "Hangar *",
	"Robot Factory *", "Ufopia *", "Chinese *", "Chasm *", "Streets *", "Compound *", "Site *", "Circus *"};
static const int kBotKartenN = 16;
// Capture the Bag has its own (shorter) level list in the game
static const char* kBotKartenCtb[] = {"Mexican Mission", "Ice Station", "Training Ground", "Hangar *", "Streets *"};
static const int kBotKartenCtbN = 5;
// the game's bot presets (Setup Bots -> Current Bot Set), in their order
static const char* kBotSets[] = {"Recommended", "Misfits", "Freaks", "The Living Dead", "Lovely Ladies", "Military", "Classic", "Leads", "Sidekicks"};
static const int kBotSetsN = 9;
	// ---------- Online: server browser (TS2 online multiplayer, see server/ in the port repository) ----------
	// List: community server list (GitHub, PS2XGS_SERVERLISTE) + own favourites. Every server answers a short
	// UDP query "T2IN"+nonce with name, players, password flag and message; the ping comes from that too.
	// The game side (runtime) reads online_server/online_name/online_passwort from the settings file when the
	// network starts (Arcade -> Network). PS2XGS_ONLINE=1 (TS2 start script) turns the tab on.
	namespace online
	{
		struct Eintrag
		{
			std::string adresse, name, motd;
			int spieler = -1, max = 0, ping = -1, proto = 0;
			bool pw = false, fav = false, antwort = false;
		};
		std::mutex om;
		std::vector<Eintrag> liste;
		std::atomic<bool> laeuft{false};
		std::string meldung = "Not refreshed yet";
		std::string spielStatus;            // from the runtime (ps2xgs_online_status)
		std::atomic<bool> oeffnen{false};   // Runtime: the game reached the network configuration -> show the browser
		bool vonSpielGeoeffnet = false;
		int auswahl = -1;
		char eingabe[128] = "";
		char pwEingabe[64] = "";
		char nameEingabe[32] = "";
		std::string pwFuer;
		bool nurFrei = false, nurOffen = false, nurFav = false;

		bool aktiv() { const char* e = std::getenv("PS2XGS_ONLINE"); return e && *e == '1'; }
		const char* listenUrl() { return envOr("PS2XGS_SERVERLISTE", "https://raw.githubusercontent.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/main/server/servers.txt"); }

		std::vector<std::string> teilen(const std::string& s, char t)
		{
			std::vector<std::string> v;
			std::string cur;
			for (char ch : s)
			{
				if (ch == t) { if (!cur.empty()) v.push_back(cur); cur.clear(); }
				else if (ch != ' ' && ch != '\t' && ch != '\r') cur.push_back(ch);
			}
			if (!cur.empty()) v.push_back(cur);
			return v;
		}

		// "host[:port]" -> Abfrage per UDP; Ergebnis in e (Ping in ms)
		void abfragen(std::vector<Eintrag>& v)
		{
#ifdef _WIN32
			static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
			(void)wsa;
#endif
			struct Offen { size_t i; int fd; std::chrono::steady_clock::time_point t0; uint32_t nonce; };
			std::vector<Offen> offen;
			for (size_t i = 0; i < v.size(); ++i)
			{
				std::string h = v[i].adresse, port = "27713";
				if (!h.empty() && h[0] == '[' && h.find(']') != std::string::npos)
				{
					const size_t e = h.find(']');
					if (e + 1 < h.size() && h[e + 1] == ':') port = h.substr(e + 2);
					h = h.substr(1, e - 1);
				}
				else if (std::count(h.begin(), h.end(), ':') == 1)
				{
					port = h.substr(h.find(':') + 1);
					h = h.substr(0, h.find(':'));
				}
				addrinfo hint{}, *res = nullptr;
				hint.ai_family = AF_UNSPEC;
				hint.ai_socktype = SOCK_DGRAM;
				if (getaddrinfo(h.c_str(), port.c_str(), &hint, &res) != 0 || !res) continue;
				const int fd = static_cast<int>(socket(res->ai_family, SOCK_DGRAM, 0));
				if (fd < 0) { freeaddrinfo(res); continue; }
				const uint32_t nonce = static_cast<uint32_t>(i * 2654435761u) ^ static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
				char q[8] = {'T', '2', 'I', 'N'};
				std::memcpy(q + 4, &nonce, 4);
				const auto t0 = std::chrono::steady_clock::now();
				sendto(fd, q, 8, 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
				freeaddrinfo(res);
				offen.push_back({i, fd, t0, nonce});
			}
			const auto ende = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
			while (!offen.empty() && std::chrono::steady_clock::now() < ende)
			{
				std::vector<pollfd> pf;
				for (auto& o : offen) pf.push_back({static_cast<decltype(pollfd::fd)>(o.fd), POLLIN, 0});
				const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(ende - std::chrono::steady_clock::now()).count());
#ifdef _WIN32
				if (WSAPoll(pf.data(), static_cast<ULONG>(pf.size()), std::max(ms, 1)) <= 0) break;
#else
				if (poll(pf.data(), pf.size(), std::max(ms, 1)) <= 0) break;
#endif
				for (size_t k = pf.size(); k-- > 0;)
				{
					if (!(pf[k].revents & POLLIN)) continue;
					Offen o = offen[k];
					char b[512];
					const int n = static_cast<int>(recv(o.fd, b, sizeof b, 0));
					if (n >= 15 && std::memcmp(b, "T2IN", 4) == 0 && std::memcmp(b + 4, &o.nonce, 4) == 0)
					{
						Eintrag& e = v[o.i];
						auto u16 = [&](int p) { return static_cast<int>(static_cast<unsigned char>(b[p]) | (static_cast<unsigned char>(b[p + 1]) << 8)); };
						e.proto = u16(8);
						e.spieler = u16(10);
						e.max = u16(12);
						e.pw = b[14] != 0;
						const std::string rest(b + 15, static_cast<size_t>(n - 15));
						e.name = rest.substr(0, rest.find('\0'));
						e.motd = rest.find('\0') != std::string::npos ? rest.substr(rest.find('\0') + 1) : "";
						e.ping = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - o.t0).count());
						e.antwort = true;
					}
#ifdef _WIN32
					closesocket(o.fd);
#else
					close(o.fd);
#endif
					offen.erase(offen.begin() + static_cast<long>(k));
				}
			}
			for (auto& o : offen)
#ifdef _WIN32
				closesocket(o.fd);
#else
				close(o.fd);
#endif
		}

		void aktualisieren(const std::string& favoriten, const std::string& aktuell)
		{
			if (laeuft.exchange(true)) return;
			{ std::lock_guard<std::mutex> lk(om); meldung = "Suche Server ..."; }
			std::thread([favoriten, aktuell] {
				std::vector<std::string> adressen;
				std::string fehler;
				// Community-Liste laden (Zeilen "host[:port]   # Kommentar")
				if (auto dl = HTTPDownloader::Create())
				{
					dl->CreateRequest(listenUrl(), [&](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
						if (code != 200) { fehler = "Server list not reachable (" + std::to_string(code) + ")"; return; }
						std::string text(data.begin(), data.end());
						size_t p = 0;
						while (p < text.size())
						{
							size_t e = text.find('\n', p);
							if (e == std::string::npos) e = text.size();
							std::string z = text.substr(p, e - p);
							p = e + 1;
							if (const size_t h = z.find('#'); h != std::string::npos) z.resize(h);
							for (auto& a : teilen(z, ' ')) { adressen.push_back(a); break; }
						}
					});
					dl->WaitForAllRequests();
				}
				std::vector<Eintrag> v;
				auto dazu = [&](const std::string& a, bool fav) {
					for (auto& e : v) if (e.adresse == a) { e.fav = e.fav || fav; return; }
					Eintrag e; e.adresse = a; e.fav = fav; e.name = a; v.push_back(e);
				};
				for (auto& a : teilen(favoriten, ',')) dazu(a, true);
				for (auto& a : adressen) dazu(a, false);
				if (!aktuell.empty()) dazu(aktuell, false);
				abfragen(v);
				int erreichbar = 0;
				for (auto& e : v) erreichbar += e.antwort ? 1 : 0;
				std::lock_guard<std::mutex> lk(om);
				liste = std::move(v);
				auswahl = -1;
				meldung = (fehler.empty() ? "" : fehler + " - ") + std::to_string(erreichbar) + " of " + std::to_string(liste.size()) + " servers reachable";
				laeuft.store(false);
			}).detach();
		}
	}

	// Tab "Online" in the game menu: player name, server list (sortable/filterable), connect, favourites, direct address
	bool onlineTab(Cfg& c, bool& schliessen)
	{
		using namespace online;
		bool geaendert = false;
		if (nameEingabe[0] == 0 && !c.onlineName.empty()) std::snprintf(nameEingabe, sizeof nameEingabe, "%s", c.onlineName.c_str());
		ImGui::TextColored(ImVec4(0.50f, 1.00f, 0.42f, 1.0f), "%s", tr("Play online"));
		ImGui::TextDisabled("%s", tr("Pick a server and connect. Then confirm \"TS2 Online\" in the game: Start Game hosts, Join Game lists the games on the server."));
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
		if (ImGui::InputText(tr("Player name"), nameEingabe, sizeof nameEingabe)) { c.onlineName = nameEingabe; geaendert = true; }
		ImGui::SameLine();
		ImGui::TextDisabled("  Server: %s", c.onlineServer.empty() ? tr("(none)") : c.onlineServer.c_str());
		{
			std::lock_guard<std::mutex> lk(om);
			if (!spielStatus.empty()) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", spielStatus.c_str());
		}
		if (ImGui::Button(laeuft.load() ? tr("Searching ...") : tr("Refresh")) && !laeuft.load()) aktualisieren(c.onlineFavs, c.onlineServer);
		ImGui::SameLine();
		ImGui::Checkbox(tr("not full"), &nurFrei);
		ImGui::SameLine();
		ImGui::Checkbox(tr("no password"), &nurOffen);
		ImGui::SameLine();
		ImGui::Checkbox(tr("favourites only"), &nurFav);
		ImGui::SameLine();
		{ std::lock_guard<std::mutex> lk(om); ImGui::TextDisabled("%s", meldung.c_str()); }

		std::string verbinden;
		bool verbindenPw = false;
		const float tabH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.2f;
		if (ImGui::BeginTable("##server", 6, ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
				ImVec2(0, std::max(tabH, ImGui::GetFontSize() * 6))))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, ImGui::GetFontSize() * 1.4f);
			ImGui::TableSetupColumn(tr("Server"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(tr("Players"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, ImGui::GetFontSize() * 4.5f);
			ImGui::TableSetupColumn(tr("Ping"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, ImGui::GetFontSize() * 4.0f);
			ImGui::TableSetupColumn(tr("PW"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.2f);
			ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			std::lock_guard<std::mutex> lk(om);
			if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs(); sp && sp->SpecsDirty && sp->SpecsCount > 0)
			{
				const int col = sp->Specs[0].ColumnIndex;
				const bool ab = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				std::stable_sort(liste.begin(), liste.end(), [&](const Eintrag& a, const Eintrag& b) {
					if (a.antwort != b.antwort) return a.antwort; // erreichbare zuerst
					int x = 0;
					if (col == 1) x = a.name.compare(b.name);
					else if (col == 2) x = a.spieler - b.spieler;
					else if (col == 3) x = a.ping - b.ping;
					else if (col == 4) x = int(a.pw) - int(b.pw);
					else if (col == 5) x = a.motd.compare(b.motd);
					return ab ? x > 0 : x < 0;
				});
				sp->SpecsDirty = false;
				auswahl = -1;
			}
			for (int i = 0; i < static_cast<int>(liste.size()); ++i)
			{
				Eintrag& e = liste[static_cast<size_t>(i)];
				if (nurFrei && e.antwort && e.spieler >= e.max) continue;
				if (nurOffen && e.pw) continue;
				if (nurFav && !e.fav) continue;
				ImGui::TableNextRow();
				ImGui::PushID(i);
				ImGui::TableNextColumn();
				if (ImGui::SmallButton(e.fav ? "*##f" : ".##f"))
				{
					e.fav = !e.fav;
					std::string f;
					for (auto& x : liste) if (x.fav) f += (f.empty() ? "" : ",") + x.adresse;
					c.onlineFavs = f;
					geaendert = true;
				}
				ImGui::TableNextColumn();
				const bool aktuell = e.adresse == c.onlineServer;
				if (ImGui::Selectable((e.antwort ? e.name : e.adresse + tr(" (no answer)")).c_str(), auswahl == i || aktuell,
						ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap))
				{
					auswahl = i;
					if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown) || ImGui::IsKeyPressed(ImGuiKey_Enter))
					{ verbinden = e.adresse; verbindenPw = e.pw; }
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", e.adresse.c_str(), e.proto && e.proto != 1 ? tr("  (other version)") : "");
				ImGui::TableNextColumn();
				if (e.antwort) ImGui::Text("%d/%d", e.spieler, e.max); else ImGui::TextDisabled("-");
				ImGui::TableNextColumn();
				if (e.antwort) ImGui::TextColored(e.ping < 60 ? ImVec4(0.5f, 0.9f, 0.5f, 1) : e.ping < 120 ? ImVec4(0.95f, 0.85f, 0.4f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1), "%d ms", e.ping);
				else ImGui::TextDisabled("-");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(e.pw ? tr("yes") : "");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(e.motd.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		{
			std::lock_guard<std::mutex> lk(om);
			const bool sel = auswahl >= 0 && auswahl < static_cast<int>(liste.size());
			if (!sel) ImGui::BeginDisabled();
			if (ImGui::Button(tr("  Connect  ")) && sel) { verbinden = liste[static_cast<size_t>(auswahl)].adresse; verbindenPw = liste[static_cast<size_t>(auswahl)].pw; }
			if (!sel) ImGui::EndDisabled();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
		ImGui::InputTextWithHint("##adr", tr("address (host:port)"), eingabe, sizeof eingabe);
		ImGui::SameLine();
		if (ImGui::Button(tr("Connect to address")) && eingabe[0]) verbinden = eingabe;
		ImGui::SameLine();
		if (ImGui::Button(tr("Add favourite")) && eingabe[0])
		{
			c.onlineFavs += (c.onlineFavs.empty() ? "" : ",") + std::string(eingabe);
			geaendert = true;
			aktualisieren(c.onlineFavs, c.onlineServer);
		}
		ImGui::TextDisabled("%s", tr("Host your own: ts2-server (see server/ in the project). Permanent listing: server/servers.txt."));

		if (!verbinden.empty())
		{
			if (verbindenPw) { pwFuer = verbinden; pwEingabe[0] = 0; ImGui::OpenPopup("##pwpopup"); }
			else
			{
				c.onlineServer = verbinden;
				c.onlinePw.clear();
				geaendert = true;
				std::lock_guard<std::mutex> lk(om);
				spielStatus = tr("Selected: ") + verbinden + tr(" - now confirm \"TS2 Online\" in the game");
				if (vonSpielGeoeffnet) schliessen = true;
			}
		}
		if (ImGui::BeginPopupModal("##pwpopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
		{
			ImGui::Text(tr("Password for %s"), pwFuer.c_str());
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			const bool enter = ImGui::InputText("##pw", pwEingabe, sizeof pwEingabe, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::Button("OK") || enter)
			{
				c.onlineServer = pwFuer;
				c.onlinePw = pwEingabe;
				geaendert = true;
				{ std::lock_guard<std::mutex> lk(om); spielStatus = tr("Selected: ") + pwFuer + tr(" - now confirm \"TS2 Online\" in the game"); }
				if (vonSpielGeoeffnet) schliessen = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		return geaendert;
	}

	void draw()
	{
		static bool styled = false;
		if (opOeffnen.exchange(false) && active.load())
		{
			s_hubModus = true;
			open.store(true);
		}
		if (!open.load()) s_hubModus = false;
		if (online::oeffnen.exchange(false) && online::aktiv() && active.load())
		{
			online::vonSpielGeoeffnet = true;
			s_onlineTabWaehlen = true;
			open.store(true);
			Cfg c0; { std::lock_guard<std::mutex> lk(m); c0 = cfg; }
			online::aktualisieren(c0.onlineFavs, c0.onlineServer);
		}
		if (!open.load()) online::vonSpielGeoeffnet = false;
		if (const double at = applyAt.load(); at > 0.0 && (ImGui::GetTime() >= at || !open.load()))
		{
			applyAt.store(0.0);
			applyPending.store(true);
		}
		if (!open.load())
		{
			s_prevNav = 0;
			// The first seconds: a subtle hint how to open the menu
			static const auto s_t0 = std::chrono::steady_clock::now();
			const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_t0).count();
			{
				// Input display (debug option, default off): what the game currently gets from the pad/keyboard
				bool show; { std::lock_guard<std::mutex> lk(m); show = cfg.showInput; }
				if (show)
				{
					const u32 b = ps2xosd::btns.load(), ax = ps2xosd::axes.load(), kb = ps2xosd::keyBtns.load();
					static const char* kN[16] = {"SEL", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT", "L2", "R2", "L1", "R1", "TRI", "O", "X", "SQ"};
					std::string txt = fmt::format("L {:3d},{:3d}  R {:3d},{:3d} ", (ax & 0xFF), ((ax >> 8) & 0xFF), ((ax >> 16) & 0xFF), (ax >> 24));
					for (int i = 0; i < 16; ++i) if (b & (1u << i)) { txt += kN[i]; txt += (kb & (1u << i)) ? "(Tast) " : " "; }
					ImDrawList* dl = ImGui::GetForegroundDrawList();
					const float fs = ImGui::GetFontSize();
					const ImVec2 p0(fs * 1.2f, fs * 1.0f);
					const ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
					dl->AddRectFilled(ImVec2(p0.x - fs * 0.4f, p0.y - fs * 0.3f), ImVec2(p0.x + ts.x + fs * 0.4f, p0.y + ts.y + fs * 0.3f), IM_COL32(0, 0, 0, 150), fs * 0.2f);
					dl->AddText(p0, IM_COL32(120, 255, 140, 255), txt.c_str());
				}
			}
			{
				// Own crosshair at the screen centre while playing with the mouse (the game's aim HUD is hidden, aim mode is held for exact shots)
				int art; { std::lock_guard<std::mutex> lk(m); art = cfg.fadenkreuz; }
				if (ps2xosd::ego.load() && !ps2xosd::zoomAktiv.load() && !open.load() && art != 3)
				{
					ImDrawList* dl = ImGui::GetForegroundDrawList();
					ps2xImSpiel spiel(dl); const ImVec2 ds = spiel.ds;
					const float cx = std::floor(ds.x * 0.5f), cy = std::floor(ds.y * 0.5f);
					const float u = std::max(1.0f, std::round(ds.y / 540.0f)); // Strichstaerke
					const float in = 4.0f * u, out = 11.0f * u;
					const ImU32 sch = IM_COL32(0, 0, 0, 170), hell = IM_COL32(255, 255, 255, 235);
					const ImVec2 a[4][2] = {{{cx - out, cy}, {cx - in, cy}}, {{cx + in, cy}, {cx + out, cy}}, {{cx, cy - out}, {cx, cy - in}}, {{cx, cy + in}, {cx, cy + out}}};
					if (art == 0)
					{
						for (const auto& l : a) dl->AddLine(l[0], l[1], sch, u * 3.0f);
						for (const auto& l : a) dl->AddLine(l[0], l[1], hell, u);
					}
					else if (art == 2)
					{
						dl->AddCircle(ImVec2(cx, cy), out, sch, 48, u * 3.0f);
						dl->AddCircle(ImVec2(cx, cy), out, hell, 48, u);
					}
					dl->AddCircleFilled(ImVec2(cx, cy), u * (art == 1 ? 2.6f : 1.6f), sch); dl->AddCircleFilled(ImVec2(cx, cy), u * (art == 1 ? 1.7f : 0.9f), hell);
				}
			}
			{
				// Online+ hit marker (white X, 0.22 s) and kill marker (red X, larger, 0.45 s) around the screen centre
				bool imp, mk; { std::lock_guard<std::mutex> lk(m); imp = cfg.impact != 0; mk = cfg.impactMarker != 0; }
				static const bool impEnv = [] { const char* e = std::getenv("TS2_IMPACT"); return e && *e == '1'; }();
				if (impEnv) { imp = true; mk = true; }
				if (!onlinePlus()) imp = false;
				if (imp && mk && !open.load())
				{
					const long long jetzt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
					const float aT = static_cast<float>(jetzt - ps2xosd::trefferMs.load()) / 1000.0f, aK = static_cast<float>(jetzt - ps2xosd::killMs.load()) / 1000.0f;
					const bool kill = aK >= 0.0f && aK < 0.45f, treffer = aT >= 0.0f && aT < 0.22f;
					if (kill || treffer)
					{
						ImDrawList* dl = ImGui::GetForegroundDrawList();
						ps2xImSpiel spiel(dl); const ImVec2 ds = spiel.ds;
						const float cx = std::floor(ds.x * 0.5f), cy = std::floor(ds.y * 0.5f);
						const float u = std::max(1.0f, std::round(ds.y / 540.0f));
						const float t01 = kill ? aK / 0.45f : aT / 0.22f;
						const float in = (kill ? 9.0f : 7.0f) * u + t01 * 3.0f * u, out = in + (kill ? 10.0f : 7.0f) * u;
						const int al = static_cast<int>(255 * (1.0f - t01 * t01));
						const ImU32 sch = IM_COL32(0, 0, 0, al * 2 / 3), col = kill ? IM_COL32(255, 60, 50, al) : IM_COL32(255, 255, 255, al);
						const float d = 0.7071f;
						for (int q = 0; q < 4; ++q)
						{
							const float sx = (q & 1) ? 1.0f : -1.0f, sy = (q & 2) ? 1.0f : -1.0f;
							const ImVec2 a(cx + sx * in * d, cy + sy * in * d), b(cx + sx * out * d, cy + sy * out * d);
							dl->AddLine(a, b, sch, u * (kill ? 4.0f : 3.0f));
							dl->AddLine(a, b, col, u * (kill ? 2.2f : 1.5f));
						}
						if (kill)
						{
							const char* txt = "+100 XP";
							ImFont* xf = ps2xschrift::fett(); if (!xf) xf = ImGui::GetFont();
							const float xs = ImGui::GetFontSize() * 1.35f;
							const ImVec2 ts = xf->CalcTextSizeA(xs, FLT_MAX, 0.0f, txt);
							const ImVec2 p0(cx - ts.x * 0.5f, cy + out + 6.0f * u - t01 * 8.0f * u);
							dl->AddText(xf, xs, ImVec2(p0.x + u * 1.5f, p0.y + u * 1.5f), IM_COL32(0, 0, 0, al), txt);
							dl->AddText(xf, xs, p0, IM_COL32(255, 210, 90, al), txt);
						}
					}
				}
			}
			{
				// Online+ arcade layer: red edge flash when hit, big announcer text for multi-kills and streaks
				bool imp, rd, an; { std::lock_guard<std::mutex> lk(m); imp = cfg.impact != 0; rd = cfg.impactRand != 0; an = cfg.impactAnsage != 0; }
				static const bool impEnv2 = [] { const char* e = std::getenv("TS2_IMPACT"); return e && *e == '1'; }();
				if (impEnv2) { imp = true; rd = true; an = true; }
				if (!onlinePlus()) imp = false;
				if (imp && !open.load())
				{
					const long long jetzt = ps2xosd::jetztMs();
					ImDrawList* dl = ImGui::GetForegroundDrawList();
					ps2xImSpiel spiel(dl); const ImVec2 ds = spiel.ds;
					const float aS = static_cast<float>(jetzt - ps2xosd::schadenMs.load()) / 1000.0f;
					if (rd && aS >= 0.0f && aS < 0.6f)
					{
						const float k = (1.0f - aS / 0.6f); const float st = 0.35f + 0.65f * ps2xosd::schadenStaerke.load() / 100.0f;
						const int al = static_cast<int>(200 * k * k * st);
						const ImU32 v = IM_COL32(200, 10, 0, al), z = IM_COL32(200, 10, 0, 0);
						const float bx = ds.x * 0.16f, by = ds.y * 0.18f;
						dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(bx, ds.y), v, z, z, v);
						dl->AddRectFilledMultiColor(ImVec2(ds.x - bx, 0), ImVec2(ds.x, ds.y), z, v, v, z);
						dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ds.x, by), v, v, z, z);
						dl->AddRectFilledMultiColor(ImVec2(0, ds.y - by), ImVec2(ds.x, ds.y), z, z, v, v);
					}
					std::string txt; long long ams; int stufe;
					{ std::lock_guard<std::mutex> lk(ps2xosd::am); txt = ps2xosd::ansText; ams = ps2xosd::ansMs; stufe = ps2xosd::ansStufe; }
					const float aA = static_cast<float>(jetzt - ams) / 1000.0f;
					if (false && an && !txt.empty() && aA >= 0.0f && aA < 1.8f)   // now shown in-game by the game's own message system
					{
						// pop in (overshoot), hold, fade out
						const float pop = aA < 0.12f ? 0.6f + 0.6f * (aA / 0.12f) : aA < 0.25f ? 1.2f - 0.2f * ((aA - 0.12f) / 0.13f) : 1.0f;
						const float al = aA > 1.3f ? 1.0f - (aA - 1.3f) / 0.5f : 1.0f;
						const float fs = ImGui::GetFontSize() * (2.4f + 0.25f * std::min(stufe, 4)) * pop;
						ImFont* f = ps2xschrift::kopf(); if (!f) f = ImGui::GetFont();
						const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, txt.c_str());
						const ImVec2 p0(std::floor(ds.x * 0.5f - ts.x * 0.5f), std::floor(ds.y * 0.27f - ts.y * 0.5f));
						const ImU32 farbe = stufe >= 4 ? IM_COL32(255, 70, 40, static_cast<int>(255 * al)) : stufe >= 2 ? IM_COL32(255, 150, 30, static_cast<int>(255 * al)) : IM_COL32(255, 220, 80, static_cast<int>(255 * al));
						const float o = std::max(2.0f, fs * 0.06f);
						for (int q = 0; q < 4; ++q) dl->AddText(f, fs, ImVec2(p0.x + ((q & 1) ? o : -o), p0.y + ((q & 2) ? o : -o)), IM_COL32(0, 0, 0, static_cast<int>(200 * al)), txt.c_str());
						dl->AddText(f, fs, p0, farbe, txt.c_str());
					}
				}
			}
			if (onlinePlus() && !open.load())
			{
				// ---- Online+ killfeed (top right, under the game's radar) and scoreboard (hold Select / Tab) ----
				ImDrawList* dl = ImGui::GetForegroundDrawList();
				ps2xImSpiel spiel(dl); const ImVec2 ds = spiel.ds;
				const long long jetzt = ps2xosd::jetztMs();
				std::vector<ps2xfeed::Zeile> zl; std::string tafel;
				{ std::lock_guard<std::mutex> lk(ps2xfeed::m); zl = ps2xfeed::zeilen; tafel = ps2xfeed::tafel; }
				ImFont* tf = ps2xschrift::fett(); if (!tf) tf = ImGui::GetFont();
				const float fs = std::max(14.0f, ds.y * 0.028f), pad = fs * 0.35f;
				float y = ds.y * 0.205f;
				zl.clear();   // the killfeed runs in-game now (game's own message lines), not as an overlay
				for (const auto& z : zl)
				{
					const float alter = static_cast<float>(jetzt - z.ms) / 1000.0f;
					if (alter < 0.0f || alter > 6.0f) continue;
					const float al = alter > 5.0f ? 6.0f - alter : 1.0f, ein = std::min(1.0f, alter / 0.15f);
					const std::string k = z.killer.empty() ? std::string() : z.killer, o = z.opfer == "?" ? std::string() : z.opfer;
					const char* mitte = (z.fl & 4) ? "  //  " : "  >>  ";
					const ImVec2 sk = tf->CalcTextSizeA(fs, FLT_MAX, 0, k.c_str()), sm = tf->CalcTextSizeA(fs, FLT_MAX, 0, mitte), so = tf->CalcTextSizeA(fs, FLT_MAX, 0, o.c_str());
					const float w = sk.x + sm.x + so.x + pad * 2;
					const float x = ds.x - fs * 0.8f - w + (1.0f - ein) * w * 0.3f;
					const bool ich = (z.fl & 3) != 0;
					dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + fs + pad), ich ? IM_COL32(60, 40, 0, static_cast<int>(170 * al)) : IM_COL32(0, 0, 0, static_cast<int>(140 * al)), fs * 0.2f);
					if (ich) dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + fs + pad), IM_COL32(255, 200, 70, static_cast<int>(200 * al)), fs * 0.2f, 1.5f);
					float cx = x + pad; const float cy = y + pad * 0.5f;
					auto txt = [&](const std::string& t2, ImU32 c, const ImVec2& sz) { dl->AddText(tf, fs, ImVec2(cx + 1, cy + 1), IM_COL32(0, 0, 0, static_cast<int>(200 * al)), t2.c_str()); dl->AddText(tf, fs, ImVec2(cx, cy), c, t2.c_str()); cx += sz.x; };
					const auto mitA = [&](ImU32 c) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(255 * al) << 24); };
					txt(k, mitA((z.fl & 1) ? IM_COL32(255, 210, 90, 255) : IM_COL32(120, 200, 255, 255)), sk);
					txt(mitte, mitA(IM_COL32(255, 90, 60, 255)), sm);
					txt(o, mitA((z.fl & 2) ? IM_COL32(255, 210, 90, 255) : IM_COL32(235, 235, 235, 255)), so);
					y += fs + pad + fs * 0.25f;
				}
				// scoreboard (hold Select on the pad or Tab on the keyboard) - replaces the game's own score screen in Online+
				const u32 b = ps2xosd::btns.load();
				static const bool tafelImmer = std::getenv("PS2XGS_TAFEL_IMMER") != nullptr;   // tests: always show it
				if (((b & 1u) || ps2xfeed::tab.load() || tafelImmer) && ps2xfeed::imMatch())
				{
					struct E { std::string n; int k, d, su, serie, best; bool ich; };
					std::vector<E> e; size_t p0 = 0;
					while (p0 < tafel.size())
					{
						const size_t nl = tafel.find('\n', p0); const std::string l = tafel.substr(p0, nl == std::string::npos ? std::string::npos : nl - p0);
						p0 = nl == std::string::npos ? tafel.size() : nl + 1;
						std::vector<std::string> f; size_t q = 0;
						while (true) { const size_t t2 = l.find('\t', q); f.push_back(l.substr(q, t2 == std::string::npos ? std::string::npos : t2 - q)); if (t2 == std::string::npos) break; q = t2 + 1; }
						if (f.size() < 4) continue;
						auto zi = [&](size_t i) { return i < f.size() ? std::atoi(f[i].c_str()) : 0; };
						e.push_back({f[0], zi(1), zi(2), zi(4), zi(5), zi(6), f[3] == "1"});
					}
					auto punkte = [](const E& x) { return x.k - x.su; };
					std::stable_sort(e.begin(), e.end(), [&](const E& x, const E& y2) { return punkte(x) != punkte(y2) ? punkte(x) > punkte(y2) : x.d < y2.d; });
					ImFont* kf = ps2xschrift::kopf(); if (!kf) kf = ImGui::GetFont();
					const float zh = fs * 1.45f, bw = std::min(ds.x * 0.86f, fs * 40.0f), x0 = (ds.x - bw) * 0.5f;
					const float hh = fs * 2.7f, y0 = ds.y * 0.10f, h = hh + zh * (std::max<size_t>(e.size(), 1) + 1) + fs * 3.4f;
					dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + h), IM_COL32(8, 10, 24, 232), fs * 0.4f);
					dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + h), IM_COL32(80, 160, 255, 200), fs * 0.4f, 2.0f);
					dl->AddText(kf, fs * 1.3f, ImVec2(x0 + fs, y0 + fs * 0.6f), IM_COL32(110, 210, 255, 255), "SCOREBOARD");
					const int lv = ps2xstats::level();
					const std::string rg = fmt::format("LV {}  {}", lv, ps2xRang(lv));
					const ImVec2 rs = tf->CalcTextSizeA(fs, FLT_MAX, 0, rg.c_str());
					dl->AddText(tf, fs, ImVec2(x0 + bw - fs - rs.x, y0 + fs * 0.85f), ps2xRangFarbe(lv), rg.c_str());
					// columns: # | player | score | kills | deaths | suicides | K/D | streak | best
					const float sp[] = {x0 + fs, x0 + fs * 2.6f, x0 + bw - fs * 24.0f, x0 + bw - fs * 20.5f, x0 + bw - fs * 17.0f, x0 + bw - fs * 13.3f,
						x0 + bw - fs * 9.6f, x0 + bw - fs * 6.4f, x0 + bw - fs * 3.2f};
					const char* kopfz[] = {"#", "PLAYER", "SCORE", "KILLS", "DEATHS", "SUIC.", "K/D", "STREAK", "BEST"};
					float yy = y0 + hh;
					for (int i = 0; i < 9; ++i) dl->AddText(tf, fs * 0.82f, ImVec2(sp[i], yy), IM_COL32(140, 150, 180, 255), kopfz[i]);
					yy += zh;
					int platz = 0; const E* ich = nullptr; int meinPlatz = 0;
					for (const auto& x : e)
					{
						++platz;
						if (x.ich) { ich = &x; meinPlatz = platz; dl->AddRectFilled(ImVec2(x0 + fs * 0.4f, yy - fs * 0.15f), ImVec2(x0 + bw - fs * 0.4f, yy + fs * 1.2f), IM_COL32(70, 50, 0, 200), fs * 0.2f); }
						const ImU32 c = x.ich ? IM_COL32(255, 210, 90, 255) : (platz == 1 ? IM_COL32(255, 240, 180, 255) : IM_COL32(230, 230, 235, 255));
						const std::string sv[] = {std::to_string(platz), x.ich ? std::string("YOU") : x.n, std::to_string(punkte(x)), std::to_string(x.k), std::to_string(x.d), std::to_string(x.su),
							fmt::format("{:.2f}", x.d ? static_cast<float>(x.k) / x.d : static_cast<float>(x.k)), std::to_string(x.serie), std::to_string(x.best)};
						for (int i = 0; i < 9; ++i) dl->AddText(tf, fs, ImVec2(sp[i], yy), c, sv[i].c_str());
						yy += zh;
					}
					if (e.empty()) dl->AddText(tf, fs, ImVec2(sp[1], yy), IM_COL32(160, 160, 170, 255), "Waiting for match data...");
					// footer: own progress this session
					yy = y0 + h - fs * 2.6f;
					dl->AddLine(ImVec2(x0 + fs, yy - fs * 0.3f), ImVec2(x0 + bw - fs, yy - fs * 0.3f), IM_COL32(80, 160, 255, 120), 1.5f);
					const long long xp = ps2xstats::xp(), a0 = ps2xstats::xpFuer(lv), b0 = ps2xstats::xpFuer(lv + 1);
					const std::string fz = ich ? fmt::format("Place {} of {}    Kills {}    Best streak {}    XP {} / {}", meinPlatz, e.size(), ich->k, ich->best, xp, lv >= 50 ? xp : b0)
					                            : fmt::format("XP {} / {}", xp, lv >= 50 ? xp : b0);
					dl->AddText(tf, fs * 0.95f, ImVec2(x0 + fs, yy), IM_COL32(200, 205, 220, 255), fz.c_str());
					const float bx0 = x0 + fs, bx1 = x0 + bw - fs, by = yy + fs * 1.3f;
					const float frac = lv >= 50 ? 1.0f : std::clamp(static_cast<float>(xp - a0) / static_cast<float>(std::max(1LL, b0 - a0)), 0.0f, 1.0f);
					dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + fs * 0.45f), IM_COL32(40, 45, 70, 255), fs * 0.2f);
					dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * frac, by + fs * 0.45f), ps2xRangFarbe(lv), fs * 0.2f);
				}
			}
			{
				std::string tt; std::chrono::steady_clock::time_point ta;
				{ std::lock_guard<std::mutex> lk(ps2xhot::tm); tt = ps2xhot::toastText; ta = ps2xhot::toastAt; }
				const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - ta).count();
				if (!tt.empty() && age < 2.5f)
				{
					const float al = std::clamp((2.5f - age) / 0.5f, 0.0f, 1.0f);
					ImDrawList* dl = ImGui::GetForegroundDrawList();
					const float fs = ImGui::GetFontSize();
					const ImVec2 ts = ImGui::CalcTextSize(tt.c_str());
					const ImVec2 p0(ImGui::GetIO().DisplaySize.x - ts.x - fs * 1.6f, fs * 1.2f);
					dl->AddRectFilled(ImVec2(p0.x - fs * 0.6f, p0.y - fs * 0.4f), ImVec2(p0.x + ts.x + fs * 0.6f, p0.y + ts.y + fs * 0.4f), IM_COL32(10, 10, 8, static_cast<int>(170 * al)), fs * 0.3f);
					dl->AddText(p0, IM_COL32(245, 222, 160, static_cast<int>(255 * al)), tt.c_str());
				}
			}
			static const bool hinweisAus = std::getenv("TS2_LAUNCHER") != nullptr;   // launcher users got the hint after the setup
			if (t < 15.0f && !hinweisAus)
			{
				const float a = std::clamp((15.0f - t) / 2.0f, 0.0f, 1.0f);
				const char* txt = tr("Menu: ESC / F1   |   Gamepad: Select + Start (or Guide)");
				ImDrawList* dl = ImGui::GetForegroundDrawList();
				const float fs = ImGui::GetFontSize();
				const ImVec2 ts = ImGui::CalcTextSize(txt);
				const ImVec2 p0(fs * 1.2f, ImGui::GetIO().DisplaySize.y - ts.y - fs * 1.6f);
				dl->AddRectFilled(ImVec2(p0.x - fs * 0.6f, p0.y - fs * 0.4f), ImVec2(p0.x + ts.x + fs * 0.6f, p0.y + ts.y + fs * 0.4f), IM_COL32(10, 10, 8, static_cast<int>(170 * a)), fs * 0.3f);
				dl->AddText(p0, IM_COL32(245, 222, 160, static_cast<int>(255 * a)), txt);
			}
			return;
		}
		feedInput();
		if (!styled) { style(); styled = true; }
		ImGuiIO& io = ImGui::GetIO();
		const ImVec2 ds = io.DisplaySize;
		// Hintergrund abdunkeln
		ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(8, 8, 6, 150));
		const float w = std::min(ds.x * 0.62f, 980.0f * ImGuiManager::GetGlobalScale());
		const float h = std::min(ds.y * 0.80f, 820.0f * ImGuiManager::GetGlobalScale());
		ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
		ImGui::Begin("##ps2xmenu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

		Cfg c;
		{ std::lock_guard<std::mutex> lk(m); c = cfg; }
		const Cfg before = c;
		bool closeReq = false, quitReq = false;
		// TS2 packages: English (US) is the default for everything; German only when chosen. Other games keep "auto".
		sprache.store(c.lang < 0 ? (std::strcmp(cfgName(), "ts2-recomp") == 0 ? 0 : spracheAuto()) : c.lang);

		if (s_hubModus)
		{
			// ---- Online+ hub (opened from the game's main menu item "Online+") ----
			const ImVec4 gold(1.00f, 0.80f, 0.32f, 1.0f), cyan(0.42f, 0.86f, 1.00f, 1.0f);
			ImGui::PushFont(ps2xschrift::text(), ImGui::GetFontSize() * 1.2f);
			ImGui::PushFont(ps2xschrift::kopf(), ImGui::GetFontSize() * 1.9f);
			ImGui::TextColored(cyan, "ONLINE");
			ImGui::SameLine(0, 0);
			ImGui::TextColored(gold, "+");
			ImGui::PopFont();
			ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
			{
				const int lv = ps2xstats::level();
				ImGui::BeginGroup();
				ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
				ImGui::TextColored(gold, "%s %d", tr("Level"), lv);
				ImGui::SameLine();
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ps2xRangFarbe(lv)), "%s", ps2xRang(lv));
				ImGui::EndGroup();
			}
			ImGui::TextDisabled("%s", tr("TimeSplitters 2 Online+ - online play, progression and extra impact."));
			ImGui::Spacing();
			static int seite = 0;
			// PS2XGS_DEMO=<bot match code> (showcase recordings): show every hub page for a moment, then start that bot match
			static const int demoCode = [] { const char* e = std::getenv("PS2XGS_DEMO"); return e ? std::atoi(e) : 0; }();
			static double demoAb = -1.0; static bool demoFertig = false; static bool demoStart = false;
			if (demoCode > 0 && !demoFertig)
			{
				if (demoAb < 0) demoAb = ImGui::GetTime();
				const double d = ImGui::GetTime() - demoAb;
				const int folge[] = {2, 3, 1, 0};
				const int i = static_cast<int>(d / 2.6);
				if (i < 4) seite = folge[i]; else { demoFertig = true; demoStart = true; }
			}
			const char* seiten[] = {tr("BOT MATCH"), tr("PLAY ONLINE"), tr("PROFILE & STATS"), tr("SETTINGS")};
			const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
			for (int i = 0; i < 4; ++i)
			{
				if (i) ImGui::SameLine();
				const bool akt = seite == i;
				ImGui::PushStyleColor(ImGuiCol_Button, akt ? ImVec4(0.16f, 0.36f, 0.62f, 1.0f) : ImVec4(0.10f, 0.12f, 0.20f, 0.95f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.46f, 0.78f, 1.0f));
				ImGui::PushFont(ps2xschrift::kopf(), ImGui::GetFontSize() * 0.85f);
				if (ImGui::Button(seiten[i], ImVec2(bw, ImGui::GetFontSize() * 2.0f))) seite = i;
				ImGui::PopFont();
				ImGui::PopStyleColor(2);
			}
			ImGui::Spacing();
			const float fussH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
			ImGui::BeginChild("##hub", ImVec2(0, -fussH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
			if (seite == 0)
			{
				static int modus = 0, karte = 0, botSet = 0;
				if (demoCode > 0) { const int v = demoCode - 1; karte = v & 15; modus = (v >> 4) % 3; botSet = std::min((v >> 4) / 3, kBotSetsN - 1); }
				const char* kModi[] = {"Deathmatch", "Team Deathmatch", "Capture the Bag"};
				ImGui::TextWrapped("%s", tr("Jump straight into a match against bots. Pick a mode and a map, the game sets everything up for you."));
				ImGui::Spacing();
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
				combo(tr("Mode"), &modus, kModi, 3);
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
				const bool ctb = (modus == 2);
				const int kartenN = ctb ? kBotKartenCtbN : kBotKartenN;
				if (karte >= kartenN) karte = 0;
				combo(tr("Map"), &karte, ctb ? kBotKartenCtb : kBotKarten, kartenN);
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
				combo(tr("Opponents"), &botSet, kBotSets, kBotSetsN);
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.10f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.58f, 0.18f, 1.0f));
				ImGui::PushFont(ps2xschrift::kopf(), ImGui::GetFontSize() * 1.0f);
				if (ImGui::Button(tr("START BOT MATCH"), ImVec2(ImGui::GetFontSize() * 14, ImGui::GetFontSize() * 2.2f)))
				{
					botAnfrage.store(1 + (botSet * 3 + modus) * 16 + karte);
					closeReq = true;
				}
				if (demoStart) { demoStart = false; botAnfrage.store(demoCode); closeReq = true; }
				ImGui::PopFont();
				ImGui::PopStyleColor(2);
				ImGui::TextDisabled("%s", tr("Don't touch the controller for a few seconds while the menus run."));
				ImGui::TextDisabled("%s", tr("* = has to be unlocked in the game first (Arcade League / Challenge)."));
			}
			else if (seite == 1)
			{
				if (online::aktiv()) onlineTab(c, closeReq);
				else ImGui::TextWrapped("%s", tr("Online play is not available in this start mode."));
			}
			else if (seite == 2)
			{
				const int lv = ps2xstats::level(); const long long x = ps2xstats::xp();
				const long long a0 = ps2xstats::xpFuer(lv), b0 = ps2xstats::xpFuer(lv + 1);
				ImGui::PushFont(ps2xschrift::kopf(), ImGui::GetFontSize() * 1.3f);
				ImGui::TextColored(gold, "%s %d", tr("Level"), lv);
				ImGui::SameLine(0, ImGui::GetFontSize() * 0.8f);
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ps2xRangFarbe(lv)), "%s", ps2xRang(lv));
				ImGui::PopFont();
				if (lv < 50) ImGui::TextDisabled("%s %s (%s %d)", tr("Next rank:"), ps2xRang(std::min(50, (lv / 5 + 1) * 5)), tr("Level"), std::min(50, (lv / 5 + 1) * 5));
				ImGui::ProgressBar(lv >= 50 ? 1.0f : static_cast<float>(x - a0) / static_cast<float>(std::max(1LL, b0 - a0)), ImVec2(-1, ImGui::GetFontSize() * 1.4f),
					fmt::format("{} / {} XP", x, lv >= 50 ? x : b0).c_str());
				ImGui::Spacing();
				if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
				{
					auto zeile = [](const char* k, const std::string& v) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k); ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str()); };
					zeile(tr("Kills"), std::to_string(ps2xstats::kills()));
					zeile(tr("Hits"), std::to_string(ps2xstats::hits()));
					zeile(tr("Total XP"), std::to_string(x));
					zeile(tr("XP to next level"), lv >= 50 ? std::string("-") : std::to_string(b0 - x));
					ImGui::EndTable();
				}
				ImGui::Spacing();
				ImGui::TextDisabled("%s", tr("Hit +10 XP, kill +100 XP. Saved locally; online servers will track it later."));
			}
			else
			{
				bool an = c.impact != 0;
				if (ImGui::Checkbox(tr("Impact effects (Online+)"), &an)) c.impact = an ? 1 : 0;
				ImGui::BeginDisabled(!an);
				bool mk = c.impactMarker != 0;
				if (ImGui::Checkbox(tr("Hit and kill markers"), &mk)) c.impactMarker = mk ? 1 : 0;
				bool sn = c.impactSound != 0;
				if (ImGui::Checkbox(tr("Hit and kill sounds"), &sn)) c.impactSound = sn ? 1 : 0;
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
				ImGui::SliderInt(tr("Camera kick when hit"), &c.impactKick, 0, 100, "%d %%");
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
				ImGui::SliderInt(tr("Recoil punch when firing"), &c.impactRecoil, 0, 100, "%d %%");
				bool an2 = c.impactAnsage != 0;
				if (ImGui::Checkbox(tr("Multi-kill and streak announcer"), &an2)) c.impactAnsage = an2 ? 1 : 0;
				bool rd = c.impactRand != 0;
				if (ImGui::Checkbox(tr("Red screen edge when hit"), &rd)) c.impactRand = rd ? 1 : 0;
				ImGui::EndDisabled();
				ImGui::Spacing();
				ImGui::TextDisabled("%s", tr("All other settings: ESC / F1 during the game."));
			}
			ImGui::EndChild();
			ImGui::Separator();
			const float bw2 = ImGui::CalcTextSize(tr("  Back to main menu  ")).x + ImGui::GetStyle().FramePadding.x * 2;
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw2 - ImGui::GetStyle().WindowPadding.x);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.45f, 0.22f, 0.95f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.65f, 0.30f, 1.0f));
			if (ImGui::Button(tr("  Back to main menu  "))) closeReq = true;
			ImGui::PopStyleColor(2);
			ImGui::PopFont();
		}
		else
		{
		// Header: game title (cyan), language on the right; below it a green line with the keys
		{
			const ImVec4 cyan(0.42f, 0.86f, 1.00f, 1.0f), green(0.50f, 1.00f, 0.42f, 1.0f);
			ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.7f);
			ImGui::TextColored(cyan, "%s", spielTitel());
			ImGui::PopFont();
			ImGui::SameLine(0, ImGui::GetFontSize() * 1.2f);
			ImGui::TextColored(green, "%s", tr("Settings"));
			const char* kLang[] = {"Auto", "English", "Deutsch"};
			const float lw = ImGui::GetFontSize() * 7.5f;
			ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - lw);
			ImGui::SetNextItemWidth(lw);
			int li = c.lang + 1;
			if (ImGui::Combo("##lang", &li, kLang, 3)) c.lang = li - 1;
			ImGui::TextDisabled("%s", tr("ESC / F1 / Guide: menu   F7: free camera   F8: record video   F9: FPS   F10: turbo   F11 or Alt+Enter: fullscreen   F12: screenshot"));
			ImGui::Spacing();
		}

		const auto head = [](const char* t) { ImGui::SeparatorText(t); };
		// The content scrolls in its own area, the footer always stays visible
		const float fussH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
		ImGui::BeginChild("##inhalt", ImVec2(0, -fussH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
		if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(tr("Display")))
			{
				ImGui::TextDisabled("%s:", tr("Preset"));
				ImGui::SameLine();
				if (ImGui::Button(tr("Remaster (sharp)")))
				{ c.upscale = 4; c.aniso = 16; c.filter = 2; c.cas = 1; c.casSharp = 25; c.fxaa = false; c.tv = 0; c.shadeboost = false; c.dither = 0; c.camBlur = 1; c.nachzieh = 0; c.scope = 1; c.aspect = 4; }
				ImGui::SameLine();
				if (ImGui::Button(tr("Original look")))
				{ c.upscale = 1; c.aniso = 0; c.filter = 2; c.cas = 0; c.fxaa = false; c.tv = 5; c.shadeboost = false; c.aspect = 2; c.dither = 2; c.camBlur = 1; c.nachzieh = 1; c.scope = 0; c.padModern = 0; c.padSens = 100; c.unlockAll = 0; }
				ImGui::SameLine();
				if (ImGui::Button(tr("Performance")))
				{ c.upscale = 2; c.aniso = 4; c.filter = 2; c.cas = 1; c.casSharp = 40; c.fxaa = false; c.tv = 0; }
				head(tr("Resolution & filtering"));
				static const char* kRes[] = {"1x  original (512x448)", "2x  (~720p)", "3x  (~1080p)", "4x  (~1440p)", "5x", "6x  (~4K)", "7x", "8x  (~5K)", "Auto (best for this screen, smooth edges)"};
				int r = c.upscale - 1;
				if (combo(tr("Internal resolution"), &r, kRes, 9)) c.upscale = r + 1;
				static const char* kAspEn[] = {"16:9 widescreen (more view)", "As set in the game's options", "4:3 original", "Fill window (stretched)", "Fit any screen (16:9, 16:10, 21:9, 32:9)"};
				static const char* kAspDe[] = {"16:9 Breitbild (mehr Sichtfeld)", "Wie im Spiel-Optionsmenue", "4:3 Original", "Fenster fuellen (gestreckt)", "Passt zu jedem Bildschirm (16:9, 16:10, 21:9, 32:9)"};
				combo(tr("Aspect ratio"), &c.aspect, sprache.load() == 1 ? kAspDe : kAspEn, 5);
				if (c.aspect != before.aspect)
					ImGui::TextDisabled("%s", tr("Takes full effect after the next area change or restart."));
				static const char* kFiltEn[] = {"Pixelated (nearest)", "Bilinear (always)", "Bilinear (as PS2)", "Bilinear (no sprites)"};
				static const char* kFiltDe[] = {"Pixelig (Nearest)", "Bilinear (immer)", "Bilinear (wie PS2)", "Bilinear (ohne Sprites)"};
				combo(tr("Texture filtering"), &c.filter, sprache.load() == 1 ? kFiltDe : kFiltEn, 4);
				static const char* kAniEn[] = {"Off", "2x", "4x", "8x", "16x"};
				static const char* kAniDe[] = {"Aus", "2x", "4x", "8x", "16x"};
				int ai = c.aniso == 0 ? 0 : c.aniso == 2 ? 1 : c.aniso == 4 ? 2 : c.aniso == 8 ? 3 : 4;
				if (combo(tr("Anisotropic filtering"), &ai, sprache.load() == 1 ? kAniDe : kAniEn, 5)) c.aniso = ai == 0 ? 0 : (1 << ai);
				static const char* kTriEn[] = {"Automatic", "As PS2", "Forced (recommended with texture packs)"};
				static const char* kTriDe[] = {"Automatisch", "Wie PS2", "Erzwungen (empfohlen mit Texturpaketen)"};
				combo(tr("Trilinear filtering"), &c.tri, sprache.load() == 1 ? kTriDe : kTriEn, 3);
				head(tr("Window"));
				ImGui::Checkbox(tr("Fullscreen"), &c.fullscreen);
				ImGui::SameLine(0, ImGui::GetFontSize() * 2);
				ImGui::Checkbox(tr("V-Sync"), &c.vsync);
				ImGui::SameLine(0, ImGui::GetFontSize() * 2);
				ImGui::Checkbox(tr("Show FPS"), &c.showFps);
				ImGui::Checkbox(tr("Open this menu at start"), &c.startMenu);
				ImGui::SameLine(0, ImGui::GetFontSize() * 2);
				ImGui::Checkbox(tr("Input overlay (debug)"), &c.showInput);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Picture")))
			{
				{
					bool nz = c.nachzieh != 0;
					if (ImGui::Checkbox(tr("Original frame blending (ghosting when turning)"), &nz)) c.nachzieh = nz ? 1 : 0;
					ImGui::TextDisabled("%s", tr("The PS2 game blends the previous frame half over the new one. Off = clear picture."));
				}
				{
					const char* kScEn[] = {"Original (smeared)", "Clear", "Extra clear"};
					const char* kScDe[] = {"Original (verschwommen)", "Klar", "Extra klar"};
					combo(tr("Sniper scope"), &c.scope, sprache.load() == 1 ? kScDe : kScEn, 3);
				}
				ImGui::Checkbox(tr("Anti-aliasing (FXAA)"), &c.fxaa);
				static const char* kCasEn[] = {"Off", "Sharpen", "Sharpen + upscale"};
				static const char* kCasDe[] = {"Aus", "Schaerfen", "Schaerfen + Skalieren"};
				combo(tr("Sharpening (AMD CAS)"), &c.cas, sprache.load() == 1 ? kCasDe : kCasEn, 3);
				if (c.cas) ImGui::SliderInt(tr("Sharpening strength"), &c.casSharp, 0, 100);
				static const char* kDithEn[] = {"Off (clean, recommended)", "Scaled", "Original (unscaled)"};
				static const char* kDithDe[] = {"Aus (sauber, empfohlen)", "Skaliert", "Original (unskaliert)"};
				combo(tr("PS2 dithering"), &c.dither, sprache.load() == 1 ? kDithDe : kDithEn, 3);
				static const char* kTvEn[] = {"None", "Scanlines", "Diagonal", "Triangular", "Wave", "CRT (Lottes)", "4x RGSS", "NxAGSS"};
				static const char* kTvDe[] = {"Kein Filter", "Scanlines", "Diagonal", "Dreieck", "Welle", "CRT (Lottes)", "4x RGSS", "NxAGSS"};
				combo(tr("TV / CRT filter"), &c.tv, sprache.load() == 1 ? kTvDe : kTvEn, 8);
				head(tr("Colour"));
				ImGui::Checkbox(tr("Colour adjustment"), &c.shadeboost);
				if (c.shadeboost)
				{
					ImGui::SliderInt(tr("Brightness"), &c.sbB, 1, 100);
					ImGui::SliderInt(tr("Contrast"), &c.sbC, 1, 100);
					ImGui::SliderInt(tr("Saturation"), &c.sbS, 1, 100);
					if (ImGui::Button(tr("Reset colours"))) { c.sbB = c.sbC = c.sbS = 50; }
				}
#ifndef _WIN32
				head(tr("Post effects (vkBasalt)"));
				static const char* kFxEn[] = {"Off", "Original+ (SMAA only)", "Clear (SMAA + sharpen + contrast)", "Cinematic (SMAA + sharpen + colour grade)"};
				static const char* kFxDe[] = {"Aus", "Original+ (nur SMAA)", "Klar (SMAA + Schaerfe + Kontrast)", "Filmisch (SMAA + Schaerfe + Farblook)"};
				combo("##fx", &c.fx, sprache.load() == 1 ? kFxDe : kFxEn, 4);
				ImGui::TextDisabled("%s", tr("Applies at the next start. Home key toggles the effects in game. Needs vkBasalt installed."));
#endif
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Audio")))
			{
				ImGui::SliderInt(tr("Volume"), &c.volume, 0, 200, "%d %%");
				ImGui::TextDisabled("%s", tr("Original sound chip (SPU2) at 48 kHz. Sound is paused while this menu is open."));
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Controls")))
			{
				head(tr("Gamepad"));
				ImGui::Checkbox(tr("Rumble"), &c.rumble);
				if (c.rumble) { ImGui::SameLine(0, ImGui::GetFontSize() * 2); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12); ImGui::SliderInt(tr("Rumble strength"), &c.rumblePct, 0, 100, "%d %%"); }
				ImGui::TextDisabled("%s", tr("Any modern gamepad is detected automatically (Xbox, PlayStation, Switch, Deck)."));
				head(tr("Mouse"));
				ImGui::Checkbox(tr("Mouse aiming"), &c.maus);
				if (c.maus)
				{
					float sens = static_cast<float>(c.mausSens) / 10.f;
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
					if (ImGui::SliderFloat(tr("Sensitivity"), &sens, 0.1f, 10.f, "%.1f"))
						c.mausSens = std::clamp(static_cast<int>(sens * 10.f + 0.5f), 1, 100);
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
					ImGui::SliderInt(tr("Zoom sensitivity"), &c.zoomSens, 5, 200, "%d %%");
					ImGui::Checkbox(tr("Invert vertical"), &c.mausInvert);
					(void)c.mausZielmodus;   // mouse aiming always uses the exact aim mode now (ts2_override maus::tick)
					{
						const char* kFkEn[] = {"Cross", "Dot", "Circle", "Off"};
						const char* kFkDe[] = {"Kreuz", "Punkt", "Kreis", "Aus"};
						ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
						ImGui::Combo(tr("Crosshair"), &c.fadenkreuz, sprache.load() == 1 ? kFkDe : kFkEn, 4);
						ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
						ImGui::SliderInt(tr("Look up/down limit"), &c.nickMax, 40, 85, "%d\xC2\xB0");
					}
					ImGui::TextDisabled("%s", tr("Click into the game to capture the mouse, ESC or F1 releases it."));
				}
				if (std::strcmp(cfgName(), "ts2-recomp") == 0)
				{
					head(tr("Gamepad"));
					bool pm = c.padModern != 0;
					if (ImGui::Checkbox(tr("Modern aiming (right stick turns the view, crosshair fixed in the centre)"), &pm)) c.padModern = pm ? 1 : 0;
					ImGui::TextDisabled("%s", tr("Off = the original PS2 controls. On: no auto-aim, L2 zooms, shots land on the crosshair."));
					if (pm) { ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14); ImGui::SliderInt(tr("Turn speed"), &c.padSens, 20, 300, "%d %%"); }
				}
				head(tr("Key bindings"));
				ImGui::TextDisabled("%s", tr("Click a field, then press a key or mouse button. Delete clears, ESC cancels."));
				{
					// take over the result from the window thread
					const int la = lernAktion.load();
					if (la >= 0)
					{
						const int code = lernCode.exchange(0);
						if (code == -1) lernAktion.store(-1);
						else if (code != 0)
						{
							const int pl = lernPlatz.load();
							const int neu = code == 0xffff ? 0 : code; // Entf leert
							if (neu) for (int i = 0; i < kAnzAktionen; ++i) for (int j = 0; j < 2; ++j) if (c.bel[i][j] == neu) c.bel[i][j] = 0; // doppelte entfernen
							c.bel[la][pl] = neu;
							lernAktion.store(-1);
						}
					}
				}
				if (ImGui::BeginTable("belegung", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
					ImGui::TableSetupColumn(tr("Primary"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
					ImGui::TableSetupColumn(tr("Secondary"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
					ImGui::TableHeadersRow();
					for (int i = 0; i < kAnzAktionen; ++i)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(kAktionen[i].name));
						for (int j = 0; j < 2; ++j)
						{
							ImGui::TableNextColumn();
							const bool lernt = lernAktion.load() == i && lernPlatz.load() == j;
							const std::string t = (lernt ? std::string(tr("... press a key ...")) : tastenName(c.bel[i][j])) + "##b" + std::to_string(i * 2 + j);
							if (ImGui::Button(t.c_str(), ImVec2(-1.f, 0.f)) && !lernt)
							{
								lernCode.store(0); lernPlatz.store(j); lernAb.store(ImGui::GetTime() + 0.25); lernAktion.store(i);
							}
						}
					}
					ImGui::EndTable();
				}
				if (ImGui::Button(tr("Reset to defaults")))
				{
					for (int i = 0; i < kAnzAktionen; ++i) { c.bel[i][0] = kAktionen[i].std1; c.bel[i][1] = kAktionen[i].std2; }
					c.belVersion = kBelegungVersion;
				}
				ImGui::EndTabItem();
			}
			if (onlinePlus() && ImGui::BeginTabItem(tr("Online+"), nullptr, s_opTabWaehlen ? ImGuiTabItemFlags_SetSelected : 0))
			{
				s_opTabWaehlen = false;
				ImGui::SeparatorText(tr("Play online"));
				if (online::aktiv())
				{
					if (ImGui::Button(tr("Open server browser"))) { s_onlineTabWaehlen = true; Cfg c0 = c; online::aktualisieren(c0.onlineFavs, c0.onlineServer); }
					ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Join a server, then pick Network Game in the game."));
				}
				else ImGui::TextDisabled("%s", tr("Online play is not available in this start mode."));
				ImGui::SeparatorText(tr("Effects"));
				bool an = c.impact != 0;
				if (ImGui::Checkbox(tr("Impact effects (Online+)"), &an)) c.impact = an ? 1 : 0;
				ImGui::TextDisabled("%s", tr("Extra feedback on top of the original game. Off = the game exactly as on the PS2."));
				ImGui::BeginDisabled(!an);
				bool mk = c.impactMarker != 0;
				if (ImGui::Checkbox(tr("Hit and kill markers"), &mk)) c.impactMarker = mk ? 1 : 0;
				bool sn = c.impactSound != 0;
				if (ImGui::Checkbox(tr("Hit and kill sounds"), &sn)) c.impactSound = sn ? 1 : 0;
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
				ImGui::SliderInt(tr("Camera kick when hit"), &c.impactKick, 0, 100, "%d %%");
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
				ImGui::SliderInt(tr("Recoil punch when firing"), &c.impactRecoil, 0, 100, "%d %%");
				bool an2 = c.impactAnsage != 0;
				if (ImGui::Checkbox(tr("Multi-kill and streak announcer"), &an2)) c.impactAnsage = an2 ? 1 : 0;
				bool rd = c.impactRand != 0;
				if (ImGui::Checkbox(tr("Red screen edge when hit"), &rd)) c.impactRand = rd ? 1 : 0;
				ImGui::EndDisabled();
				head(tr("Progress"));
				{
					const int lv = ps2xstats::level(); const long long x = ps2xstats::xp();
					const long long a = ps2xstats::xpFuer(lv), b = ps2xstats::xpFuer(lv + 1);
					ImGui::Text("%s %d", tr("Level"), lv);
					ImGui::ProgressBar(lv >= 50 ? 1.0f : static_cast<float>(x - a) / static_cast<float>(std::max(1LL, b - a)), ImVec2(ImGui::GetFontSize() * 18, 0),
						fmt::format("{} / {} XP", x, lv >= 50 ? x : b).c_str());
					ImGui::Text("%s: %d    %s: %d", tr("Kills"), ps2xstats::kills(), tr("Hits"), ps2xstats::hits());
					ImGui::TextDisabled("%s", tr("Hit +10 XP, kill +100 XP. Saved locally; online servers will track it later."));
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Textures")))
			{
				static std::vector<std::string> quellen; static double lastScan = -1e9;
				static std::vector<std::string> actCache; static std::string actKey;
				if (ImGui::GetTime() - lastScan > 2.0) { quellen = listSources(); lastScan = ImGui::GetTime(); actKey.clear(); }
				static unsigned linked = 0; static bool dirty = false;
				if (ImGui::Checkbox(tr("HD text (own re-typesetting for text pages without a pack)"), &c.hdText)) dirty = true;
				head(tr("Texture packs (PCSX2 format)"));
				if (actKey != c.packs || actKey.empty()) { actCache = activePacks(c.packs); actKey = c.packs.empty() ? std::string("-") : c.packs; }
				const auto& act = actCache;
				if (quellen.empty()) ImGui::TextDisabled("%s", tr("no packs found"));
				for (const auto& q : quellen)
				{
					bool on = std::find(act.begin(), act.end(), q) != act.end();
					if (ImGui::Checkbox(q.c_str(), &on)) { setPackActive(c.packs, q, on); dirty = true; }
				}
				ImGui::TextDisabled(tr("New packs: put the archive into the project's 'texturpakete' folder, %s imports it."), startName());
				ImGui::TextDisabled(tr("Or extracted to %s/<name>/"), quellenDir().c_str());
				if (ImGui::Button(dirty ? tr("Apply and reload textures") : tr("Reload textures")))
				{
					{ std::lock_guard<std::mutex> lk(m); cfg = c; }
					linked = assembleTextures();
					if (g_gs_renderer) { GSTextureReplacements::ReloadReplacementMap(); g_gs_renderer->PurgeTextureCache(true, false, true); }
					dirty = false;
				}
				if (linked) { ImGui::SameLine(); ImGui::TextDisabled(tr("%u files active"), linked); }
				ImGui::TextDisabled("%s", tr("Large packs are preloaded into video memory (short pause)."));
				ImGui::EndTabItem();
			}
			if (online::aktiv() && ImGui::BeginTabItem(tr("Online"), nullptr, s_onlineTabWaehlen ? ImGuiTabItemFlags_SetSelected : 0))
			{
				s_onlineTabWaehlen = false;
				static bool einmal = false;
				if (!einmal) { einmal = true; if (online::liste.empty()) online::aktualisieren(c.onlineFavs, c.onlineServer); }
				onlineTab(c, closeReq);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Save data")))
			{
				ImGui::TextWrapped("%s", memcardInfo().c_str());
				ImGui::TextDisabled("%s", tr(speicherHinweis()));
				ImGui::TextDisabled("%s", tr("A backup copy of the save data is made at every start."));
				if (std::strcmp(cfgName(), "ts2-recomp") == 0)
				{
					ImGui::Separator();
					bool ua = c.unlockAll != 0;
					if (ImGui::Checkbox(tr("Unlock everything (levels, characters, cheats)"), &ua)) c.unlockAll = ua ? 1 : 0;
					ImGui::TextDisabled("%s", tr("Only while this is on. Your own progress and save data stay untouched."));
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::EndChild();
		// Fusszeile: Buttons unten rechts
		{
			const float bw = ImGui::CalcTextSize(tr("  Back to game  ")).x + ImGui::CalcTextSize(tr("  Quit game  ")).x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
			ImGui::Separator();
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.45f, 0.22f, 0.95f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.65f, 0.30f, 1.0f));
			if (ImGui::Button(tr("  Back to game  ")))
				closeReq = true;
			ImGui::PopStyleColor(2);
			ImGui::SameLine();
			if (ImGui::Button(tr("  Quit game  ")))
				quitReq = true;
		}
		}
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere(-1);
		ImGui::End();

		const bool changed = !sameCfg(before, c);
		if (changed)
		{
			{ std::lock_guard<std::mutex> lk(m); cfg = c; }
			if (c.fullscreen != before.fullscreen) fsPending.store(true);
			// Apply only when nothing has changed for ~0.3 s (dragging a slider would otherwise re-apply every frame -> stutter)
			applyAt.store(ImGui::GetTime() + 0.3);
			save();
		}
		if (closeReq) { open.store(false); latch.store(true); }
		if (quitReq) { save(); ps2xhot::aufnahmeBeenden(); std::fflush(nullptr); std::_Exit(0); }
	}

	void toggle()
	{
		const bool o = !open.load();
		open.store(o);
		if (!o) latch.store(true);
	}
}

// ---- ps2x: input directly in the game window (keyboard) + gamepad through SDL3 (also without focus) ----------------
#include <SDL3/SDL.h>
#ifndef _WIN32
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <dlfcn.h>
// Raw mouse input on X11 / XWayland (XInput2 raw motion): unaccelerated counts straight from the device, like raw input
// on Windows. libXi is loaded at run time, so the bridge needs neither its headers nor a link dependency. Without it
// (or with PS2XGS_RAWMOUSE=0) the pointer position is used as before.
namespace ps2xraw
{
	struct XIEventMaskT { int deviceid; int mask_len; unsigned char* mask; };
	struct XIValuatorStateT { int mask_len; unsigned char* mask; double* values; };
	struct XIRawEventT { int type; unsigned long serial; Bool send_event; Display* display; int extension; int evtype; Time time;
		int deviceid; int sourceid; int detail; int flags; XIValuatorStateT valuators; double* raw_values; };
	static int opcode = -1; static bool aktiv = false; static double restX = 0.0, restY = 0.0;
	static bool init(Display* d)
	{
		if (const char* e = std::getenv("PS2XGS_RAWMOUSE"); e && *e == '0') return false;
		int ev = 0, err = 0;
		if (!XQueryExtension(d, "XInputExtension", &opcode, &ev, &err)) return false;
		void* lib = dlopen("libXi.so.6", RTLD_NOW | RTLD_GLOBAL);
		if (!lib) return false;
		auto query = reinterpret_cast<int (*)(Display*, int*, int*)>(dlsym(lib, "XIQueryVersion"));
		auto select = reinterpret_cast<int (*)(Display*, Window, XIEventMaskT*, int)>(dlsym(lib, "XISelectEvents"));
		if (!query || !select) return false;
		int major = 2, minor = 0;
		if (query(d, &major, &minor) != 0) return false;
		unsigned char mask[4] = {0, 0, 0, 0};
		mask[17 >> 3] |= static_cast<unsigned char>(1u << (17 & 7));   // XI_RawMotion
		XIEventMaskT m{1 /* XIAllMasterDevices */, static_cast<int>(sizeof(mask)), mask};
		if (select(d, DefaultRootWindow(d), &m, 1) != 0) return false;
		XFlush(d);
		Console.WriteLn("[ps2x] mouse: raw input (XInput2)");
		return true;
	}
}
#endif
namespace ps2xin
{
	// PS2 button bits (1 = pressed), as in libpad: Select L3 R3 Start Up Right Down Left L2 R2 L1 R1 Tri Circle Cross Square
	enum : u16 { SELECT = 0x0001, L3 = 0x0002, R3 = 0x0004, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080,
		L2 = 0x0100, R2 = 0x0200, L1 = 0x0400, R1 = 0x0800, TRI = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000 };
	std::atomic<u32> keyBtns{0};      // keyboard -> buttons
	std::atomic<u32> keyStick{0};     // Tastatur -> Sticks: Bits 0-3 links (oben unten links rechts), 4-7 rechts
	std::atomic<u32> padBtns{0};
	std::atomic<u32> padAxes{0x80808080u}; // lx | ly<<8 | rx<<16 | ry<<24
	std::atomic<bool> padOn{false};
	std::atomic<u32> rumble{0}; // lo = kleiner Motor, hi = grosser Motor (0..65535)
	// Mouse aiming: relative movement since the game's last query, only while the mouse is captured
	std::atomic<int> mausDX{0}, mausDY{0};
	std::atomic<bool> mausGefangen{false};
	std::atomic<bool> mausNeu{false};  // first motion event after capturing: only remember the start position
	std::atomic<u32> mausTasten{0};   // (alt, ungenutzt)
	std::atomic<int> mausRad{0};      // (alt, ungenutzt)
	std::atomic<u32> aktivA{0};       // pressed actions (bit = action number), keyboard + mouse
	std::atomic<int> impulse[32] = {}; // Mausrad: anstehende kurze Druecke je Aktion
	// action pressed/released through keyboard/mouse -> recompute keyBtns/keyStick
	void neuBerechnen()
	{
		const u32 a = aktivA.load();
		u32 b = 0, st = 0;
		for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
			if (a & (1u << i))
			{
				const auto& ak = ps2xmenu::kAktionen[i];
				if (ak.wirkung == ps2xmenu::W_TASTE) b |= ak.wert; else if (ak.wirkung == ps2xmenu::W_STICK) st |= ak.wert;
			}
		keyBtns.store(b); keyStick.store(st);
	}
	// code (KeySym or kMaus+button) pressed/released; wheel codes create an impulse
	void code(int c, bool down)
	{
		if (c == 0) return;
		int bel[32][2];
		{ std::lock_guard<std::mutex> lk(ps2xmenu::m); std::memcpy(bel, ps2xmenu::cfg.bel, sizeof(bel)); }
		const bool rad = c == ps2xmenu::kMaus + 4 || c == ps2xmenu::kMaus + 5;
		for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
			if (bel[i][0] == c || bel[i][1] == c)
			{
				if (rad) { if (down) impulse[i].fetch_add(1); }
				else if (down) aktivA.fetch_or(1u << i); else aktivA.fetch_and(~(1u << i));
			}
		neuBerechnen();
	}
	bool zoomGehalten()
	{
		const u32 a = aktivA.load();
		for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
			if ((a & (1u << i)) && ps2xmenu::kAktionen[i].wirkung == ps2xmenu::W_ZOOM) return true;
		return false;
	}
#ifndef _WIN32
	Atom wmDelete = None;
#endif
	bool fullscreen = false;
	SDL_Gamepad* gp = nullptr;
	bool sdlOk = false;
	// ps2x: Spieler 2-4 (weitere Gamepads, Splitscreen). Spieler 1 bleibt gp/padBtns/padAxes (Tastatur + erstes Pad).
	SDL_Gamepad* gpx[3] = {nullptr, nullptr, nullptr};
	std::atomic<u32> xBtns[3] = {0, 0, 0};
	std::atomic<u32> xAxes[3] = {0x80808080u, 0x80808080u, 0x80808080u};
	std::atomic<bool> xOn[3] = {false, false, false};
	// Last input device used: Steam (Deck, desktop mode) sends keyboard/mouse events in addition to the gamepad
	// (D-pad = arrow keys, B = Esc ...). While the gamepad is active, the keyboard does not count as a pad.
	std::atomic<long long> padAktivMs{-100000};
	long long jetztMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	bool padAktiv() { return jetztMs() - padAktivMs.load() < 1500; }
	// Last real mouse/keyboard input. If the gamepad is used afterwards, the bridge releases the mouse
	// (Steam Deck: a touchscreen tap or the trackpad would otherwise capture the mouse and the game would stay in mouse aiming).
	std::atomic<long long> mausAktivMs{-100000};
	bool padSeitMaus() { return padAktivMs.load() > mausAktivMs.load() + 250; }

	void key(KeySym ks, bool down)
	{
		int c = static_cast<int>(ks);
		if (c >= 'A' && c <= 'Z') c += 32; // Gross/klein gleich
		code(c, down);
	}

#ifndef _WIN32
	void toggleFullscreen(Display* d, Window w)
	{
		fullscreen = !fullscreen;
		XEvent e{};
		e.xclient.type = ClientMessage;
		e.xclient.window = w;
		e.xclient.message_type = XInternAtom(d, "_NET_WM_STATE", False);
		e.xclient.format = 32;
		e.xclient.data.l[0] = fullscreen ? 1 : 0;
		e.xclient.data.l[1] = static_cast<long>(XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False));
		e.xclient.data.l[2] = 0;
		e.xclient.data.l[3] = 1;
		XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XFlush(d);
	}

#else
	WINDOWPLACEMENT wpPrev{sizeof(WINDOWPLACEMENT)};
	LONG stylePrev = 0;
	void toggleFullscreen(Display*, Window w)
	{
		fullscreen = !fullscreen;
		if (!w)
			return;
		if (fullscreen)
		{
			stylePrev = GetWindowLongW(w, GWL_STYLE);
			GetWindowPlacement(w, &wpPrev);
			MONITORINFO mi{sizeof(mi)};
			GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
			SetWindowLongW(w, GWL_STYLE, stylePrev & ~WS_OVERLAPPEDWINDOW);
			SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
				mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
		else
		{
			SetWindowLongW(w, GWL_STYLE, stylePrev ? stylePrev : WS_OVERLAPPEDWINDOW);
			SetWindowPlacement(w, &wpPrev);
			SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
	}
#endif

	void pollExtraPads();
	void pollPad()
	{
		if (!sdlOk)
		{
			static bool tried = false;
			if (tried)
				return;
			tried = true;
			SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
			sdlOk = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
			if (!sdlOk)
			{
				Console.Error(fmt::format("[ps2x] gamepad support (SDL) not available: {}", SDL_GetError()));
				return;
			}
			int n0 = 0;
			if (SDL_JoystickID* ids0 = SDL_GetJoysticks(&n0)) SDL_free(ids0);
			Console.WriteLn(fmt::format("[ps2x] SDL ready, {} controller(s) found", n0));
		}
		SDL_PumpEvents();
		SDL_UpdateGamepads();
		if (gp && !SDL_GamepadConnected(gp)) { SDL_CloseGamepad(gp); gp = nullptr; }
		if (!gp)
		{
			int n = 0;
			if (SDL_JoystickID* ids = SDL_GetGamepads(&n))
			{
				if (n > 0)
				{
					gp = SDL_OpenGamepad(ids[0]);
					if (gp) Console.WriteLn(fmt::format("[ps2x] Gamepad: {}", SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?"));
				}
				SDL_free(ids);
			}
		}
		if (!gp) { padOn.store(false); padBtns.store(0); padAxes.store(0x80808080u); return; }
		auto btn = [](SDL_GamepadButton b) { return SDL_GetGamepadButton(gp, b); };
		u32 m = 0;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) m |= UP;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) m |= DOWN;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) m |= LEFT;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= RIGHT;
		if (btn(SDL_GAMEPAD_BUTTON_SOUTH)) m |= CROSS;
		if (btn(SDL_GAMEPAD_BUTTON_EAST)) m |= CIRCLE;
		if (btn(SDL_GAMEPAD_BUTTON_WEST)) m |= SQUARE;
		if (btn(SDL_GAMEPAD_BUTTON_NORTH)) m |= TRI;
		if (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) m |= L1;
		if (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) m |= R1;
		if (btn(SDL_GAMEPAD_BUTTON_START)) m |= START;
		if (btn(SDL_GAMEPAD_BUTTON_BACK)) m |= SELECT;
		if (btn(SDL_GAMEPAD_BUTTON_LEFT_STICK)) m |= L3;
		if (btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) m |= R3;
		if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) m |= L2;
		if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) m |= R2;
		auto ax = [](SDL_GamepadAxis a) -> u32 { const int v = SDL_GetGamepadAxis(gp, a); return static_cast<u32>((v + 32768) >> 8) & 0xFFu; };
		{
			// Menu: Guide button or Select+Start; in the menu the pad drives the navigation
			static bool s_prevToggle = false;
			const bool tog = btn(SDL_GAMEPAD_BUTTON_GUIDE) || (btn(SDL_GAMEPAD_BUTTON_BACK) && btn(SDL_GAMEPAD_BUTTON_START));
			if (tog && !s_prevToggle)
				ps2xmenu::toggle();
			s_prevToggle = tog;
			if (ps2xmenu::open.load())
			{
				const int lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
				u32 nb = 0;
				if ((m & UP) || ly < -20000) nb |= 1;
				if ((m & DOWN) || ly > 20000) nb |= 2;
				if ((m & LEFT) || lx < -20000) nb |= 4;
				if ((m & RIGHT) || lx > 20000) nb |= 8;
				if (m & CROSS) nb |= 16;
				if (m & CIRCLE) nb |= 32;
				static u32 s_prevPadNav = 0;
				const u32 chg = nb ^ s_prevPadNav;
				if (chg) { ps2xmenu::nav.fetch_or(nb & chg); ps2xmenu::nav.fetch_and(~(s_prevPadNav & chg & ~nb)); }
				s_prevPadNav = nb;
				m = 0;
			}
			// Vibration (from the game through ps2xgs_rumble)
			static u32 s_lastRumble = 0;
			const u32 r = rumble.load();
			bool on; int pct;
			{ std::lock_guard<std::mutex> lk(ps2xmenu::m); on = ps2xmenu::cfg.rumble; pct = ps2xmenu::cfg.rumblePct; }
			const u32 eff = (on && !ps2xmenu::open.load()) ? r : 0u;
			if (eff != s_lastRumble || eff)
			{
				const u16 lo = static_cast<u16>(std::min(65535u, ((eff & 0xFFFFu) * static_cast<u32>(pct)) / 100u));
				const u16 hi = static_cast<u16>(std::min(65535u, ((eff >> 16) * static_cast<u32>(pct)) / 100u));
				SDL_RumbleGamepad(gp, lo, hi, 100);
				s_lastRumble = eff;
			}
		}
		{
			const auto weg = [](SDL_GamepadAxis a) { const int v = SDL_GetGamepadAxis(gp, a); return v > 9000 || v < -9000; };
			if (m || weg(SDL_GAMEPAD_AXIS_LEFTX) || weg(SDL_GAMEPAD_AXIS_LEFTY) || weg(SDL_GAMEPAD_AXIS_RIGHTX) || weg(SDL_GAMEPAD_AXIS_RIGHTY) ||
			    btn(SDL_GAMEPAD_BUTTON_GUIDE) || SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000 || SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000)
				padAktivMs.store(jetztMs());
		}
		padBtns.store(m);
		padAxes.store(ax(SDL_GAMEPAD_AXIS_LEFTX) | (ax(SDL_GAMEPAD_AXIS_LEFTY) << 8) | (ax(SDL_GAMEPAD_AXIS_RIGHTX) << 16) | (ax(SDL_GAMEPAD_AXIS_RIGHTY) << 24));
		padOn.store(true);
		pollExtraPads();
	}

	// ps2x: players 2-4 = the other connected gamepads in SDL order (without player 1's)
	void pollExtraPads()
	{
		const SDL_JoystickID first = gp ? SDL_GetGamepadID(gp) : 0;
		for (int i = 0; i < 3; ++i)
			if (gpx[i] && !SDL_GamepadConnected(gpx[i])) { SDL_CloseGamepad(gpx[i]); gpx[i] = nullptr; }
		int n = 0;
		if (SDL_JoystickID* ids = SDL_GetGamepads(&n))
		{
			for (int k = 0; k < n; ++k)
			{
				if (ids[k] == first) continue;
				bool known = false;
				for (int i = 0; i < 3; ++i) if (gpx[i] && SDL_GetGamepadID(gpx[i]) == ids[k]) known = true;
				if (known) continue;
				for (int i = 0; i < 3; ++i)
					if (!gpx[i]) { gpx[i] = SDL_OpenGamepad(ids[k]); if (gpx[i]) Console.WriteLn(fmt::format("[ps2x] Gamepad Spieler {}: {}", i + 2, SDL_GetGamepadName(gpx[i]) ? SDL_GetGamepadName(gpx[i]) : "?")); break; }
			}
			SDL_free(ids);
		}
		for (int i = 0; i < 3; ++i)
		{
			SDL_Gamepad* g = gpx[i];
			if (!g) { xOn[i].store(false); xBtns[i].store(0); xAxes[i].store(0x80808080u); continue; }
			auto b = [g](SDL_GamepadButton x) { return SDL_GetGamepadButton(g, x); };
			u32 m = 0;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) m |= UP;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) m |= DOWN;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) m |= LEFT;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= RIGHT;
			if (b(SDL_GAMEPAD_BUTTON_SOUTH)) m |= CROSS;
			if (b(SDL_GAMEPAD_BUTTON_EAST)) m |= CIRCLE;
			if (b(SDL_GAMEPAD_BUTTON_WEST)) m |= SQUARE;
			if (b(SDL_GAMEPAD_BUTTON_NORTH)) m |= TRI;
			if (b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) m |= L1;
			if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) m |= R1;
			if (b(SDL_GAMEPAD_BUTTON_START)) m |= START;
			if (b(SDL_GAMEPAD_BUTTON_BACK)) m |= SELECT;
			if (b(SDL_GAMEPAD_BUTTON_LEFT_STICK)) m |= L3;
			if (b(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) m |= R3;
			if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) m |= L2;
			if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) m |= R2;
			auto a = [g](SDL_GamepadAxis x) -> u32 { const int v = SDL_GetGamepadAxis(g, x); return static_cast<u32>((v + 32768) >> 8) & 0xFFu; };
			if (ps2xmenu::open.load()) m = 0;
			xBtns[i].store(m);
			xAxes[i].store(a(SDL_GAMEPAD_AXIS_LEFTX) | (a(SDL_GAMEPAD_AXIS_LEFTY) << 8) | (a(SDL_GAMEPAD_AXIS_RIGHTX) << 16) | (a(SDL_GAMEPAD_AXIS_RIGHTY) << 24));
			xOn[i].store(true);
		}
	}
}

namespace ps2xmenu
{
	// CPU thread (live playback): apply settings; redraw while the menu is open (game paused)
	void cpuHook()
	{
		ps2xgsq::drain();
		if (applyPending.exchange(false))
		{
			toSettings(s_settings_interface);
			const auto t0 = std::chrono::steady_clock::now();
			VMManager::ApplySettings();
			Console.WriteLn(fmt::format("[ps2xmenu] Einstellungen angewendet ({:.0f} ms)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()));
		}
		if (fsPending.exchange(false) && s_display && s_window != None)
		{
			bool want;
			{ std::lock_guard<std::mutex> lk(m); want = cfg.fullscreen; }
			if (want != ps2xin::fullscreen)
				ps2xin::toggleFullscreen(s_display, s_window);
		}
		{
			// Sound: track the volume, pause in the menu
			static int s_vol = -1;
			static bool s_paused = false;
			int vol;
			{ std::lock_guard<std::mutex> lk(m); vol = cfg.volume; }
			if (vol != s_vol && VMManager::HasValidVM()) { SPU2::SetOutputVolume(static_cast<u32>(vol)); s_vol = vol; }
			const bool o = open.load();
			if (o != s_paused && VMManager::HasValidVM()) { SPU2::SetOutputPaused(o); s_paused = o; }
		}
		if (open.load())
		{
			static auto s_last = std::chrono::steady_clock::now();
			const auto now = std::chrono::steady_clock::now();
			if (now - s_last > std::chrono::milliseconds(16))
			{
				s_last = now;
				MTGS::PresentCurrentFrame();
			}
		}
	}
}

// Keyboard in the game window (X11 and Win32): hotkeys, menu control, otherwise PS2 buttons
static void ps2xKey(KeySym ks, bool down, bool alt)
{
	if (ks == XK_Tab) ps2xfeed::tab.store(down);
	if (down && ps2xin::mausGefangen.load()) ps2xin::mausAktivMs.store(ps2xin::jetztMs());
	if (ps2xmenu::lernAktion.load() >= 0)
	{
		if (down)
		{
			int c = static_cast<int>(ks);
			if (c >= 'A' && c <= 'Z') c += 32;
			ps2xmenu::lernCode.store(ks == XK_Escape ? -1 : c);
		}
		return;
	}
	// Mouse captured = keyboard/mouse is the active device, even with a gamepad lying next to it (stick drift counts as active)
	if (down && ps2xin::padAktiv() && !ps2xin::mausGefangen.load() && !(ks == XK_F7 || ks == XK_F8 || ks == XK_F9 || ks == XK_F10 || ks == XK_F11 || ks == XK_F12 || ks == XK_Print))
	{
		static int s_n = 0;
		if (s_n++ < 3) Console.WriteLn("[ps2x] keyboard event ignored (gamepad active, e.g. Steam desktop layout)");
		return;
	}
		if (down && (ks == XK_F11 || (ks == XK_Return && alt)))
		{
			ps2xin::toggleFullscreen(s_display, s_window);
			std::lock_guard<std::mutex> lk(ps2xmenu::m);
			ps2xmenu::cfg.fullscreen = ps2xin::fullscreen;
		}
		else if (down && (ks == XK_Escape || ks == XK_F1))
			ps2xmenu::toggle();
		else if (down && ks == XK_F9)
		{
			bool on;
			{ std::lock_guard<std::mutex> lk(ps2xmenu::m); on = ps2xmenu::cfg.showFps = !ps2xmenu::cfg.showFps; }
			ps2xmenu::applyPending.store(true);
			ps2xmenu::save();
			ps2xhot::toast(on ? "FPS: on" : "FPS: off");
		}
		else if (down && ks == XK_F10)
		{
			const bool on = !ps2xhot::turbo.load();
			ps2xhot::turbo.store(on);
			ps2xhot::toast(on ? "Turbo: on (F10)" : "Turbo: off");
		}
		else if (down && ks == XK_F7)
		{
			const bool on = !ps2xhot::freecam.load();
			ps2xhot::freecam.store(on);
			ps2xhot::toast(on ? "Free camera: on (WASD / Space / Ctrl, Shift = fast, F7 = off)" : "Free camera: off");
		}
		else if (down && ks == XK_F8)
		{
			if (ps2xhot::aufnahme.load()) ps2xhot::aufnahmeStopp();
			else
			{
				char tb[32]; const std::time_t now = std::time(nullptr);
				std::strftime(tb, sizeof(tb), "%Y%m%d_%H%M%S", std::localtime(&now));
				ps2xhot::aufnahmeStart(ps2xhot::shotDir() + "/ts2_" + tb + ".mp4");
			}
		}
		else if (down && (ks == XK_F12 || ks == XK_Print))
		{
			char tb[32]; const std::time_t now = std::time(nullptr);
			std::strftime(tb, sizeof(tb), "%Y%m%d_%H%M%S", std::localtime(&now));
			const std::string p = ps2xhot::shotDir() + "/ts2_" + tb + ".png";
			gsCall([p]() { ps2xShotAspect(); GSQueueSnapshot(p); });
			ps2xhot::toast("Screenshot: " + p);
		}
		else if (ps2xmenu::open.load())
		{
			u32 bit = 0;
			switch (ks)
			{
				case XK_Up: case XK_w: bit = 1; break;
				case XK_Down: case XK_s: bit = 2; break;
				case XK_Left: case XK_a: bit = 4; break;
				case XK_Right: case XK_d: bit = 8; break;
				case XK_Return: case XK_KP_Enter: case XK_space: case XK_x: bit = 16; break;
				case XK_BackSpace: case XK_c: bit = 32; break;
				default: break;
			}
			if (bit) { if (down) ps2xmenu::nav.fetch_or(bit); else ps2xmenu::nav.fetch_and(~bit); }
			if (!down) ps2xin::key(ks, false); // do not leave game keys hanging that were released inside the menu
		}
		else
			ps2xin::key(ks, down);
}

// Mouse button (1 left, 2 middle, 3 right, 4/5 wheel, 8/9 side): capture in learn mode, as a bound action in capture mode
static bool ps2xMausLernen(int knopf, bool down)
{
	if (ps2xmenu::lernAktion.load() < 0) return false;
	if (down && ImGui::GetCurrentContext() && ImGui::GetTime() >= ps2xmenu::lernAb.load())
		ps2xmenu::lernCode.store(ps2xmenu::kMaus + knopf);
	return true;
}
static void ps2xMausTaste(int knopf, bool down)
{
	if (down) ps2xin::mausAktivMs.store(ps2xin::jetztMs());
	ps2xin::code(ps2xmenu::kMaus + knopf, down);
}
static bool ps2xMausWill() // may the mouse be/stay captured?
{
	bool an; { std::lock_guard<std::mutex> lk(ps2xmenu::m); an = ps2xmenu::cfg.maus; }
	return an && !ps2xmenu::open.load() && !(ps2xin::mausGefangen.load() && ps2xin::padSeitMaus());
}

#ifdef _WIN32
static std::thread s_win_thread;
static std::mutex s_win_mtx;
static std::condition_variable s_win_cv;
static bool s_win_ready = false;
static constexpr UINT WM_PS2X_DESTROY = WM_APP + 0x101;

static KeySym ps2xVkToKeySym(WPARAM vk, LPARAM lp)
{
	if (vk >= 'A' && vk <= 'Z')
		return static_cast<KeySym>('a' + (vk - 'A'));
	switch (vk)
	{
		case VK_UP: return XK_Up;
		case VK_DOWN: return XK_Down;
		case VK_LEFT: return XK_Left;
		case VK_RIGHT: return XK_Right;
		case VK_RETURN: return (lp & (1 << 24)) ? XK_KP_Enter : XK_Return;
		case VK_SHIFT: return (MapVirtualKeyW(static_cast<UINT>((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? XK_Shift_R : XK_Shift_L;
		case VK_TAB: return XK_Tab;
		case VK_ESCAPE: return XK_Escape;
		case VK_BACK: return XK_BackSpace;
		case VK_SPACE: return XK_space;
		case VK_F1: return XK_F1;
		case VK_F7: return XK_F7;
		case VK_F8: return XK_F8;
		case VK_F9: return XK_F9;
		case VK_F10: return XK_F10;
		case VK_F11: return XK_F11;
		case VK_F12: return XK_F12;
		case VK_SNAPSHOT: return XK_Print;
		case VK_CONTROL: return (lp & (1 << 24)) ? 0xffe4 : 0xffe3;
		case VK_MENU: return 0xffe9;
		case VK_CAPITAL: return 0xffe5;
		case VK_DELETE: return 0xffff;
		case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6: return static_cast<KeySym>(0xffbe + (vk - VK_F1));
		case VK_HOME: return 0xff50;
		case VK_PRIOR: return 0xff55;
		case VK_NEXT: return 0xff56;
		case VK_END: return 0xff57;
		case VK_INSERT: return 0xff63;
		case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3: case VK_NUMPAD4:
		case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7: case VK_NUMPAD8: case VK_NUMPAD9:
			return static_cast<KeySym>(0xffb0 + (vk - VK_NUMPAD0)); // XK_KP_0..9
		case VK_ADD: return 0xffab;
		case VK_SUBTRACT: return 0xffad;
		case VK_MULTIPLY: return 0xffaa;
		case VK_DIVIDE: return 0xffaf;
		case VK_DECIMAL: return 0xffae;
		default:
			if (vk >= '0' && vk <= '9') return static_cast<KeySym>(vk);
			// Punctuation, umlauts etc.: the character of the current keyboard layout; in the Latin-1 range the
			// KeySym equals the character code (like XLookupKeysym on X11), above that a Unicode KeySym
			if (const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu; ch >= 0x20)
			{
				if (ch >= 'A' && ch <= 'Z') return static_cast<KeySym>('a' + (ch - 'A'));
				if (ch < 0x100) return static_cast<KeySym>(ch);
				return static_cast<KeySym>(0x01000000u + ch);
			}
			return 0;
	}
}

static LRESULT CALLBACK ps2xWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		{
			if (lp & (1 << 30)) // ignore auto-repeat while held (like XkbSetDetectableAutoRepeat)
				return 0;
			if (msg == WM_SYSKEYDOWN && wp == VK_F4)
				break;
			if (const KeySym ks = ps2xVkToKeySym(wp, lp))
				ps2xKey(ks, true, msg == WM_SYSKEYDOWN && (lp & (1 << 29)));
			return 0;
		}
		case WM_KEYUP:
		case WM_SYSKEYUP:
			if (const KeySym ks = ps2xVkToKeySym(wp, lp))
				ps2xKey(ks, false, false);
			return 0;
		case WM_SYSCHAR:
			return 0; // no beep on Alt+Enter
		case WM_SYSCOMMAND:
			if ((wp & 0xFFF0) == SC_KEYMENU)
				return 0;
			break;
		case WM_SIZE:
			s_wi.surface_width = LOWORD(lp);
			s_wi.surface_height = HIWORD(lp);
			// ps2x: resize the swap chain - D3D11/Vulkan do not follow the window by themselves (fullscreen left it at the
			// start size, the menu then took the mouse at the wrong place). Only once the CPU thread runs (MTGS open).
			if (LOWORD(lp) > 0 && HIWORD(lp) > 0 && ps2xgsq::aktiv.load())
			{
				ps2xmenu::fensterGroesse();
				const u32 w = LOWORD(lp), h = HIWORD(lp);
				gsCall([w, h]() { if (g_gs_device) GSResizeDisplayWindow(w, h, 1.0f); });
			}
			[[fallthrough]];
		case WM_MOVE:
			if (ps2xin::mausGefangen.load()) // Fenster verschoben / Vollbild gewechselt: Zeigerbegrenzung nachziehen
			{
				RECT cr; GetClientRect(hwnd, &cr); MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
				ClipCursor(&cr);
			}
			return 0;
		case WM_INPUT:
		{
			if (!ps2xin::mausGefangen.load()) break;
			RAWINPUT ri{}; UINT sz = sizeof(ri);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
				ri.header.dwType == RIM_TYPEMOUSE)
			{
				if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
				{
					ps2xin::mausDX.fetch_add(ri.data.mouse.lLastX);
					ps2xin::mausDY.fetch_add(ri.data.mouse.lLastY);
				}
				else
				{
					// Absolute movement (VM, remote desktop, tablet mode): 0..65535 across the (virtual) screen,
					// build the difference to the last position from it
					static LONG letztX = -1, letztY = -1;
					const bool virt = ri.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP;
					const LONG bw = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
					const LONG bh = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
					const LONG x = static_cast<LONG>(ri.data.mouse.lLastX * static_cast<long long>(bw) / 65535);
					const LONG y = static_cast<LONG>(ri.data.mouse.lLastY * static_cast<long long>(bh) / 65535);
					if (letztX >= 0 && ps2xin::mausGefangen.load())
					{
						ps2xin::mausDX.fetch_add(x - letztX);
						ps2xin::mausDY.fetch_add(y - letztY);
					}
					letztX = x; letztY = y;
				}
			}
			break;
		}
		case WM_MOUSEMOVE:
			ps2xmenu::mouseX.store(GET_X_LPARAM(lp));
			ps2xmenu::mouseY.store(GET_Y_LPARAM(lp));
			return 0;
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		{
			const u32 bit = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1u : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2u : 4u;
			const bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
			const int knopf = bit == 1u ? 1 : bit == 2u ? 3 : 2;
			if (ps2xMausLernen(knopf, down)) return 0;
			if (down) { ps2xmenu::mouseBtn.fetch_or(bit); SetFocus(hwnd); } else ps2xmenu::mouseBtn.fetch_and(~bit);
			if (ps2xin::mausGefangen.load())
				ps2xMausTaste(knopf, down);
			else if (down && bit == 1u && ps2xMausWill())
			{
				// Fangen: Zeiger aufs Fenster beschraenken, Rohdaten (relative Bewegung) einschalten
				RAWINPUTDEVICE rid{0x01, 0x02, 0, hwnd};
				RegisterRawInputDevices(&rid, 1, sizeof(rid));
				RECT cr; GetClientRect(hwnd, &cr); MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
				ClipCursor(&cr);
				ps2xin::mausDX.store(0); ps2xin::mausDY.store(0);
				ps2xin::mausAktivMs.store(ps2xin::jetztMs()); ps2xin::mausGefangen.store(true);
			}
			return 0;
		}
		case WM_MOUSEWHEEL:
		{
			const int knopf = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 4 : 5;
			if (ps2xMausLernen(knopf, true)) return 0;
			if (ps2xin::mausGefangen.load()) ps2xMausTaste(knopf, true);
			else ps2xmenu::wheel.fetch_add(knopf == 4 ? 1 : -1);
			return 0;
		}
		case WM_XBUTTONDOWN: case WM_XBUTTONUP:
		{
			const int knopf = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 8 : 9;
			const bool down = msg == WM_XBUTTONDOWN;
			if (ps2xMausLernen(knopf, down)) return TRUE;
			if (ps2xin::mausGefangen.load()) ps2xMausTaste(knopf, down);
			return TRUE;
		}
		case WM_KILLFOCUS:
			if (ps2xin::mausGefangen.exchange(false)) ClipCursor(nullptr);
			ps2xin::aktivA.store(0);
			ps2xin::keyBtns.store(0);
			ps2xin::keyStick.store(0);
			return 0;
		case WM_TIMER:
			ps2xin::pollPad(); // Gamepad ~250 Hz
			if (ps2xin::mausGefangen.load() && !ps2xMausWill()) // menu opened / mouse aiming off -> release
			{
				ps2xin::mausGefangen.store(false);
				ClipCursor(nullptr);
			}
			return 0;
		case WM_SETCURSOR:
			if (LOWORD(lp) == HTCLIENT && !ps2xmenu::open.load()) { SetCursor(nullptr); return TRUE; } // mouse pointer only in the menu
			break;
		case WM_CLOSE:
			ps2xhot::aufnahmeBeenden();
			std::fflush(nullptr);
			std::_Exit(0); // the memory card is written immediately when saving
		case WM_PS2X_DESTROY:
			DestroyWindow(hwnd);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window, "Tried to create window when there already was one!");
	if (const char* fs = std::getenv("PS2XGS_FULLSCREEN"); fs && *fs == '1')
		ps2xin::fullscreen = true; // set after showing
	// Win32: messages must run on the thread that created the window -> own UI thread
	s_win_thread = std::thread([] {
		const wchar_t* cls = L"ps2xgs-window";
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = ps2xWndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		wc.lpszClassName = cls;
		wc.hIcon = LoadIcon(GetModuleHandleW(nullptr), MAKEINTRESOURCE(1));
		RegisterClassExW(&wc);
		const char* t = ps2xmenu::envOr("PS2X_WINDOW_TITLE", "TimeSplitters 2 (TS2 Native Recomp)");
		wchar_t titel[256] = {};
		MultiByteToWideChar(CP_UTF8, 0, t, -1, titel, 255);
		RECT r{0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
		HWND h = CreateWindowExW(0, cls, titel, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		if (h)
		{
			ShowWindow(h, SW_SHOW);
			UpdateWindow(h);
			SetForegroundWindow(h);
			RECT cr{};
			GetClientRect(h, &cr);
			s_wi.type = WindowInfo::Type::Win32;
			s_wi.window_handle = h;
			s_wi.surface_width = static_cast<u32>(cr.right - cr.left);
			s_wi.surface_height = static_cast<u32>(cr.bottom - cr.top);
			s_wi.surface_scale = 1.0f;
			SetTimer(h, 1, 4, nullptr);
		}
		{
			std::lock_guard<std::mutex> lk(s_win_mtx);
			s_window = h;
			s_display = h;
			s_win_ready = true;
		}
		s_win_cv.notify_all();
		if (!h)
			return;
		MSG m;
		while (GetMessageW(&m, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	});
	{
		std::unique_lock<std::mutex> lk(s_win_mtx);
		s_win_cv.wait(lk, [] { return s_win_ready; });
	}
	if (!s_window)
	{
		Console.Error("Failed to create Win32 window");
		if (s_win_thread.joinable()) s_win_thread.join();
		return false;
	}
	if (ps2xin::fullscreen) { ps2xin::fullscreen = false; ps2xin::toggleFullscreen(s_display, s_window); }
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_window)
	{
		PostMessageW(s_window, WM_PS2X_DESTROY, 0, 0);
		if (s_win_thread.joinable()) s_win_thread.join();
		s_window = None;
		s_display = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	// The messages run in the window's UI thread; without a window (test run) only poll the gamepads here
	do
	{
		if (s_shutdown_requested.load())
			return;
		if (forever)
		{
			if (!s_window)
				ps2xin::pollPad();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	} while (forever && !s_shutdown_requested.load());
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}
#else
bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_display && s_window == None, "Tried to create window when there already was one!");

	XInitThreads();
	s_display = XOpenDisplay(nullptr);
	if (!s_display)
	{
		Console.Error("Failed to open X11 display");
		return false;
	}

	int screen = DefaultScreen(s_display);
	Window root = RootWindow(s_display, screen);

	s_window = XCreateSimpleWindow(s_display, root, 0, 0, ps2xFensterMass(false), ps2xFensterMass(true), 1,
		BlackPixel(s_display, screen), WhitePixel(s_display, screen));

	if (s_window == None)
	{
		Console.Error("Failed to create X11 window");
		XCloseDisplay(s_display);
		s_display = nullptr;
		return false;
	}

	XStoreName(s_display, s_window, ps2xmenu::envOr("PS2X_WINDOW_TITLE", "Shadow of the Colossus (SotC Recomp)"));
	XSelectInput(s_display, s_window, StructureNotifyMask | KeyPressMask | KeyReleaseMask | FocusChangeMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
	XkbSetDetectableAutoRepeat(s_display, True, nullptr); // held keys without release/press flicker
	ps2xin::wmDelete = XInternAtom(s_display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(s_display, s_window, &ps2xin::wmDelete, 1);
	{
		XClassHint* ch = XAllocClassHint();
		if (ch) { ch->res_name = const_cast<char*>(ps2xmenu::cfgName()); ch->res_class = const_cast<char*>(ps2xmenu::cfgName()); XSetClassHint(s_display, s_window, ch); XFree(ch); }
	}
	if (const char* fs = std::getenv("PS2XGS_FULLSCREEN"); fs && *fs == '1')
		ps2xin::fullscreen = true; // set after showing
	{
		// Without WM_HINTS input=True some window managers (KWin under XWayland) do not give the window keyboard focus
		if (XWMHints* wh = XAllocWMHints())
		{
			wh->flags = InputHint | StateHint;
			wh->input = True;
			wh->initial_state = NormalState;
			XSetWMHints(s_display, s_window, wh);
			XFree(wh);
		}
	}
	XMapRaised(s_display, s_window);

	s_wi.type = WindowInfo::Type::X11;
	s_wi.display_connection = s_display;
	s_wi.window_handle = reinterpret_cast<void*>(s_window);
	s_wi.surface_width = ps2xFensterMass(false);
	s_wi.surface_height = ps2xFensterMass(true);
	s_wi.surface_scale = 1.0f;

	XFlush(s_display);
	PumpPlatformMessages();
	if (ps2xin::fullscreen) { ps2xin::fullscreen = false; ps2xin::toggleFullscreen(s_display, s_window); }
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_display && s_window != None)
	{
		XDestroyWindow(s_display, s_window);
		s_window = None;
	}

	if (s_display)
	{
		XCloseDisplay(s_display);
		s_display = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_display && s_window != None)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	if (!s_display)
		return;

	do
	{
		{ static bool einmal = false; if (!einmal) { einmal = true; ps2xraw::aktiv = ps2xraw::init(s_display); } }
		while (XPending(s_display) > 0)
		{
			XEvent event;
			XNextEvent(s_display, &event);

			switch (event.type)
			{
				case ConfigureNotify:
				{
					const XConfigureEvent& configure = event.xconfigure;
					s_wi.surface_width = static_cast<u32>(configure.width);
					s_wi.surface_height = static_cast<u32>(configure.height);
					ps2xmenu::fensterGroesse();
					break;
				}
				case DestroyNotify:
					std::_Exit(0);
				case MapNotify:
					XSetInputFocus(s_display, s_window, RevertToParent, CurrentTime);
					break;
				case FocusIn:
					if (std::getenv("PS2X_KEYLOG"))
						std::fprintf(stderr, "[ps2x] focus gained\n");
					break;
				case KeyPress:
				case KeyRelease:
				{
					const KeySym ks = XLookupKeysym(&event.xkey, 0);
					const bool down = event.type == KeyPress;
					if (down)
					{
						static int s_keys = 0;
						if (s_keys++ == 0) Console.WriteLn("[ps2x] keyboard active");
						if (std::getenv("PS2X_KEYLOG")) std::fprintf(stderr, "[ps2x] key %lx\n", static_cast<unsigned long>(ks));
					}
					ps2xKey(ks, down, (event.xkey.state & Mod1Mask) != 0);
					break;
				}
				case GenericEvent:
					if (ps2xraw::aktiv && event.xcookie.extension == ps2xraw::opcode && XGetEventData(s_display, &event.xcookie))
					{
						if (event.xcookie.evtype == 17 && ps2xin::mausGefangen.load())
						{
							const auto* re = static_cast<const ps2xraw::XIRawEventT*>(event.xcookie.data);
							const double* v = re->raw_values; double dx = 0.0, dy = 0.0;
							for (int i = 0; i < re->valuators.mask_len * 8 && i < 2; ++i)
								if (re->valuators.mask[i >> 3] & (1u << (i & 7))) { if (i == 0) dx = *v; else dy = *v; ++v; }
							ps2xraw::restX += dx; ps2xraw::restY += dy;
							const int ix = static_cast<int>(ps2xraw::restX), iy = static_cast<int>(ps2xraw::restY);
							ps2xraw::restX -= ix; ps2xraw::restY -= iy;
							if (ix) ps2xin::mausDX.fetch_add(ix);
							if (iy) ps2xin::mausDY.fetch_add(iy);
						}
						XFreeEventData(s_display, &event.xcookie);
					}
					break;
				case MotionNotify:
					ps2xmenu::mouseX.store(event.xmotion.x);
					ps2xmenu::mouseY.store(event.xmotion.y);
					if (ps2xin::mausGefangen.load())
					{
						// Relative movement to the last position. Near the edge move the pointer to the centre; the event caused by
						// that (exactly the centre) does not count. If warping does not work (some XWayland
						// versions), the movement is still right, only limited at the edge.
						static int s_lx = -1, s_ly = -1, s_warp = 0;
						const int w = static_cast<int>(s_wi.surface_width), h = static_cast<int>(s_wi.surface_height);
						const int cx = w / 2, cy = h / 2, x = event.xmotion.x, y = event.xmotion.y;
						if (s_warp > 0 && x == cx && y == cy) { s_warp = 0; s_lx = cx; s_ly = cy; break; }
						if (s_warp > 0 && --s_warp == 0) { /* warp did not arrive */ }
						if (s_lx >= 0 && ps2xin::mausNeu.exchange(false) == false && !ps2xraw::aktiv)
						{
							ps2xin::mausDX.fetch_add(x - s_lx); ps2xin::mausDY.fetch_add(y - s_ly);
						}
						s_lx = x; s_ly = y;
						if (s_warp == 0 && (x < w / 4 || x > w * 3 / 4 || y < h / 4 || y > h * 3 / 4))
						{
							XWarpPointer(s_display, None, s_window, 0, 0, 0, 0, cx, cy);
							s_warp = 8;
						}
					}
					break;
				case ButtonPress:
				case ButtonRelease:
				{
					const unsigned b = event.xbutton.button;
					const bool down = event.type == ButtonPress;
					if (down) XSetInputFocus(s_display, s_window, RevertToParent, CurrentTime);
					if (ps2xMausLernen(static_cast<int>(b), down)) break;
					if (b == 8 || b == 9) { if (ps2xin::mausGefangen.load()) ps2xMausTaste(static_cast<int>(b), down); break; }
					if (b >= 1 && b <= 3)
					{
						const u32 bit = (b == 1) ? 1u : (b == 3) ? 2u : 4u;
						if (down) ps2xmenu::mouseBtn.fetch_or(bit); else ps2xmenu::mouseBtn.fetch_and(~bit);
						if (ps2xin::mausGefangen.load())
							ps2xMausTaste(static_cast<int>(b), down);
						else if (down && b == 1 && ps2xMausWill() &&
							XGrabPointer(s_display, s_window, True, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
								GrabModeAsync, GrabModeAsync, s_window, None, CurrentTime) == GrabSuccess)
						{
							// Capture: pointer confined to the window (under XWayland: pointer lock by the compositor)
							ps2xin::mausNeu.store(true);
							ps2xin::mausDX.store(0); ps2xin::mausDY.store(0);
							ps2xin::mausAktivMs.store(ps2xin::jetztMs()); ps2xin::mausGefangen.store(true);
						}
					}
					else if (down && (b == 4 || b == 5))
					{
						if (ps2xin::mausGefangen.load()) ps2xMausTaste(static_cast<int>(b), true);
						else ps2xmenu::wheel.fetch_add(b == 4 ? 1 : -1);
					}
					break;
				}
				case FocusOut:
					if (std::getenv("PS2X_KEYLOG"))
						std::fprintf(stderr, "[ps2x] focus lost\n");
					if (ps2xin::mausGefangen.exchange(false)) XUngrabPointer(s_display, CurrentTime);
					ps2xin::aktivA.store(0);
					ps2xin::keyBtns.store(0);
					ps2xin::keyStick.store(0);
					break;
				case ClientMessage:
					if (static_cast<Atom>(event.xclient.data.l[0]) == ps2xin::wmDelete)
					{
						Console.WriteLn("[ps2x] window closed -> quitting the game");
						std::fflush(nullptr);
						std::_Exit(0); // the memory card is written immediately when saving
					}
					break;
				default:
					break;
			}
		}

		if (s_shutdown_requested.load())
			return;

		if (forever)
		{
			// Mouse pointer: invisible in the game, visible in the menu (mouse control)
			static Cursor s_blank = None;
			static int s_cursorAus = -1;
			const int aus = ps2xmenu::open.load() ? 0 : 1;
			if (s_window != None && aus != s_cursorAus)
			{
				if (s_blank == None)
				{
					char z = 0;
					Pixmap pm = XCreateBitmapFromData(s_display, s_window, &z, 1, 1);
					XColor c{};
					s_blank = XCreatePixmapCursor(s_display, pm, pm, &c, &c, 0, 0);
					XFreePixmap(s_display, pm);
				}
				if (aus) XDefineCursor(s_display, s_window, s_blank); else XUndefineCursor(s_display, s_window);
				XFlush(s_display);
				s_cursorAus = aus;
			}
			if (ps2xin::mausGefangen.load() && !ps2xMausWill()) // menu opened / mouse aiming off -> release
			{
				ps2xin::mausGefangen.store(false);
				for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i) ps2xin::impulse[i].store(0);
				XUngrabPointer(s_display, CurrentTime); XFlush(s_display);
			}
			static u32 s_tick = 0;
			if ((++s_tick & 3u) == 0u)
				ps2xin::pollPad(); // Gamepad ~250 Hz
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} while (forever && !s_shutdown_requested.load());
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}
#endif // _WIN32
#endif // __APPLE__ / _WIN32 || __linux__


//////////////////////////////////////////////////////////////////////////
// ps2x API (the only exported symbols of the library)
//////////////////////////////////////////////////////////////////////////
// Hooks in the patched PCSX2 (ImGuiOverlays.cpp, GSDumpReplayer.cpp). Declared at file scope: inside the
// extern "C" export functions they would get C linkage, which on Windows (MSVC name mangling) does not match the definition.
extern void (*g_ps2xMenuDraw)();
extern void (*g_ps2xCpuHook)();

#ifdef _WIN32
#define PS2XGS_API extern "C" __declspec(dllexport)
#else
#define PS2XGS_API extern "C" __attribute__((visibility("default")))
#endif

static std::thread s_ps2x_cpu_thread;
static std::thread s_ps2x_pump_thread;
// When the DLL unloads / the process ends the threads are still running: a destroyed, still joinable std::thread calls
// std::terminate (Windows: abort -> 0xC0000409 on exit). This object is destroyed before the threads and detaches them.
static struct Ps2xThreadsLoesen
{
	~Ps2xThreadsLoesen()
	{
		for (std::thread* t : {&s_ps2x_cpu_thread, &s_ps2x_pump_thread})
			if (t->joinable()) t->detach();
#ifdef _WIN32
		if (s_win_thread.joinable()) s_win_thread.detach();
#endif
	}
} s_ps2x_threads_loesen;
static std::atomic<int> s_ps2x_ret{0};
static VMBootParameters s_ps2x_params;

#ifdef _WIN32

#include <crtdbg.h>
// Windows diagnosis: bridge log into a file (PCSX2's Windows console does not write to redirected output)
// and an own crash message for cases the CrashHandler (SEH) does not see: abort()/std::terminate/invalid
// CRT parameters otherwise end as 0xC0000409 without any trace.
namespace ps2xdiag
{
	static std::string s_log;
	static void zeile(const char* text)
	{
		if (!s_log.empty())
			if (std::FILE* f = std::fopen(s_log.c_str(), "ab")) { std::fputs(text, f); std::fputc('\n', f); std::fclose(f); }
		std::fputs(text, stderr); std::fputc('\n', stderr); std::fflush(stderr);
		OutputDebugStringA(text);
	}
	static void stapel(const char* grund)
	{
		char b[512];
		std::snprintf(b, sizeof(b), "[ps2xgs] ABSTURZ: %s (Thread %lu)", grund, GetCurrentThreadId());
		zeile(b);
		void* f[48] = {};
		const USHORT n = CaptureStackBackTrace(1, 48, f, nullptr);
		for (USHORT i = 0; i < n; ++i)
		{
			HMODULE m = nullptr; char name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(f[i]), &m) && m)
				GetModuleFileNameA(m, name, MAX_PATH);
			const char* kurz = std::strrchr(name, '\\');
			std::snprintf(b, sizeof(b), "  #%02u %s+0x%llx", i, kurz ? kurz + 1 : name,
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(f[i]) - reinterpret_cast<uintptr_t>(m)));
			zeile(b);
		}
	}
	static void beiAbort(int) { stapel("abort() (SIGABRT)"); }
	static void beiParameter(const wchar_t* expr, const wchar_t* fn, const wchar_t* file, unsigned line, uintptr_t)
	{
		char b[600];
		std::snprintf(b, sizeof(b), "ungueltiger CRT-Parameter: %ls in %ls (%ls:%u)", expr ? expr : L"?", fn ? fn : L"?", file ? file : L"?", line);
		stapel(b);
	}
	static void beiTerminate() { stapel("std::terminate (uncaught exception)"); std::abort(); }
	static LONG WINAPI beiAusnahme(EXCEPTION_POINTERS* e)
	{
		const DWORD c = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0;
		// only real errors (no debug/C++ exceptions that get caught)
		if (c == 0xC0000005 || c == 0xC0000409 || c == 0xC000001D || c == 0xC00000FD || c == 0xC0000374 || c == 0xC0000094)
		{
			char b[160];
			HMODULE m = nullptr; char name[MAX_PATH] = "?";
			void* adr = e->ExceptionRecord->ExceptionAddress;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(adr), &m) && m)
				GetModuleFileNameA(m, name, MAX_PATH);
			const char* kurz = std::strrchr(name, '\\');
			std::snprintf(b, sizeof(b), "exception 0x%08lX at %s+0x%llx", c, kurz ? kurz + 1 : name,
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(adr) - reinterpret_cast<uintptr_t>(m)));
			stapel(b);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}
	static void installieren()
	{
		if (const char* p = std::getenv("PS2XGS_LOG"); p && *p) s_log = p;
		else if (const char* a = std::getenv("APPDATA"); a && *a)
		{
			const char* cn = std::getenv("PS2XGS_CFGNAME");
			s_log = std::string(a) + "\\" + ((cn && *cn) ? cn : "ts2-recomp");
			CreateDirectoryA(s_log.c_str(), nullptr);
			s_log += "\\bridge.log";
		}
		if (!s_log.empty()) if (std::FILE* f = std::fopen(s_log.c_str(), "wb")) std::fclose(f); // je Start neu
		signal(SIGABRT, beiAbort);
		_set_invalid_parameter_handler(beiParameter);
		std::set_terminate(beiTerminate);
		AddVectoredExceptionHandler(1, beiAusnahme);
		zeile("[ps2xgs] diagnostics on");
	}
}
#define PS2XDIAG(text) ps2xdiag::zeile(text)
#else
#define PS2XDIAG(text) ((void)0)
#endif

#ifdef _WIN32
// Laptops with two graphics chips (Intel + Nvidia/AMD): without a hint Windows hands a program the power-saving chip and
// the first DXGI adapter is the Intel one - the game then ran on it (measured on a GTX 1050 laptop: its helper window
// reported "Intel(R) HD Graphics 630"). Ask DXGI for the high-performance adapter and give its name to the renderer.
// PS2XGS_ADAPTER=<name> picks a card by name, PS2XGS_ADAPTER=default leaves the choice to Windows.
static std::string ps2xStarkeGrafikkarte()
{
	if (const char* e = std::getenv("PS2XGS_ADAPTER"); e && *e)
		return std::strcmp(e, "default") == 0 ? std::string() : std::string(e);
	std::string name;
	IDXGIFactory1* f1 = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f1))) || !f1)
		return name;
	IDXGIFactory6* f6 = nullptr;
	if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6))) && f6)
	{
		for (UINT i = 0;; ++i)
		{
			IDXGIAdapter1* a = nullptr;
			if (f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a)) != S_OK || !a)
				break;
			DXGI_ADAPTER_DESC1 d{};
			const bool ok = SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
			a->Release();
			if (ok) { name = StringUtil::WideStringToUTF8String(d.Description); break; }
		}
		f6->Release();
	}
	f1->Release();
	return name;
}
#endif

PS2XGS_API int ps2xgs_start(const char* live_path, int renderer, float upscale, int widescreen)
{
#ifdef _WIN32
	ps2xdiag::installieren();
	// Prozessende: PCSX2 laesst eigene Threads laufen (z. B. Texturersatz-Lader); deren statische std::thread-Objekte
	// would trigger std::terminate when the DLL unloads (0xC0000409 on exit). This atexit entry of the DLL
	// runs before all statically registered destructors registered earlier and ends the process right away.
	std::atexit([] { std::fflush(nullptr); TerminateProcess(GetCurrentProcess(), 0); });
#endif
	CrashHandler::Install(); // Absturz-Aufrufstapel ins Log (libbacktrace)
	GSRunner::InitializeConsole();
#ifdef _WIN32
	if (!ps2xdiag::s_log.empty()) Log::SetFileOutputLevel(LOGLEVEL_DEBUG, ps2xdiag::s_log + ".pcsx2");
#endif
	PS2XDIAG("[ps2xgs] start: console");
	if (!GSRunner::InitializeConfig())
	{
		Console.Error("ps2xgs: configuration failed");
		PS2XDIAG("[ps2xgs] start: configuration failed");
		return 0;
	}
	PS2XDIAG("[ps2xgs] start: configuration ok");
#ifdef _WIN32
	{
		const std::string karte = ps2xStarkeGrafikkarte();
		if (!karte.empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "Adapter", karte.c_str());
		const std::string z = "[ps2xgs] graphics adapter: " + (karte.empty() ? std::string("(Windows default)") : karte);
		Console.WriteLn(z);
		PS2XDIAG(z.c_str());
	}
#endif
	// PS2XGS_SURFACELESS=1 (Testlaeufe): ohne sichtbares Fenster rendern, Screenshots gehen trotzdem
	const char *surfaceless = std::getenv("PS2XGS_SURFACELESS");
	ps2xmenu::load(upscale);
	{
		std::lock_guard<std::mutex> lk(ps2xmenu::m);
		if (ps2xmenu::cfg.fullscreen)
			ps2x_setenv("PS2XGS_FULLSCREEN", "1", 0);
	}
	PS2XDIAG("[ps2xgs] start: menu settings loaded");
	if (!(surfaceless && *surfaceless && *surfaceless != '0') && !GSRunner::CreatePlatformWindow())
	{
		Console.Error("ps2xgs: creating the window failed");
		PS2XDIAG("[ps2xgs] start: window failed");
		return 0;
	}
	PS2XDIAG("[ps2xgs] start: window ok");
	GSRunner::SettingsOverride();
	// The port paces itself (VBlank 50 Hz) -> no limiter of its own, no VSync wait in the GS (V-Sync optional in the menu)
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", renderer);
	s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
	if (widescreen)
		s_settings_interface.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowGSStats", false);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", false);
	// SotC fixes from PCSX2's GameIndex.yaml (SCES-53326). Without a disc boot the game database does not apply,
	// so fixed here: water colour (blending 4), borders/offset (half-pixel 5), post-processing/DOF (native scaling 3)
	// Only for SotC (other games get the PCSX2 defaults or their own values through PS2XGS_SERIAL)
	if (const char* ser = std::getenv("PS2XGS_SERIAL"); !ser || !*ser || std::string_view(ser) == "SCES-53326")
	{
		s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);
		s_settings_interface.SetIntValue("EmuCore/GS", "accurate_blending_unit", 4);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", 5);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_native_scaling", 3);
	}
	else if (std::string_view(ser) == "SLUS-20314") // TimeSplitters 2 (GameIndex.yaml): "Fixes slight texture misalignment"
	{
		s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", 2);
	}
	// Diagnose: PS2XGS_GSOPT="UserHacks_HalfPixelOffset=0;upscale_multiplier=3" setzt beliebige EmuCore/GS-Werte (Ganzzahl/Kommazahl)
	if (const char* o = std::getenv("PS2XGS_GSOPT"); o && *o)
	{
		std::string_view rest(o);
		while (!rest.empty())
		{
			const size_t sc = rest.find(';');
			const std::string_view kv = rest.substr(0, sc);
			rest = sc == std::string_view::npos ? std::string_view() : rest.substr(sc + 1);
			const size_t eq = kv.find('=');
			if (eq == std::string_view::npos) continue;
			const std::string k(kv.substr(0, eq)), v(kv.substr(eq + 1));
			if (!v.empty() && v[0] == '/') s_settings_interface.SetStringValue("EmuCore/GS", k.c_str(), v.c_str());   // paths (dump directories)
			else if (v.find('.') != std::string::npos) s_settings_interface.SetFloatValue("EmuCore/GS", k.c_str(), std::strtof(v.c_str(), nullptr));
			else s_settings_interface.SetIntValue("EmuCore/GS", k.c_str(), std::atoi(v.c_str()));
			if (k.rfind("UserHacks_", 0) == 0) s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);
			Console.WriteLn(fmt::format("[ps2xgs] GS-Option {} = {}", k, v));
		}
	}
	// Game menu: take over saved settings (windowless test runs: unchanged, as before)
	const bool cfgTest = std::getenv("PS2XGS_CFG") != nullptr; // test run with fixed menu settings
	if (cfgTest)
		s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotSize", 1); // Screenshot in interner Aufloesung
	if (!(surfaceless && *surfaceless && *surfaceless != '0') || cfgTest)
	{
		// Sound: SPU2 output through Cubeb (PulseAudio/PipeWire), 48 kHz, time stretching against crackling
		if (!cfgTest)
		{
			s_settings_interface.SetStringValue("SPU2/Output", "Backend", "Cubeb");
			s_settings_interface.SetIntValue("SPU2/Output", "BufferMS", 50);
			s_settings_interface.SetIntValue("SPU2/Output", "OutputLatencyMS", 20);
		}
		ps2xmenu::toSettings(s_settings_interface);
		if (widescreen)
			s_settings_interface.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
		g_ps2xMenuDraw = &ps2xmenu::draw;
		ps2xmenu::active.store(true);
		g_ps2xCpuHook = &ps2xmenu::cpuHook;
		// Start menu: settings before playing (can be turned off in the menu)
		bool sm;
		{ std::lock_guard<std::mutex> lk(ps2xmenu::m); sm = ps2xmenu::cfg.startMenu; }
		// started from the launcher: settings were made there, the menu stays closed (ESC/F1 opens it)
		const bool vomLauncher = std::getenv("TS2_LAUNCHER") != nullptr;
		if (sm && !cfgTest && !vomLauncher)
			ps2xmenu::open.store(true);
	}
	s_settings_interface.SetBoolValue("EmuCore", "WarnAboutUnsafeSettings", false); // no PCSX2 warning messages in the picture
	// Picture output: SotC draws full frames, the PCRTC shows them as fields (SMODE2 INT=1, FFMD=0). PCSX2's
	// automatic deinterlacing (adaptive) makes moving characters jitter/"wobble" on screen, not visible in
	// screenshots. So: deinterlacing off = show the full frame directly. PS2X_DEINTERLACE=<0..9> for testing.
	{
		int di = 1; // GSInterlaceMode::Off
		if (const char* e = std::getenv("PS2X_DEINTERLACE"); e && *e) di = std::atoi(e);
		s_settings_interface.SetIntValue("EmuCore/GS", "deinterlace_mode", di);
	}
	{
		// HD-Texturen (Schrift, Logos, Menues): eigener Ordner, PCSX2-Austauschmechanismus
		std::string tex;
		if (const char* t = std::getenv("PS2XGS_TEXDIR"); t && *t) tex = t;
		else tex = ps2xmenu::texRoot();
		s_settings_interface.SetStringValue("Folders", "Textures", tex.c_str());
		EmuFolders::Textures = tex; // already set at start from the PCSX2 configuration
		{ const unsigned n = ps2xmenu::assembleTextures(); if (n) Console.WriteLn("[ps2x] textures: %u replacement files active (HD text/packs)", n); }
		s_settings_interface.SetBoolValue("EmuCore/GS", "LoadTextureReplacements", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "LoadTextureReplacementsAsync", true); // never load on the graphics thread (stutter); preloading at start prevents pop-in
		s_settings_interface.SetBoolValue("EmuCore/GS", "PrecacheTextureReplacements", true);
		const char* dump = std::getenv("PS2XGS_TEXDUMP");
		const bool d = dump && *dump == '1';
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpReplaceableTextures", d);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpReplaceableMipmaps", false);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpDirectTextures", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpPaletteTextures", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpTexturesWithFMVActive", false);
		Console.WriteLn(fmt::format("[ps2xgs] HD textures: {}{}", tex, d ? " (dumping on)" : ""));
	}

	s_ps2x_params.filename = live_path; // ends in ps2x_live.gs -> GSDumpReplayer live mode
	s_ps2x_cpu_thread = std::thread(CPUThreadMain, &s_ps2x_params, &s_ps2x_ret);
	PS2XDIAG("[ps2xgs] start: CPU thread started");
	s_ps2x_pump_thread = std::thread([] { GSRunner::PumpPlatformMessages(true); });
	// Wait until the VM (incl. SPU2) is ready: the port writes SPU2 registers right afterwards (sound driver start)
	// up to 30 s: on some drivers Vulkan first builds pipeline/shader caches on the first start
	int ms = 0;
	for (; ms < 30000 && !VMManager::HasValidVM(); ms += 10)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	if (VMManager::HasValidVM())
	{
		char b[64]; std::snprintf(b, sizeof(b), "[ps2xgs] start: VM ready (%d ms)", ms);
		PS2XDIAG(b);
	}
	else
		PS2XDIAG("[ps2xgs] start: VM not ready after 30 s");
	// PS2XGS_VM_PFLICHT=1 (Windows launcher): without a valid VM (e.g. the graphics device could not be created) report
	// failure so that the launcher can restart with another renderer instead of showing a black window
	if (const char* e = std::getenv("PS2XGS_VM_PFLICHT"); e && *e == '1' && !VMManager::HasValidVM())
		return 0;
	return 1;
}

PS2XGS_API void ps2xgs_transfer(int path, const unsigned char* data, unsigned int size)
{
	GSDumpReplayer::LivePush(0, static_cast<u8>(path), data, size);
}

// Hand over the calling thread's collected graphics packets (port: after every VU1 job and before every handover)
PS2XGS_API void ps2xgs_flush()
{
	GSDumpReplayer::LiveFlush();
}

// GS download for VIF1 DMA towards memory (games read VRAM back, e.g. TimeSplitters 2):
// hand over open packets, then read the VIF1 FIFO from the GS as PCSX2 does (BITBLTBUF/TRXPOS/TRXREG from the GS state)
PS2XGS_API void ps2xgs_read_fifo(unsigned char* mem, unsigned int qwc)
{
	GSDumpReplayer::LiveReadFifo(mem, qwc);   // on the CPU thread in packet order, waits for completion
}

// ps2x: GS download without stopping the EE (TS2 reads back every menu frame; the result is only needed at the VBlank)
PS2XGS_API void* ps2xgs_read_fifo_async(unsigned char* mem, unsigned int qwc)
{
	return GSDumpReplayer::LiveReadFifoAsync(mem, qwc);
}

PS2XGS_API void ps2xgs_read_fifo_wait(void* ticket)
{
	GSDumpReplayer::LiveReadFifoWait(static_cast<GSDumpReplayer::LiveReadFifoReq*>(ticket));
}

PS2XGS_API void ps2xgs_vsync(const unsigned char* regs0x2000, int field)
{
	// Preloading the replacement textures (texture packs): wait once at start until the loader thread is done instead of
	// loading during the first minutes in the menu (that stuttered). At most 45 s; PS2XGS_TEX_WARTEN=0 turns it off.
	static unsigned s_vsyncs = 0;
	// Remove PCSX2 notices that do not apply to the port (no PCSX2 patch database, input runs through the port
	// itself) again in the first seconds instead of showing them as an overlay top left
	if (s_vsyncs < 600 && (s_vsyncs % 15u) == 0u)
	{
		Host::RemoveKeyedOSDMessage("PatchesZipOpenWarning");
		Host::RemoveKeyedOSDMessage("ControllerNotConfigured");
	}
	if (++s_vsyncs == 120) // after the first frames (internal resolution known)
	{
		if (const char* c = std::getenv("PS2XGS_CAPTURE"); c && *c) ps2xhot::aufnahmeStart(c);
	}
	if (s_vsyncs == 20)
	{
		static const bool warten = [] { const char* e = std::getenv("PS2XGS_TEX_WARTEN"); return !(e && *e == '0'); }();
		const unsigned pending = GSTextureReplacements::PendingWorkerItems();
		if (warten && pending > 0)
		{
			const auto t0 = std::chrono::steady_clock::now();
			const bool ok = GSTextureReplacements::WaitForWorkerIdle(45000);
			Console.WriteLn("[ps2x] textures preloaded: %u files in %.1f s%s", pending,
				std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), ok ? "" : " (Zeitlimit)");
		}
	}
	GSDumpReplayer::LivePush(3, 0, regs0x2000, 0x2000);
	GSDumpReplayer::LivePush(1, static_cast<u8>(field), nullptr, 0);
}

PS2XGS_API unsigned int ps2xgs_queue_size()
{
	return static_cast<unsigned int>(GSDumpReplayer::LiveQueueSize());
}

PS2XGS_API int ps2xgs_turbo() { return ps2xhot::turbo.load() ? 1 : 0; }
PS2XGS_API void ps2xgs_capture_end() { ps2xhot::aufnahmeBeenden(); }

PS2XGS_API void ps2xgs_screenshot(const char* path)
{
	std::string p(path);
	gsCall([p]() { ps2xShotAspect(); GSQueueSnapshot(p); });
}

// Input for the port: pressed PS2 buttons (1 = pressed) and sticks (lx, ly, rx, ry; 0x80 = centre).
// Gamepad and keyboard (in the game window) are merged. Returns 1 if a window/gamepad is present.
PS2XGS_API int ps2xgs_pad(unsigned short* pressed, unsigned char* axes4)
{
	u32 b = ps2xin::keyBtns.load() | ps2xin::padBtns.load();
	{
		// Mouse wheel impulses: press the action for ~6 queries per notch, then release briefly
		static int s_halten = 0, s_akt = -1;
		if (s_halten > 0) { --s_halten; if (s_halten == 0) s_halten = -4; }
		else if (s_halten < 0) ++s_halten;
		else
			for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
				if (ps2xin::impulse[i].load() > 0) { ps2xin::impulse[i].fetch_sub(1); s_akt = i; s_halten = 6; break; }
		// Zoom (hold) works like the aim button of the original: it brings up the scope at the zoom level the game
		// remembers (the first press shows the first level - checked against PCSX2, build/ref789). The zoom level itself is
		// changed with the mouse wheel while the zoom key is held (D-pad up/down in the original); without the zoom key the
		// wheel changes the weapon as before. Before, holding the key also held D-pad up, which zoomed in right away.
		const bool zh = ps2xin::zoomGehalten();
		if (s_halten > 0 && s_akt >= 0 && ps2xmenu::kAktionen[s_akt].wirkung == ps2xmenu::W_TASTE)
		{
			u32 w = ps2xmenu::kAktionen[s_akt].wert;
			if (zh && w == ps2xin::RIGHT) w = ps2xin::UP; else if (zh && w == ps2xin::LEFT) w = ps2xin::DOWN;
			b |= w;
		}
		const bool za = zh;
		ps2xosd::zoomAktiv.store(za);
		if (za) b |= ps2xin::L2;
	}
	u32 ax = ps2xin::padAxes.load();
	if (ps2xmenu::open.load() || ps2xmenu::latch.load())
	{
		// Menu open or just closed: the game gets nothing until everything is released
		if (!ps2xmenu::open.load() && b == 0 && (ps2xin::keyStick.load() == 0))
			ps2xmenu::latch.store(false);
		if (pressed) *pressed = 0;
		if (axes4) std::memset(axes4, 0x80, 4);
		return (s_display != nullptr || ps2xin::padOn.load()) ? 1 : 0;
	}
	unsigned char a[4] = {static_cast<unsigned char>(ax), static_cast<unsigned char>(ax >> 8), static_cast<unsigned char>(ax >> 16), static_cast<unsigned char>(ax >> 24)};
	const u32 ks = ps2xin::keyStick.load();
	if (ks & 1) a[1] = 0x00; if (ks & 2) a[1] = 0xFF; if (ks & 4) a[0] = 0x00; if (ks & 8) a[0] = 0xFF;
	if (ks & 16) a[3] = 0x00; if (ks & 32) a[3] = 0xFF; if (ks & 64) a[2] = 0x00; if (ks & 128) a[2] = 0xFF;
	ps2xosd::btns.store(b);
	if (ps2xmenu::onlinePlus() && ps2xfeed::imMatch()) b &= ~static_cast<u32>(ps2xin::SELECT);   // Online+: Select = own scoreboard, not the game's
	if (pressed) *pressed = static_cast<unsigned short>(b);
	if (axes4) std::memcpy(axes4, a, 4); ps2xosd::axes.store(static_cast<u32>(a[0]) | (static_cast<u32>(a[1]) << 8) | (static_cast<u32>(a[2]) << 16) | (static_cast<u32>(a[3]) << 24));
	ps2xosd::keyBtns.store(ps2xin::keyBtns.load());
	{
		// Input log (letzter_start.log): on change, at most ~10/s -> diagnosing walking/stick
		static u32 s_last = 0xFFFFFFFFu, s_lastAx = 0;
		static auto s_t = std::chrono::steady_clock::now();
		const u32 axq = (a[0] >> 4) | ((a[1] >> 4) << 4) | ((a[2] >> 4) << 8) | ((a[3] >> 4) << 12);
		const auto now = std::chrono::steady_clock::now();
		if ((b != s_last || axq != s_lastAx) && now - s_t > std::chrono::milliseconds(100))
		{
			s_last = b; s_lastAx = axq; s_t = now;
			std::fprintf(stderr, "[Eingabe] tasten=%04x links=%02x/%02x rechts=%02x/%02x pad=%d tast=%02x\n", b, a[0], a[1], a[2], a[3],
				ps2xin::padOn.load() ? 1 : 0, ps2xin::keyStick.load());
		}
	}
	return (s_display != nullptr || ps2xin::padOn.load()) ? 1 : 0;
}

// Widescreen: 0 = the game decides, 1 = force 16:9, 2 = force 4:3
// ps2x: input per player (0 = as ps2xgs_pad, 1..3 = further gamepads). Returns 1 = gamepad present.
PS2XGS_API int ps2xgs_pad_n(int player, unsigned short* pressed, unsigned char* axes4)
{
	if (player <= 0)
		return ps2xgs_pad(pressed, axes4);
	if (player > 3 || !ps2xin::xOn[player - 1].load())
		return 0;
	const u32 ax = ps2xin::xAxes[player - 1].load();
	if (pressed) *pressed = static_cast<unsigned short>(ps2xin::xBtns[player - 1].load());
	if (axes4) { axes4[0] = ax & 0xFF; axes4[1] = (ax >> 8) & 0xFF; axes4[2] = (ax >> 16) & 0xFF; axes4[3] = (ax >> 24) & 0xFF; }
	return 1;
}

PS2XGS_API int ps2xgs_wide_mode()
{
	if (!ps2xmenu::active.load())
		return 0;
	int a;
	{ std::lock_guard<std::mutex> lk(ps2xmenu::m); a = ps2xmenu::cfg.aspect; }
	return (a == 0 || a == 4) ? 1 : a == 2 ? 2 : 0;
}
// Ultrawide (aspect option 4): width/height of the window x 1000, so the game renders with exactly that aspect ratio.
// 0 = not ultrawide (16:9 or 4:3 as before).
PS2XGS_API int ps2xgs_aspect_milli()
{
	if (!ps2xmenu::active.load())
		return 0;
	int a;
	{ std::lock_guard<std::mutex> lk(ps2xmenu::m); a = ps2xmenu::cfg.aspect; }
	if (a != 4 || s_wi.surface_width == 0 || s_wi.surface_height == 0)
		return 0;
	return static_cast<int>(static_cast<u64>(s_wi.surface_width) * 1000u / s_wi.surface_height);
}
// The game reports its video mode -> switch the output aspect ratio to match
PS2XGS_API void ps2xgs_game_wide(int wide)
{
	if (ps2xmenu::gameWide.exchange(wide != 0) != (wide != 0))
		ps2xmenu::applyPending.store(true);
}
// ---- Sound: SPU2 chip from PCSX2, driven by the port's IOP (registers, DMA, clock, interrupts) ----
extern void (*g_ps2xSpuIrqHook)(int); // IopIrq.cpp (globaler Namensraum!)
namespace ps2xspu
{
	std::atomic<u32> irqs{0}; // Bit 0 SPU-IRQ (INUM 9), Bit 1 DMA4 fertig, Bit 2 DMA7 fertig
	std::atomic<u64> nDma{0}, nDmaHw{0}, nIrq[3];
	void irqHook(int which) { irqs.fetch_or(1u << which); nIrq[which].fetch_add(1); }
	bool hooked = false;
	bool ready() { return VMManager::HasValidVM(); }
	void ensure()
	{
		if (!hooked)
		{
			g_ps2xSpuIrqHook = &irqHook;
			hooked = true;
		}
	}
}
PS2XGS_API void ps2xspu_write(unsigned int addr, unsigned short value, unsigned int cycle)
{
	if (!ps2xspu::ready()) return;
	ps2xspu::ensure();
	psxRegs.cycle = cycle;
	SPU2write(addr, value);
	{
		// PS2X_SPULOG: activity of the sound driver (register writes, key-on) every 5 s
		static const bool log = std::getenv("PS2X_SPULOG") != nullptr;
		if (log)
		{
			static u64 n = 0, kon = 0;
			static auto last = std::chrono::steady_clock::now();
			++n;
			const u32 off = addr & 0x3FF;
			if ((off == 0x1A0 || off == 0x1A2) && value) ++kon;
			const auto now = std::chrono::steady_clock::now();
			if (now - last > std::chrono::seconds(5))
			{
				last = now;
				Console.WriteLn(fmt::format("[SPU2] {} Registerschreibungen, {} Key-On, {} DMA ({} KB), IRQ spu/dma4/dma7 {}/{}/{}", n, kon,
					ps2xspu::nDma.load(), ps2xspu::nDmaHw.load() / 512, ps2xspu::nIrq[0].load(), ps2xspu::nIrq[1].load(), ps2xspu::nIrq[2].load()));
				for (int c = 0; c < 2; ++c)
				{
					const V_Core& k = Cores[c];
					std::string vs;
					for (int v = 0; v < 24; ++v)
					{
						const V_Voice& vo = k.Voices[v];
						if (vo.ADSR.Phase == 0) continue;
						int nz = 0;
						for (u32 i = 0; i < 0x1000; ++i) nz += _spu2mem[(vo.StartA + i) & 0xFFFFF] != 0;
						vs += fmt::format(" v{}[a={:x} nz={} n={:x} p={:x} vol={}/{} env={} g={}/{}]", v, vo.StartA, nz, vo.NextA, vo.Pitch,
							vo.Volume.Left.Value, vo.Volume.Right.Value, vo.ADSR.Value, k.VoiceGates[v].DryL, k.VoiceGates[v].DryR);
					}
					Console.WriteLn(fmt::format("[SPU2] Kern{}: irq={} irqa={:x} master={}/{} inp={}/{} ext={}/{} fx={} admactl={:x} dry snd/inp/ext {}/{} {}/{} {}/{}{}", c,
						k.IRQEnable, k.IRQA, k.MasterVol.Left.Value, k.MasterVol.Right.Value, k.InpVol.Left, k.InpVol.Right, k.ExtVol.Left, k.ExtVol.Right, k.FxEnable,
						k.AutoDMACtrl, k.DryGate.SndL, k.DryGate.SndR, k.DryGate.InpL, k.DryGate.InpR, k.DryGate.ExtL, k.DryGate.ExtR, vs));
				}
			}
		}
	}
}
PS2XGS_API unsigned short ps2xspu_read(unsigned int addr, unsigned int cycle)
{
	if (!ps2xspu::ready()) return 0;
	ps2xspu::ensure();
	psxRegs.cycle = cycle;
	const u16 r = SPU2read(addr);
	{
		// PS2X_SPUBUSYLOG: libsd-Belegtpruefung (ATTR-DMA-Bits 0x30, ADMAS) sichtbar machen
		static const bool bl = std::getenv("PS2X_SPUBUSYLOG") != nullptr;
		static int bn = 0;
		const u32 off = addr & 0x3FF;
		if (bl && bn < 400 && ((off == 0x19A && (r & 0x30)) || (off == 0x1B0 && r)))
		{
			++bn;
			Console.WriteLn(fmt::format("[SPU2-BELEGT] addr={:x} wert={:x}", addr, r));
		}
	}
	return r;
}
// DMA: core 0 = DMA4, 1 = DMA7; toSpu = 1 Speicher -> SPU2; halfwords = Anzahl 16-Bit-Woerter
PS2XGS_API void ps2xspu_dma(int core, unsigned short* mem, unsigned int halfwords, int toSpu, unsigned int cycle)
{
	if (!ps2xspu::ready()) { ps2xspu::irqs.fetch_or(core == 0 ? 2u : 4u); return; }
	ps2xspu::ensure();
	ps2xspu::nDma.fetch_add(1); ps2xspu::nDmaHw.fetch_add(halfwords);
	psxRegs.cycle = cycle;
	if (core == 0) HW_DMA4_CHCR |= 0x01000000; else HW_DMA7_CHCR |= 0x01000000;
	{
		static const bool dlog = std::getenv("PS2X_SPUDMALOG") != nullptr;
		static int dn = 0;
		if (dlog && dn < 400)
		{
			++dn;
			int nz = 0;
			for (u32 i = 0; i < halfwords; ++i) nz += mem[i] != 0;
			Console.WriteLn(fmt::format("[SPU2-DMA] kern{} {} tsa={:x} aktiv={:x} hw={} nz={} adma={:x} rest={}", core, toSpu ? "->SPU" : "<-SPU",
				Cores[core].TSA, Cores[core].ActiveTSA, halfwords, nz, Cores[core].AutoDMACtrl, Cores[core].ReadSize));
		}
	}
	if (toSpu)
	{
		if (core == 0) SPU2writeDMA4Mem(mem, halfwords); else SPU2writeDMA7Mem(mem, halfwords);
		// Copy normal (non-auto) DMA into SPU2 memory completely and at once. PCSX2 otherwise copies in 0x100-word
		// pieces over the transfer time while still reading from IOP memory. The port reports the DMA to the game
		// as done right away (IOP time hardly advances during RPC bursts); the game then immediately overwrites the buffer with the
		// next sound bank part. Completion (IRQA check, DMA IRQ) happens at the next SPU2 tick as usual.
		V_Core& k = Cores[core];
		if (!k.AdmaInProgress)
		{
			for (int guard = 0; k.ReadSize && guard < 65536; ++guard)
				k.FinishDMAwrite();
			k.LastClock = psxRegs.cycle;
			k.DMAICounter = 1;
			k.Regs.STATX |= 0x80;
			k.Regs.STATX &= ~0x400;
		}
	}
	else
	{
		if (core == 0) SPU2readDMA4Mem(mem, halfwords); else SPU2readDMA7Mem(mem, halfwords);
	}
}
PS2XGS_API void ps2xspu_async(unsigned int cycle)
{
	if (!ps2xspu::ready()) return;
	ps2xspu::ensure();
	psxRegs.cycle = cycle;
	SPU2async();
}
PS2XGS_API unsigned int ps2xspu_take_irqs()
{
	return ps2xspu::irqs.exchange(0);
}

// Game options for the port (id 0 = camera blur, 1 = TS2 motion trail). Without an active menu (test runs): fallback.
// Online+ hit/kill sounds: short synthesized clicks on an own SDL audio stream (independent of the SPU2 output)
namespace ps2xklang
{
	std::mutex m; SDL_AudioStream* st = nullptr; bool versucht = false;
	void spiele(bool kill)
	{
		int vol; bool an; { std::lock_guard<std::mutex> lk(ps2xmenu::m); an = ps2xmenu::cfg.impact != 0 && ps2xmenu::cfg.impactSound != 0; vol = ps2xmenu::cfg.volume; }
		static const bool env = [] { const char* e = std::getenv("TS2_IMPACT"); return e && *e == '1'; }();
		if ((!an && !env) || !ps2xmenu::onlinePlus()) return;
		std::lock_guard<std::mutex> lk(m);
		if (!st && !versucht)
		{
			versucht = true;
			if (SDL_InitSubSystem(SDL_INIT_AUDIO))
			{
				const SDL_AudioSpec sp{SDL_AUDIO_F32, 1, 48000};
				st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sp, nullptr, nullptr);
				if (st) SDL_ResumeAudioStreamDevice(st);
			}
		}
		if (!st) return;
		// hit: bright 2.6 kHz tick (35 ms); kill: tick + low thump (120 ms)
		const int n = kill ? 5760 : 1680; std::vector<float> b(n);
		const float g = 0.35f * std::clamp(vol, 0, 200) / 100.0f;
		for (int i = 0; i < n; ++i)
		{
			const float t = i / 48000.0f;
			float v = std::sin(6.2831853f * 2600.0f * t) * std::exp(-t * 140.0f);
			if (kill) v = 0.6f * v + std::sin(6.2831853f * (140.0f - 260.0f * t) * t) * std::exp(-t * 22.0f) + 0.4f * std::sin(6.2831853f * 1800.0f * t) * std::exp(-(t - 0.04f) * (t - 0.04f) * 9000.0f);
			b[i] = std::clamp(v * g, -1.0f, 1.0f);
		}
		SDL_PutAudioStreamData(st, b.data(), static_cast<int>(b.size() * sizeof(float)));
	}
}
namespace ps2xklang
{
	// announcer sting: rising square-ish arpeggio, more notes for bigger streaks
	void fanfare(int stufe)
	{
		int vol; bool an; { std::lock_guard<std::mutex> lk(ps2xmenu::m); an = ps2xmenu::cfg.impact != 0 && ps2xmenu::cfg.impactSound != 0; vol = ps2xmenu::cfg.volume; }
		static const bool env = [] { const char* e = std::getenv("TS2_IMPACT"); return e && *e == '1'; }();
		if ((!an && !env) || !ps2xmenu::onlinePlus()) return;
		std::lock_guard<std::mutex> lk(m);
		if (!st && !versucht)
		{
			versucht = true;
			if (SDL_InitSubSystem(SDL_INIT_AUDIO))
			{
				const SDL_AudioSpec sp{SDL_AUDIO_F32, 1, 48000};
				st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sp, nullptr, nullptr);
				if (st) SDL_ResumeAudioStreamDevice(st);
			}
		}
		if (!st) return;
		static const float noten[] = {523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f};
		const int nN = std::clamp(stufe + 1, 2, 5); const int lang = 3600; // 75 ms per note
		std::vector<float> b(static_cast<size_t>(nN * lang + 9600), 0.0f);
		const float g = 0.22f * std::clamp(vol, 0, 200) / 100.0f;
		for (int k = 0; k < nN; ++k)
		{
			const int dauer = (k == nN - 1) ? lang + 9600 : lang;
			for (int i = 0; i < dauer; ++i)
			{
				const float t = i / 48000.0f; const float ph = noten[k] * t;
				float v = (ph - std::floor(ph) < 0.5f ? 1.0f : -1.0f) * 0.45f + std::sin(6.2831853f * ph) * 0.55f;
				v *= std::min(1.0f, t * 400.0f) * std::exp(-t * (k == nN - 1 ? 6.0f : 18.0f));
				b[static_cast<size_t>(k * lang + i)] += std::clamp(v * g, -1.0f, 1.0f);
			}
		}
		SDL_PutAudioStreamData(st, b.data(), static_cast<int>(b.size() * sizeof(float)));
	}
}
namespace ps2xstats
{
	std::mutex m; bool geladen = false; long long s_xp = 0; int s_kills = 0, s_hits = 0;
	std::string pfad() { return ps2xmenu::dir() + "/stats.ini"; }
	void laden()
	{
		if (geladen) return; geladen = true;
		std::ifstream f(pfad()); std::string l;
		while (std::getline(f, l)) { const size_t e = l.find('='); if (e == std::string::npos) continue; const std::string k = l.substr(0, e); const long long v = std::atoll(l.c_str() + e + 1);
			if (k == "xp") s_xp = v; else if (k == "kills") s_kills = static_cast<int>(v); else if (k == "hits") s_hits = static_cast<int>(v); }
	}
	void speichern() { std::ofstream f(pfad()); f << "xp=" << s_xp << "\nkills=" << s_kills << "\nhits=" << s_hits << "\n"; }
	long long xpFuer(int lvl) { return 250LL * lvl * (lvl - 1); }   // XP needed to reach a level: 0, 500, 1500, 3000, ...
	int levelVon(long long x) { int l = 1; while (l < 50 && xpFuer(l + 1) <= x) ++l; return l; }
	int level() { std::lock_guard<std::mutex> lk(m); laden(); return levelVon(s_xp); }
	long long xp() { std::lock_guard<std::mutex> lk(m); laden(); return s_xp; }
	int kills() { std::lock_guard<std::mutex> lk(m); laden(); return s_kills; }
	int hits() { std::lock_guard<std::mutex> lk(m); laden(); return s_hits; }
	void dazu(long long n, bool istKill)
	{
		int vor, nach;
		{ std::lock_guard<std::mutex> lk(m); laden(); vor = levelVon(s_xp); s_xp += n; if (istKill) ++s_kills; else ++s_hits; nach = levelVon(s_xp); if (istKill || s_hits % 10 == 0) speichern(); }
		if (nach > vor) ps2xhot::toast(fmt::format("LEVEL UP  {}  -  {}", nach, ps2xRang(nach)));
	}
	void treffer() { dazu(10, false); }
	// server copy of the Online+ profile (same player on another computer, or local data lost): keep the higher values
	void abgleich(long long xpS, long long kS, long long hS)
	{
		int vor, nach;
		{ std::lock_guard<std::mutex> lk(m); laden(); vor = levelVon(s_xp); bool neu = false;
		  if (xpS > s_xp) { s_xp = xpS; neu = true; } if (kS > s_kills) { s_kills = static_cast<int>(kS); neu = true; } if (hS > s_hits) { s_hits = static_cast<int>(hS); neu = true; }
		  nach = levelVon(s_xp); if (neu) speichern(); }
		if (nach > vor) ps2xhot::toast(fmt::format("LEVEL {}  -  {}", nach, ps2xRang(nach)));
	}
	void bonus(long long n)
	{
		int vor, nach;
		{ std::lock_guard<std::mutex> lk(m); laden(); vor = levelVon(s_xp); s_xp += n; nach = levelVon(s_xp); speichern(); }
		if (nach > vor) ps2xhot::toast(fmt::format("LEVEL UP  {}  -  {}", nach, ps2xRang(nach)));
	}
	void kill() { dazu(100, true); }
}
PS2XGS_API int ps2xgs_option(int id, int fallback)
{
	if (id == 9) { ps2xosd::ego.store(fallback != 0); return fallback; } // Message from the game: first-person view with the mouse active
	if (id == 10) return ps2xosd::zoomAktiv.load() ? 1 : 0;
	if (id == 19) { static const bool lg = std::getenv("TS2_IMPACT_LOG") != nullptr; if (lg) std::fprintf(stderr, "[Online+] damage %d\n", fallback); ps2xosd::schadenStaerke.store(std::clamp(fallback, 1, 100)); ps2xosd::schadenMs.store(ps2xosd::jetztMs()); return fallback; } // player took damage
	if (id == 20) { if (ps2xosd::todN.exchange(fallback) != fallback) { std::lock_guard<std::mutex> lk(ps2xosd::am); ps2xosd::serie = 0; ps2xosd::kette = 0; } return fallback; } // player died
	if (id == 28)   // Online+ in-game server browser: refresh the list (0 = ready, 1 = still searching)
	{
		if (fallback == 1 && !ps2xmenu::online::laeuft.load()) { ps2xmenu::Cfg c0; { std::lock_guard<std::mutex> lk(ps2xmenu::m); c0 = ps2xmenu::cfg; } ps2xmenu::online::aktualisieren(c0.onlineFavs, c0.onlineServer); }
		return ps2xmenu::online::laeuft.load() ? 1 : 0;
	}
	if (id == 24) return ps2xstats::level();   // Online+ lobby: profile values for the in-game panel
	if (id == 25) return static_cast<int>(std::min<long long>(ps2xstats::xp(), 2000000000LL));
	if (id == 26) return ps2xstats::kills();
	if (id == 27) { const int lv = ps2xstats::level(); return lv >= 50 ? static_cast<int>(std::min<long long>(ps2xstats::xp(), 2000000000LL)) : static_cast<int>(ps2xstats::xpFuer(lv + 1)); }
	if (id == 23) { std::lock_guard<std::mutex> lk(ps2xmenu::m); return ps2xmenu::onlinePlus() ? ps2xmenu::cfg.impactAnsage : 0; } // announcer through the game's messages
	if (id == 22) return ps2xmenu::onlinePlus() ? 1 : fallback; // Online+ edition: killfeed / scoreboard tracking on
	if (id == 17) return 0; // main menu item "Online+": the game opens its own network menu now (no overlay window)
	if (id == 18) { const int c = ps2xmenu::botAnfrage.exchange(0); return c ? c : fallback; } // quick bot match
	if (id == 13 || id == 14) // Online+ events from the game: new hit / kill counter value
	{
		auto& n = id == 13 ? ps2xosd::trefferN : ps2xosd::killN;
		if (n.exchange(fallback) != fallback)
		{
			(id == 13 ? ps2xosd::trefferMs : ps2xosd::killMs).store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			if (id == 13) ps2xstats::treffer(); else ps2xstats::kill();
			ps2xklang::spiele(id == 14);
			if (id == 14)
			{
				// multi-kill: kills within 4 s chain up; streak: kills without dying
				const long long jetzt = ps2xosd::jetztMs();
				std::string txt; int stufe = 0; long long bonusXp = 0;
				{
					std::lock_guard<std::mutex> lk(ps2xosd::am);
					ps2xosd::kette = (jetzt - ps2xosd::letzterKillMs <= 4000) ? ps2xosd::kette + 1 : 1;
					ps2xosd::letzterKillMs = jetzt; ++ps2xosd::serie;
					static const char* kKette[] = {"", "", "DOUBLE KILL", "TRIPLE KILL", "MULTI KILL", "MEGA KILL", "MONSTER KILL"};
					static const char* kSerie[] = {"KILLING SPREE", "RAMPAGE", "UNSTOPPABLE", "GODLIKE"};
					if (ps2xosd::kette >= 2) { txt = kKette[std::min(ps2xosd::kette, 6)]; stufe = std::min(ps2xosd::kette - 1, 5); bonusXp = 50LL * (ps2xosd::kette - 1); }
					if (ps2xosd::serie >= 5 && ps2xosd::serie % 5 == 0) { const int i = std::min(ps2xosd::serie / 5 - 1, 3); if (txt.empty() || i + 2 > stufe) { txt = kSerie[i]; stufe = i + 2; } bonusXp += 100LL * (i + 1); }
					if (!txt.empty()) { ps2xosd::ansText = txt; ps2xosd::ansMs = jetzt; ps2xosd::ansStufe = stufe; }
				}
				if (bonusXp > 0) ps2xstats::bonus(bonusXp);
				if (!txt.empty()) { std::fprintf(stderr, "[Online+] announcer: %s (+%lld XP)\n", txt.c_str(), bonusXp); ps2xklang::fanfare(stufe); }
			}
		}
		return fallback;
	}
	if (!ps2xmenu::active.load())
		return fallback;
	std::lock_guard<std::mutex> lk(ps2xmenu::m);
	switch (id)
	{
		case 0: return ps2xmenu::cfg.camBlur;
		case 1: return ps2xmenu::cfg.nachzieh;
		case 2: return ps2xmenu::cfg.maus ? 1 : 0;
		case 3: return ps2xmenu::cfg.mausSens;
		case 4: return ps2xmenu::cfg.mausInvert ? 1 : 0;
		case 5: return ps2xmenu::cfg.mausZielmodus ? 1 : 0;
		case 6: return ps2xmenu::cfg.zoomSens;
		case 7: return ps2xin::zoomGehalten() ? 1 : 0;
		case 8: return ps2xhot::freecam.load() ? 1 : 0;
		case 11: return ps2xmenu::cfg.nickMax;
		case 31: return ps2xmenu::cfg.unlockAll;
		case 32: return ps2xmenu::cfg.padModern;
		case 33: return ps2xmenu::cfg.padSens;
		case 12: return ps2xmenu::cfg.scope;
		case 15: return ps2xmenu::onlinePlus() ? ps2xmenu::cfg.impact : 0;
		case 16: return ps2xmenu::cfg.impactKick;
		case 21: return ps2xmenu::onlinePlus() ? ps2xmenu::cfg.impactRecoil : 0;
		default: return fallback;
	}
}
// Mouse aiming: fetch the collected relative movement. 0 = mouse not captured (menu, no focus, off).
PS2XGS_API int ps2xgs_mouse(int* dx, int* dy)
{
	const int x = ps2xin::mausDX.exchange(0), y = ps2xin::mausDY.exchange(0);
	if (!ps2xin::mausGefangen.load() || ps2xmenu::open.load())
	{
		if (dx) *dx = 0; if (dy) *dy = 0;
		return 0;
	}
	if (x != 0 || y != 0) ps2xin::mausAktivMs.store(ps2xin::jetztMs());
	if (dx) *dx = x; if (dy) *dy = y;
	return 1;
}
// Online (TS2): the game reached the network configuration -> open the server browser
PS2XGS_API void ps2xgs_online_browser()
{
	if (ps2xmenu::onlinePlus()) return;   // Online+: the server browser is a screen inside the game, no overlay window
	ps2xmenu::online::oeffnen.store(true);
}
// Text from the bridge for the game side (Online+ in-game screens): returns the length (0 = nothing)
//   1 = server list "address\tname\tplayers\tmax\tping\tpassword\n" (answering servers, best first), 2 = chosen server
PS2XGS_API int ps2xgs_text_get(int id, char* buf, int len)
{
	if (!buf || len <= 0) return 0;
	std::string t;
	if (id == 1)
	{
		std::lock_guard<std::mutex> lk(ps2xmenu::online::om);
		std::vector<ps2xmenu::online::Eintrag> v = ps2xmenu::online::liste;
		std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { if (a.antwort != b.antwort) return a.antwort; if (a.spieler != b.spieler) return a.spieler > b.spieler; return a.ping < b.ping; });
		for (const auto& e : v)
		{
			if (!e.antwort) continue;
			std::string n = e.name.empty() ? e.adresse : e.name; for (char& ch : n) if (ch == '\t' || ch == '\n') ch = ' ';
			t += e.adresse + "\t" + n.substr(0, 28) + "\t" + std::to_string(std::max(0, e.spieler)) + "\t" + std::to_string(e.max) + "\t" + std::to_string(std::max(0, e.ping)) + "\t" + (e.pw ? "1" : "0") + "\n";
		}
	}
	else if (id == 2) { std::lock_guard<std::mutex> lk(ps2xmenu::m); t = ps2xmenu::cfg.onlineServer; }
	const int n = static_cast<int>(std::min<size_t>(t.size(), static_cast<size_t>(len - 1)));
	std::memcpy(buf, t.data(), static_cast<size_t>(n)); buf[n] = 0;
	return n;
}
// Online (TS2): connection status of the game side for the server browser
// Text events from the game side: 1 = killfeed line "killer\tvictim\tflags", 2 = scoreboard "name\tkills\tdeaths\tyou\n..."
PS2XGS_API void ps2xgs_text(int id, const char* text)
{
	if (!text) return;
	std::lock_guard<std::mutex> lk(ps2xfeed::m);
	if (id == 1)
	{
		const std::string t(text); const size_t a = t.find('\t'), b = a == std::string::npos ? a : t.find('\t', a + 1);
		if (b == std::string::npos) return;
		ps2xfeed::zeilen.push_back({t.substr(0, a), t.substr(a + 1, b - a - 1), std::atoi(t.c_str() + b + 1), ps2xosd::jetztMs()});
		if (ps2xfeed::zeilen.size() > 6) ps2xfeed::zeilen.erase(ps2xfeed::zeilen.begin());
	}
	else if (id == 3)   // Online+ profile from the server: "xp\tkills\thits"
	{
		long long x = 0, k = 0, h = 0;
		if (std::sscanf(text, "%lld\t%lld\t%lld", &x, &k, &h) == 3) { ps2xfeed::m.unlock(); ps2xstats::abgleich(x, k, h); ps2xfeed::m.lock(); }
	}
	else if (id == 4)   // Online+ in-game server browser: server chosen -> remember it like the overlay browser does
	{
		ps2xfeed::m.unlock();
		{ std::lock_guard<std::mutex> lk(ps2xmenu::m); ps2xmenu::cfg.onlineServer = text; }
		ps2xmenu::save();
		ps2xfeed::m.lock();
	}
	else if (id == 2) { ps2xfeed::tafel = text; if (*text) ps2xfeed::tafelMs.store(ps2xosd::jetztMs()); else ps2xfeed::tafelMs.store(0); }
}
PS2XGS_API void ps2xgs_online_status(const char* text)
{
	std::lock_guard<std::mutex> lk(ps2xmenu::online::om);
	ps2xmenu::online::spielStatus = text ? text : "";
}
// Game menu open? The port then pauses the game (EE) at the next VBlank.
PS2XGS_API int ps2xgs_menu_open()
{
	return ps2xmenu::open.load() ? 1 : 0;
}
// Vibration from the game: small motor (on/off) and large motor (0..255), as on the DualShock 2
PS2XGS_API void ps2xgs_rumble(int small_on, int large)
{
	const u32 lo = small_on ? 0xFFFFu : 0u;
	const u32 hi = static_cast<u32>(std::clamp(large, 0, 255)) * 257u;
	ps2xin::rumble.store(lo | (hi << 16));
}

PS2XGS_API void ps2xgs_stop()
{
	Host::RequestVMShutdown(false, false, false);
	GSRunner::StopPlatformMessagePump();
	if (s_ps2x_cpu_thread.joinable())
		s_ps2x_cpu_thread.join();
	if (s_ps2x_pump_thread.joinable())
		s_ps2x_pump_thread.join();
	GSRunner::DestroyPlatformWindow();
}
