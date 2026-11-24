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
#undef SL_GET
	get_instance_proc_addr_function = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
	if (init == nullptr || get_feature_function == nullptr || shutdown_function == nullptr || is_feature_supported == nullptr || functions.set_tag_for_frame == nullptr || functions.set_constants == nullptr || functions.evaluate_feature == nullptr || functions.get_new_frame_token == nullptr || functions.free_resources == nullptr || get_instance_proc_addr_function == nullptr) {
		ERR_PRINT("DLSS: sl.interposer.dll does not export the Streamline API.");
		FreeLibrary(lib);
		return false;
	}

	const sl::Feature features[] = { sl::kFeatureDLSS };
	const CharString engine_version = String(GODOT_VERSION_FULL_CONFIG).utf8();
	const Char16String plugin_dir = dir.replace("/", "\\").utf16();
	const wchar_t *plugin_paths[] = { (const wchar_t *)plugin_dir.get_data() };

	sl::Preferences preferences;
	preferences.renderAPI = sl::RenderAPI::eVulkan;
	preferences.featuresToLoad = features;
	preferences.numFeaturesToLoad = 1;
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

bool DLSSVulkan::register_device(VkPhysicalDevice p_physical_device) {
	MutexLock lock(mutex);
	if (physical_device != VK_NULL_HANDLE) {
		return false;
	}
	physical_device = p_physical_device;
	return true;
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
