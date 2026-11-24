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
#include "core/templates/safe_refcount.h"

#include <drivers/vulkan/godot_vulkan.h>
#include <thirdparty/streamline/include/sl.h>
#include <thirdparty/streamline/include/sl_dlss.h>
#include <thirdparty/streamline/include/sl_dlss_g.h>
#include <thirdparty/streamline/include/sl_reflex.h>

// NVIDIA DLSS Super Resolution through NVIDIA Streamline. Streamline sits between the engine and the
// Vulkan loader: Vulkan is loaded through its `vkGetInstanceProcAddr` (it adds what DLSS needs to the
// instance and device it creates, and follows the presents), so it is set up before Vulkan, and only
// when an NVIDIA GPU is present and the signed `sl.interposer.dll` is next to the executable. Its
// plugins (`sl.common.dll`, `sl.dlss.dll`, and `NvLowLatencyVk.dll`, which sl.common loads) and DLSS
// itself (`nvngx_dlss.dll`) go next to it.
//
// DLSS Frame Generation (`sl.dlss_g.dll`, with `nvngx_dlssg.dll`) is loaded with it but kept unloaded
// (unhooked) unless a swap chain generates frames: loaded, it renders every swap chain off-screen and
// copies, even when off. It needs Reflex (`sl.reflex.dll`) and PC Latency (`sl.pcl.dll`), whose
// markers (latency_marker()) go every frame while it is loaded, with Godot's frame index; without it
// they cost nothing (Streamline asks for them always, for its latency statistics: left out, so that
// a player without frame generation pays nothing for it).
class DLSSVulkan {
public:
	// The frame's steps, in order (see latency_marker()). The present ones come from the driver.
	enum LatencyMarker {
		LATENCY_FRAME_START, // Before the input of the frame is read: Reflex sleeps here.
		LATENCY_SIMULATION_END,
		LATENCY_RENDER_SUBMIT_START, // The render thread starts the frame.
	};

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

	// Reflex, PC Latency and DLSS Frame Generation (found when the device is registered).
	PFun_slSetFeatureLoaded *set_feature_loaded = nullptr;
	PFun_slReflexSleep *reflex_sleep = nullptr;
	PFun_slReflexSetOptions *reflex_set_options = nullptr;
	PFun_slPCLSetMarker *pcl_set_marker = nullptr;
	PFun_slDLSSGSetOptions *dlssg_set_options = nullptr;
	PFun_slDLSSGGetState *dlssg_get_state = nullptr;
	uint32_t frame_generation_status = 0; // sl::DLSSGStatus last reported (its failures are logged once each).
	bool latency_supported = false; // Reflex and PCL run on this GPU.
	bool frame_generation_supported = false;
	bool frame_generation_loaded = false;
	SafeFlag markers_on; // The latency markers go (while frame generation is loaded).

	// Streamline's frame tokens, markers and loading are not thread-safe: sl_mutex around each call.
	// Loading must not happen during Reflex's sleep (Vulkan calls): sleep_mutex, taken first.
	BinaryMutex sl_mutex;
	BinaryMutex sleep_mutex;
	// Main thread: the last frame each of its markers went for (a frame not drawn keeps its index).
	uint64_t frame_start_sent = UINT64_MAX;
	uint64_t simulation_end_sent = UINT64_MAX;
	// Render thread: the frame it is on (from LATENCY_RENDER_SUBMIT_START), which DLSS's constants,
	// the frame generation tags and the present markers go with, and whether it was presented.
	uint32_t render_frame = 0;
	bool render_frame_started = false;
	bool render_frame_presented = false;
	// The frame generation of the frame being rendered (see frame_generation_prepare()).
	bool frame_generation_prepared = false;
	bool frame_generation_on = false;
	uint32_t frame_generation_viewport = 0;

	sl::FrameToken *_get_frame_token(uint32_t p_frame);
	void _pcl_marker(sl::PCLMarker p_marker, uint32_t p_frame);
	void _frame_generation_set_off();

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

	// A step of frame p_frame (Godot's frame index, Engine::get_frames_drawn() when it starts). Any
	// thread: the main one gives the frame start and simulation end, the render thread the rest.
	void latency_marker(LatencyMarker p_marker, uint64_t p_frame);
	// The token of the frame the render thread is on (null if Streamline is not up).
	sl::FrameToken *get_render_frame_token();
	// Around the presents (render thread): the first of the frame sends its submit end and present
	// markers and settles the frame generation of the frame (off if it was not prepared).
	void present_begin();
	void present_end();

	// DLSS Frame Generation (RTX 40 and newer).
	bool is_frame_generation_available();
	// Hooks it (before a swap chain that generates frames is created) or unhooks it (after that swap
	// chain was destroyed, before the next one is made).
	void set_frame_generation_loaded(bool p_loaded);
	bool is_frame_generation_loaded() const { return frame_generation_loaded; }
	// Render thread, once the inputs of the frame are tagged (for p_viewport, DLSS's): generate from it.
	void frame_generation_prepare(uint32_t p_viewport, const sl::DLSSGOptions &p_options);
	// Before the swap chain that generates frames is released (resize, window closed): off first.
	void frame_generation_before_release();
	// The viewport (DLSS context) is freed: its frame generation resources too.
	void frame_generation_free_viewport(uint32_t p_viewport);

	// Before the logical device is destroyed.
	void shutdown();

	~DLSSVulkan();
};

#endif // WINDOWS_ENABLED
