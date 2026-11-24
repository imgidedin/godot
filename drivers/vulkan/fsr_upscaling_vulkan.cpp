/**************************************************************************/
/*  fsr_upscaling_vulkan.cpp                                              */
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

#include "fsr_upscaling_vulkan.h"

#ifdef WINDOWS_ENABLED

#include "core/os/os.h"
#include "core/string/print_string.h"

#define PLATFORM_WINDOWS // What ffx_api_loader.h checks to use LoadLibrary/GetProcAddress.
#include <thirdparty/amd-ffx-api/api/include/ffx_api_loader.h>
#include <thirdparty/amd-ffx-api/api/include/vk/ffx_api_vk.h>
#include <thirdparty/amd-ffx-api/upscalers/include/ffx_upscale.h>

void FSRUpscalingVulkan::set_device(VkPhysicalDevice p_physical_device, VkDevice p_device) {
	physical_device = p_physical_device;
	device = p_device;
}

void FSRUpscalingVulkan::_load() {
	// Full path: the DLL comes from next to the executable, never from the working directory.
	const String path = OS::get_singleton()->get_executable_path().get_base_dir().path_join("amd_fidelityfx_loader_vk.dll");
	HMODULE lib = LoadLibraryExW((LPCWSTR)path.utf16().get_data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (lib == nullptr) {
		print_verbose("AMD FSR Upscaling: amd_fidelityfx_loader_vk.dll not found next to the executable.");
		return;
	}

	ffxFunctions ffx = {};
	ffxLoadFunctions(&ffx, lib);
	if (ffx.CreateContext == nullptr || ffx.DestroyContext == nullptr || ffx.Configure == nullptr || ffx.Query == nullptr || ffx.Dispatch == nullptr) {
		print_line("AMD FSR Upscaling: amd_fidelityfx_loader_vk.dll does not export the AMD FSR API.");
		FreeLibrary(lib);
		return;
	}

	// The upscaler versions this device can run; none means no upscaler (e.g. the upscaler DLL is missing).
	VkDeviceContext device_context = {};
	device_context.vkDevice = device;
	device_context.vkPhysicalDevice = physical_device;
	device_context.vkDeviceProcAddr = vkGetDeviceProcAddr;

	ffxQueryDescGetVersions versions = {};
	versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	versions.device = &device_context;
	uint64_t count = 0;
	versions.outputCount = &count;
	if (ffx.Query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
		print_line("AMD FSR Upscaling: no upscaler available for this GPU.");
		FreeLibrary(lib);
		return;
	}

	module = lib;
	functions.create_context = ffx.CreateContext;
	functions.destroy_context = ffx.DestroyContext;
	functions.configure = ffx.Configure;
	functions.query = ffx.Query;
	functions.dispatch = ffx.Dispatch;
	available = true;
}

bool FSRUpscalingVulkan::is_available() {
	MutexLock lock(mutex);
	if (!load_tried && device != VK_NULL_HANDLE) {
		load_tried = true;
		_load();
	}
	return available;
}

FSRUpscalingVulkan::~FSRUpscalingVulkan() {
	if (module != nullptr) {
		FreeLibrary((HMODULE)module);
	}
}

#endif // WINDOWS_ENABLED
