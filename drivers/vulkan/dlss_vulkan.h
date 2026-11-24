/**************************************************************************/
/*  dlss_vulkan.h                                                         */
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

#pragma once

#ifdef WINDOWS_ENABLED

#include "core/os/mutex.h"

#include <drivers/vulkan/godot_vulkan.h>
#include <thirdparty/streamline/include/sl.h>
#include <thirdparty/streamline/include/sl_dlss.h>

// NVIDIA DLSS Super Resolution through NVIDIA Streamline. Streamline sits between the engine and the
// Vulkan loader: Vulkan is loaded through its `vkGetInstanceProcAddr` (it adds what DLSS needs to the
// instance and device it creates, and follows the presents), so it is set up before Vulkan, and only
// when an NVIDIA GPU is present and the signed `sl.interposer.dll` is next to the executable. Its
// plugins (`sl.common.dll`, `sl.dlss.dll`, and `NvLowLatencyVk.dll`, which sl.common loads) and DLSS
// itself (`nvngx_dlss.dll`) go next to it.
class DLSSVulkan {
public:
	struct Functions {
		PFun_slSetTagForFrame *set_tag_for_frame = nullptr;
		PFun_slSetConstants *set_constants = nullptr;
		PFun_slEvaluateFeature *evaluate_feature = nullptr;
		PFun_slGetNewFrameToken *get_new_frame_token = nullptr;
		PFun_slFreeResources *free_resources = nullptr;
		PFun_slDLSSGetOptimalSettings *dlss_get_optimal_settings = nullptr;
		PFun_slDLSSSetOptions *dlss_set_options = nullptr;
	};

private:
	void *module = nullptr;
	bool initialized = false; // slInit succeeded, slShutdown not called yet.
	PFun_slShutdown *shutdown_function = nullptr;
	PFun_slIsFeatureSupported *is_feature_supported = nullptr;
	PFun_slGetFeatureFunction *get_feature_function = nullptr;
	PFN_vkGetInstanceProcAddr get_instance_proc_addr_function = nullptr;
	Functions functions;

	BinaryMutex mutex;
	bool support_checked = false;
	bool supported = false;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;

public:
	// Before Vulkan is loaded: true when Streamline is up, and Vulkan must then be loaded through
	// get_instance_proc_addr().
	bool initialize_before_vulkan();
	PFN_vkGetInstanceProcAddr get_instance_proc_addr() const { return get_instance_proc_addr_function; }

	// The device DLSS runs on: the first one created (the main rendering device). Returns whether
	// p_physical_device is it; local rendering devices created later are not (nor do they shut
	// Streamline down when they go).
	bool register_device(VkPhysicalDevice p_physical_device);
	// True when DLSS runs on this GPU (asks Streamline once).
	bool is_available();
	// Valid only after is_available() returned true.
	const Functions &get_functions() const { return functions; }

	// Before the logical device is destroyed.
	void shutdown();

	~DLSSVulkan();
};

#endif // WINDOWS_ENABLED
