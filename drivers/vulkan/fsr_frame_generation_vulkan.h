/**************************************************************************/
/*  fsr_frame_generation_vulkan.h                                         */
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
#include "core/templates/local_vector.h"
#include "drivers/vulkan/fsr_upscaling_vulkan.h"

// AMD FSR Frame Generation through the AMD FSR API: between two rendered frames, the swapchain AMD's
// DLL provides presents a generated one. That swapchain replaces Godot's (its functions replace
// vkCreateSwapchainKHR, vkGetSwapchainImagesKHR, vkAcquireNextImageKHR and vkQueuePresentKHR for
// it), and it presents from its own threads, on queues of its own: the device reserves them, outside
// the ones Godot uses, when the frame generation DLL is next to the executable. Each frame the
// renderer gives it the depth, the motion vectors, the camera and the image before the UI (see
// dispatch()), which it uses to keep the UI out of the generated frame.
class FSRFrameGenerationVulkan {
public:
	struct Queue {
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t family = UINT32_MAX;
	};

	// One per swap chain that generates frames.
	struct SwapChain {
		void *swap_chain_context = nullptr; // ffxContext of the replacement swapchain.
		void *frame_generation_context = nullptr; // ffxContext of the frame generation.
		PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
		PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
		PFN_vkQueuePresentKHR queue_present = nullptr;
		void *destroy_swapchain = nullptr; // PFN_vkDestroySwapchainFFXAPI.
		PfnFfxDispatch dispatch_function = nullptr; // For the generation callback, which only gets this.
		VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;
		uint32_t width = 0; // The swapchain's size, which the generated frames have.
		uint32_t height = 0;
		uint64_t frame_id = 0;
		bool prepared = false; // dispatch() gave this frame's inputs.
	};

	// The inputs of one frame (Vulkan handles and descriptions, see dispatch()).
	struct FrameInputs {
		VkImage depth = VK_NULL_HANDLE; // R32_SFLOAT, reversed Z.
		VkImage motion_vectors = VK_NULL_HANDLE; // RG16F, UV units, to the previous position.
		VkImage hudless = VK_NULL_HANDLE; // The image before the UI, like the backbuffer.
		uint32_t depth_format = 0; // FfxApiSurfaceFormat.
		uint32_t motion_vectors_format = 0;
		uint32_t hudless_format = 0;
		uint32_t render_width = 0;
		uint32_t render_height = 0;
		uint32_t display_width = 0;
		uint32_t display_height = 0;
		float jitter_x = 0.0f; // Pixels.
		float jitter_y = 0.0f;
		float frame_time_delta = 0.0f; // Seconds.
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f; // Radians.
		float camera_position[3] = {}; // World space.
		float camera_up[3] = {};
		float camera_right[3] = {};
		float camera_forward[3] = {};
		bool reset = false;
	};

private:
	FSRUpscalingVulkan *api = nullptr; // The AMD FSR API (the same loader DLL as the upscaler).
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	Queue present_queue;
	Queue acquire_queue;
	Queue async_compute_queue;

	BinaryMutex mutex;
	bool support_checked = false;
	bool supported = false;

	void _configure_disabled(SwapChain &p_swap_chain);
	void _destroy(SwapChain &r_swap_chain);

public:
	// Whether the frame generation DLL is next to the executable: then the device reserves its queues.
	static bool is_installed();
	// Once the device exists, with the queues it reserved for this (none if it did not).
	void set_device(FSRUpscalingVulkan *p_api, VkPhysicalDevice p_physical_device, VkDevice p_device, const Queue &p_present_queue, const Queue &p_acquire_queue, const Queue &p_async_compute_queue);

	// True when the reserved queues exist and the AMD FSR API offers frame generation for this GPU.
	bool is_available();

	// Creates the replacement swapchain for p_create_info (instead of vkCreateSwapchainKHR) and the
	// frame generation that presents into it. p_game_queue is the queue Godot presents on; Godot
	// submits there under p_game_queue_mutex, and so must AMD's threads.
	// p_hudless_format: the VkFormat of the image before the UI (the window's render target).
	Error create(SwapChain &r_swap_chain, const VkSwapchainCreateInfoKHR &p_create_info, VkQueue p_game_queue, uint32_t p_game_queue_family, BinaryMutex *p_game_queue_mutex, VkFormat p_hudless_format);
	// Waits for its pending presents and destroys everything create() made, the swapchain included.
	void destroy(SwapChain &r_swap_chain);

	// Once per rendered frame, recorded into p_command_buffer before the frame is presented: the
	// configuration (frame generation on) and the inputs of this frame. Nothing when the inputs are
	// not the size of the swapchain (the frame then goes out as rendered).
	void dispatch(SwapChain &p_swap_chain, VkCommandBuffer p_command_buffer, const FrameInputs &p_inputs);
	// Right before each present: a frame without dispatch() (no 3D drawn, or inputs that did not fit)
	// turns the generation off for it, so no frame is generated from old inputs.
	void before_present(SwapChain &p_swap_chain);
};

#endif // WINDOWS_ENABLED
