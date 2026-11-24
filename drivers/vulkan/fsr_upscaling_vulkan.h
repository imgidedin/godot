/**************************************************************************/
/*  fsr_upscaling_vulkan.h                                                */
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
#include <thirdparty/amd-ffx-api/api/include/ffx_api.h>

// AMD FSR Upscaling through the AMD FSR API (FSR SDK 2.x): FSR 4 on the Radeon GPUs that run it
// (RX 9000 with FP8, RX 6000 and 7000 with INT8), FSR 3.1 on every other GPU. The API lives in
// AMD's signed DLLs (`amd_fidelityfx_loader_vk.dll`, which loads `amd_fidelityfx_upscaler_vk.dll`),
// shipped next to the executable. They are loaded the first time someone asks whether the upscaler
// is available, so a game that never uses it never loads them.
class FSRUpscalingVulkan {
public:
	struct Functions {
		PfnFfxCreateContext create_context = nullptr;
		PfnFfxDestroyContext destroy_context = nullptr;
		PfnFfxConfigure configure = nullptr;
		PfnFfxQuery query = nullptr;
		PfnFfxDispatch dispatch = nullptr;
	};

private:
	BinaryMutex mutex;
	bool load_tried = false;
	bool available = false;
	void *module = nullptr;
	Functions functions;

	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;

	void _load();

public:
	// The device the upscaler runs on; set once the logical device exists.
	void set_device(VkPhysicalDevice p_physical_device, VkDevice p_device);

	// Loads the DLLs on the first call; true when the AMD FSR API offers an upscaler for this device.
	bool is_available();

	// Valid only after is_available() returned true.
	const Functions &get_functions() const { return functions; }
	VkPhysicalDevice get_physical_device() const { return physical_device; }
	VkDevice get_device() const { return device; }

	~FSRUpscalingVulkan();
};

#endif // WINDOWS_ENABLED
