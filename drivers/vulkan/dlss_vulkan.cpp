/**************************************************************************/
/*  dlss_vulkan.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "dlss_vulkan.h"

#ifdef WINDOWS_ENABLED

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/version.h"

#include <dxgi.h>
// Checks the NVIDIA signature of sl.interposer.dll before loading it (it then checks its plugins).
#include <thirdparty/streamline/include/sl_security.h>

static constexpr UINT NVIDIA_VENDOR_ID = 0x10DE;

// Whether any adapter is an NVIDIA GPU, without creating a Vulkan instance (DXGI, loaded only here).
static bool _has_nvidia_adapter() {
	HMODULE lib_dxgi = LoadLibraryW(L"dxgi.dll");
	if (lib_dxgi == nullptr) {
		return false;
	}
	typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void **);
	PFN_CreateDXGIFactory1 create_factory = (PFN_CreateDXGIFactory1)(void *)GetProcAddress(lib_dxgi, "CreateDXGIFactory1");
	bool found = false;
	IDXGIFactory1 *factory = nullptr;
	if (create_factory != nullptr && SUCCEEDED(create_factory(__uuidof(IDXGIFactory1), (void **)&factory))) {
		IDXGIAdapter1 *adapter = nullptr;
		for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 desc = {};
			found = SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == NVIDIA_VENDOR_ID;
			adapter->Release();
		}
		factory->Release();
	}
	FreeLibrary(lib_dxgi);
	return found;
}

static void _on_sl_log(sl::LogType p_type, const char *p_message) {
	const String message = String::utf8(p_message).strip_edges();
	if (p_type == sl::LogType::eError) {
		ERR_PRINT("Streamline: " + message);
	} else if (p_type == sl::LogType::eWarn) {
		WARN_PRINT("Streamline: " + message);
	} else {
		print_verbose("Streamline: " + message);
	}
}

bool DLSSVulkan::initialize_before_vulkan() {
	const String dir = OS::get_singleton()->get_executable_path().get_base_dir();
	const String path = dir.path_join("sl.interposer.dll").replace("/", "\\");
	if (!FileAccess::exists(path)) {
		return false;
	}
	if (!_has_nvidia_adapter()) {
		print_verbose("DLSS: no NVIDIA GPU, Streamline not loaded.");
		return false;
	}
	if (!sl::security::verifyEmbeddedSignature((const wchar_t *)path.utf16().get_data())) {
		ERR_PRINT("DLSS: sl.interposer.dll is not signed by NVIDIA, Streamline not loaded.");
		return false;
	}
	HMODULE lib = LoadLibraryExW((LPCWSTR)path.utf16().get_data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	ERR_FAIL_NULL_V_MSG(lib, false, "DLSS: could not load sl.interposer.dll.");

#define SL_GET(m_name) (PFun_##m_name *)(void *)GetProcAddress(lib, #m_name)
	PFun_slInit *init = SL_GET(slInit);
	get_feature_function = SL_GET(slGetFeatureFunction);
	shutdown_function = SL_GET(slShutdown);
	is_feature_supported = SL_GET(slIsFeatureSupported);
	functions.set_tag_for_frame = SL_GET(slSetTagForFrame);
	functions.set_constants = SL_GET(slSetConstants);
	functions.evaluate_feature = SL_GET(slEvaluateFeature);
	functions.get_new_frame_token = SL_GET(slGetNewFrameToken);
	functions.free_resources = SL_GET(slFreeResources);
	set_feature_loaded = SL_GET(slSetFeatureLoaded);
#undef SL_GET
	get_instance_proc_addr_function = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
	if (init == nullptr || get_feature_function == nullptr || shutdown_function == nullptr || is_feature_supported == nullptr || functions.set_tag_for_frame == nullptr || functions.set_constants == nullptr || functions.evaluate_feature == nullptr || functions.get_new_frame_token == nullptr || functions.free_resources == nullptr || set_feature_loaded == nullptr || get_instance_proc_addr_function == nullptr) {
		ERR_PRINT("DLSS: sl.interposer.dll does not export the Streamline API.");
		FreeLibrary(lib);
		return false;
	}

	// The plugins that are not next to the executable are left out by Streamline (logged).
	const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	const CharString engine_version = String(GODOT_VERSION_FULL_CONFIG).utf8();
	const Char16String plugin_dir = dir.replace("/", "\\").utf16();
	const wchar_t *plugin_paths[] = { (const wchar_t *)plugin_dir.get_data() };

	sl::Preferences preferences;
	preferences.renderAPI = sl::RenderAPI::eVulkan;
	preferences.featuresToLoad = features;
	preferences.numFeaturesToLoad = std::size(features);
	preferences.engine = sl::EngineType::eCustom;
	preferences.engineVersion = engine_version.get_data();
	// Identifies this engine build to NVIDIA's runtime (an application id is only given by NVIDIA).
	preferences.projectId = "bc321fa7-f426-4936-9698-7c0b324ba655";
	preferences.pathsToPlugins = plugin_paths;
	preferences.numPathsToPlugins = 1;
	preferences.logMessageCallback = _on_sl_log;
	preferences.logLevel = OS::get_singleton()->is_stdout_verbose() ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
	// No over-the-air updates: the DLLs shipped next to the executable are the ones that run. Godot's
	// render graph restores the command buffer state itself (every command binds what it uses).
	preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

	const sl::Result result = init(preferences, sl::kSDKVersion);
	if (result != sl::Result::eOk) {
		ERR_PRINT(vformat("DLSS: Streamline did not start (error %d).", (int)result));
		FreeLibrary(lib);
		return false;
	}
	initialized = true;
	module = lib;
	print_verbose("DLSS: Streamline started; Vulkan goes through it.");
	return true;
}

template <typename T>
static T *_get_function(PFun_slGetFeatureFunction *p_get, sl::Feature p_feature, const char *p_name) {
	void *function = nullptr;
	return p_get(p_feature, p_name, function) == sl::Result::eOk ? (T *)function : nullptr;
}

bool DLSSVulkan::register_device(VkPhysicalDevice p_physical_device) {
	MutexLock lock(mutex);
	if (physical_device != VK_NULL_HANDLE) {
		return false;
	}
	physical_device = p_physical_device;
	if (!initialized) {
		return true;
	}

	// The device was created through Streamline: the features' functions exist now.
	sl::AdapterInfo adapter;
	adapter.vkPhysicalDevice = physical_device;
	if (is_feature_supported(sl::kFeatureReflex, adapter) == sl::Result::eOk && is_feature_supported(sl::kFeaturePCL, adapter) == sl::Result::eOk) {
		reflex_sleep = _get_function<PFun_slReflexSleep>(get_feature_function, sl::kFeatureReflex, "slReflexSleep");
		reflex_set_options = _get_function<PFun_slReflexSetOptions>(get_feature_function, sl::kFeatureReflex, "slReflexSetOptions");
		pcl_set_marker = _get_function<PFun_slPCLSetMarker>(get_feature_function, sl::kFeaturePCL, "slPCLSetMarker");
		latency_supported = reflex_sleep != nullptr && reflex_set_options != nullptr && pcl_set_marker != nullptr;
	}
	if (latency_supported) {
		// Reflex starts off (its options must be set once).
		sl::ReflexOptions reflex;
		reflex.mode = sl::ReflexMode::eOff;
		reflex_set_options(reflex);
	}
	frame_generation_supported = latency_supported && is_feature_supported(sl::kFeatureDLSS_G, adapter) == sl::Result::eOk;
	// Unhooked until a swap chain generates frames (none exists yet).
	set_feature_loaded(sl::kFeatureDLSS_G, false);
	print_verbose(vformat("DLSS: Frame Generation %s.", frame_generation_supported ? "available" : "not available"));
	return true;
}

sl::FrameToken *DLSSVulkan::_get_frame_token(uint32_t p_frame) {
	MutexLock lock(sl_mutex);
	sl::FrameToken *token = nullptr;
	return functions.get_new_frame_token(token, &p_frame) == sl::Result::eOk ? token : nullptr;
}

void DLSSVulkan::_pcl_marker(sl::PCLMarker p_marker, uint32_t p_frame) {
	sl::FrameToken *token = _get_frame_token(p_frame);
	ERR_FAIL_NULL(token);
	MutexLock lock(sl_mutex);
	pcl_set_marker(p_marker, *token);
}

void DLSSVulkan::latency_marker(LatencyMarker p_marker, uint64_t p_frame) {
	if (!initialized) {
		return;
	}
	const uint32_t frame = uint32_t(p_frame);
	switch (p_marker) {
		case LATENCY_FRAME_START: {
			// (A frame that was not drawn keeps its index: its markers went already.)
			if (!markers_on.is_set() || p_frame == frame_start_sent) {
				return;
			}
			frame_start_sent = p_frame;
			sl::FrameToken *token = _get_frame_token(frame);
			ERR_FAIL_NULL(token);
			{
				// Reflex's low latency mode waits here (without the device's or Streamline's lock).
				MutexLock lock(sleep_mutex);
				reflex_sleep(*token);
			}
			_pcl_marker(sl::PCLMarker::eSimulationStart, frame);
		} break;
		case LATENCY_SIMULATION_END: {
			if (!markers_on.is_set() || p_frame == simulation_end_sent) {
				return;
			}
			simulation_end_sent = p_frame;
			_pcl_marker(sl::PCLMarker::eSimulationEnd, frame);
		} break;
		case LATENCY_RENDER_SUBMIT_START: {
			render_frame = frame;
			render_frame_started = true;
			render_frame_presented = false;
			if (markers_on.is_set()) {
				_pcl_marker(sl::PCLMarker::eRenderSubmitStart, frame);
			}
		} break;
	}
}

sl::FrameToken *DLSSVulkan::get_render_frame_token() {
	return initialized ? _get_frame_token(render_frame) : nullptr;
}

void DLSSVulkan::present_begin() {
	// Once per frame (a frame may present a window early, see RenderingDevice::screen_prepare_for_drawing),
	// and not before the first frame (the boot splash).
	if (!initialized || !render_frame_started || render_frame_presented) {
		return;
	}
	if (markers_on.is_set()) {
		_pcl_marker(sl::PCLMarker::eRenderSubmitEnd, render_frame);
		_pcl_marker(sl::PCLMarker::ePresentStart, render_frame);
	}

	// A frame that was not prepared (no 3D, no DLSS) turns the frame generation off for it, so
	// nothing is generated from old inputs.
	if (frame_generation_on && !frame_generation_prepared) {
		_frame_generation_set_off();
	}
	frame_generation_prepared = false;

	// Why it does not generate, if its inputs are wrong (each reason once).
	if (frame_generation_on && dlssg_get_state != nullptr) {
		sl::DLSSGState state;
		if (dlssg_get_state(sl::ViewportHandle(frame_generation_viewport), state, nullptr) == sl::Result::eOk) {
			const uint32_t status = (uint32_t)state.status;
			const uint32_t new_failures = status & ~frame_generation_status;
			frame_generation_status = status;
			static const char *reasons[] = { "the resolution is too low", "Reflex was not detected", "the HDR format is not supported", "the camera constants are invalid", "the back buffer index was not asked for" };
			for (uint32_t i = 0; i < std::size(reasons); i++) {
				if (new_failures & (1u << i)) {
					WARN_PRINT(vformat("DLSS Frame Generation: off, %s.", reasons[i]));
				}
			}
		}
	}
}

void DLSSVulkan::present_end() {
	if (!initialized || !render_frame_started || render_frame_presented) {
		return;
	}
	render_frame_presented = true;
	if (markers_on.is_set()) {
		_pcl_marker(sl::PCLMarker::ePresentEnd, render_frame);
	}
}

bool DLSSVulkan::is_frame_generation_available() {
	return is_available() && frame_generation_supported;
}

void DLSSVulkan::set_frame_generation_loaded(bool p_loaded) {
	if (!frame_generation_supported || frame_generation_loaded == p_loaded) {
		return;
	}
	// Not while the main thread is in Reflex's sleep, nor during another Streamline call.
	MutexLock sleep_lock(sleep_mutex);
	MutexLock lock(sl_mutex);
	if (set_feature_loaded(sl::kFeatureDLSS_G, p_loaded) != sl::Result::eOk) {
		ERR_PRINT(vformat("DLSS: could not %s Frame Generation.", p_loaded ? "load" : "unload"));
		return;
	}
	frame_generation_loaded = p_loaded;
	// Its functions live in its plugin, which unloading may free.
	dlssg_set_options = p_loaded ? _get_function<PFun_slDLSSGSetOptions>(get_feature_function, sl::kFeatureDLSS_G, "slDLSSGSetOptions") : nullptr;
	dlssg_get_state = p_loaded ? _get_function<PFun_slDLSSGGetState>(get_feature_function, sl::kFeatureDLSS_G, "slDLSSGGetState") : nullptr;
	frame_generation_status = 0;
	frame_generation_on = false;
	frame_generation_prepared = false;
	// Frame Generation runs with Reflex's low latency mode (it would add the latency of a frame), and
	// the markers it needs go only meanwhile.
	sl::ReflexOptions reflex;
	reflex.mode = p_loaded ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
	reflex_set_options(reflex);
	markers_on.set_to(p_loaded);
}

void DLSSVulkan::_frame_generation_set_off() {
	sl::DLSSGOptions options;
	options.mode = sl::DLSSGMode::eOff;
	dlssg_set_options(sl::ViewportHandle(frame_generation_viewport), options);
	frame_generation_on = false;
}

void DLSSVulkan::frame_generation_prepare(uint32_t p_viewport, const sl::DLSSGOptions &p_options) {
	ERR_FAIL_NULL(dlssg_set_options);
	if (frame_generation_on && p_viewport != frame_generation_viewport) {
		// A new DLSS context (another size or mode): the old viewport stops.
		_frame_generation_set_off();
	}
	// The options each frame (they hold the sizes, which may change); on and off only change here.
	const sl::Result result = dlssg_set_options(sl::ViewportHandle(p_viewport), p_options);
	if (result != sl::Result::eOk) {
		ERR_PRINT_ONCE(vformat("DLSS: could not set the Frame Generation options (Streamline error %d).", (int)result));
		return;
	}
	frame_generation_viewport = p_viewport;
	frame_generation_on = true;
	frame_generation_prepared = true;
}

void DLSSVulkan::frame_generation_before_release() {
	// Streamline asks for it off before a resize or a mode change (its presents could deadlock).
	if (frame_generation_on && dlssg_set_options != nullptr) {
		_frame_generation_set_off();
	}
}

void DLSSVulkan::frame_generation_free_viewport(uint32_t p_viewport) {
	if (frame_generation_loaded) {
		if (frame_generation_on && p_viewport == frame_generation_viewport) {
			_frame_generation_set_off();
		}
		functions.free_resources(sl::kFeatureDLSS_G, sl::ViewportHandle(p_viewport));
	}
}

bool DLSSVulkan::is_available() {
	MutexLock lock(mutex);
	if (!support_checked && initialized && physical_device != VK_NULL_HANDLE) {
		support_checked = true;
		sl::AdapterInfo adapter;
		adapter.vkPhysicalDevice = physical_device;
		const sl::Result result = is_feature_supported(sl::kFeatureDLSS, adapter);
		if (result == sl::Result::eOk) {
			// The DLSS functions exist once the device does (created through Streamline by now).
			void *function = nullptr;
			if (get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", function) == sl::Result::eOk) {
				functions.dlss_get_optimal_settings = (PFun_slDLSSGetOptimalSettings *)function;
			}
			function = nullptr;
			if (get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", function) == sl::Result::eOk) {
				functions.dlss_set_options = (PFun_slDLSSSetOptions *)function;
			}
		}
		supported = result == sl::Result::eOk && functions.dlss_get_optimal_settings != nullptr && functions.dlss_set_options != nullptr;
		if (!supported) {
			print_line(vformat("DLSS: not available on this GPU (Streamline result %d).", (int)result));
		}
	}
	return supported;
}

void DLSSVulkan::shutdown() {
	MutexLock lock(mutex);
	if (initialized) {
		shutdown_function();
		initialized = false;
		supported = false;
		latency_supported = false;
		markers_on.clear();
		frame_generation_supported = false;
		frame_generation_loaded = false;
	}
}

DLSSVulkan::~DLSSVulkan() {
	shutdown();
	// Vulkan was loaded through this library, and the instance (destroyed before this) used it.
	if (module != nullptr) {
		FreeLibrary((HMODULE)module);
	}
}

#endif // WINDOWS_ENABLED
