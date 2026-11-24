/**************************************************************************/
/*  fsr_frame_generation_vulkan.cpp                                       */
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

#include "fsr_frame_generation_vulkan.h"

#ifdef WINDOWS_ENABLED

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#include <thirdparty/amd-ffx-api/api/include/vk/ffx_api_vk.h>
#include <thirdparty/amd-ffx-api/framegeneration/include/ffx_framegeneration.h>
#include <thirdparty/amd-ffx-api/framegeneration/include/vk/ffx_api_framegeneration_vk.h>

// The queue Godot presents on, for AMD's threads to submit there too. The AMD FSR API takes a plain
// submit function with no context to tell queues apart, so it can only reach them through these;
// there is one rendering device presenting to windows, so one game queue.
static VkQueue game_queue = VK_NULL_HANDLE;
static BinaryMutex *game_queue_mutex = nullptr;

static VkResult _submit_to_game_queue(uint32_t p_submit_count, const VkSubmitInfo *p_submits, VkFence p_fence) {
	MutexLock lock(*game_queue_mutex);
	return vkQueueSubmit(game_queue, p_submit_count, p_submits, p_fence);
}

static ffxReturnCode_t _generate_frame(ffxDispatchDescFrameGeneration *p_params, void *p_user_context) {
	FSRFrameGenerationVulkan::SwapChain *swap_chain = (FSRFrameGenerationVulkan::SwapChain *)p_user_context;
	ffxContext context = swap_chain->frame_generation_context;
	return swap_chain->dispatch_function(&context, &p_params->header);
}

static void _on_ffx_message(uint32_t p_type, const wchar_t *p_message) {
	const String message = String::utf16((const char16_t *)p_message);
	if (p_type == FFX_API_MESSAGE_TYPE_ERROR) {
		ERR_PRINT("AMD FSR Frame Generation: " + message);
	} else {
		WARN_PRINT("AMD FSR Frame Generation: " + message);
	}
}

bool FSRFrameGenerationVulkan::is_installed() {
	return FileAccess::exists(OS::get_singleton()->get_executable_path().get_base_dir().path_join("amd_fidelityfx_framegeneration_vk.dll"));
}

void FSRFrameGenerationVulkan::set_device(FSRUpscalingVulkan *p_api, VkPhysicalDevice p_physical_device, VkDevice p_device, const Queue &p_present_queue, const Queue &p_acquire_queue, const Queue &p_async_compute_queue) {
	api = p_api;
	physical_device = p_physical_device;
	device = p_device;
	present_queue = p_present_queue;
	acquire_queue = p_acquire_queue;
	async_compute_queue = p_async_compute_queue;
}

bool FSRFrameGenerationVulkan::is_available() {
	MutexLock lock(mutex);
	if (!support_checked && api != nullptr) {
		support_checked = true;
		if (present_queue.queue == VK_NULL_HANDLE || acquire_queue.queue == VK_NULL_HANDLE || async_compute_queue.queue == VK_NULL_HANDLE) {
			print_verbose("AMD FSR Frame Generation: no queues reserved for it (the DLL was not next to the executable, or the GPU has too few queues).");
		} else if (api->is_available()) {
			VkDeviceContext device_context = {};
			device_context.vkDevice = device;
			device_context.vkPhysicalDevice = physical_device;
			device_context.vkDeviceProcAddr = vkGetDeviceProcAddr;
			ffxQueryDescGetVersions versions = {};
			versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
			versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
			versions.device = &device_context;
			uint64_t count = 0;
			versions.outputCount = &count;
			supported = api->get_functions().query(nullptr, &versions.header) == FFX_API_RETURN_OK && count > 0;
			if (!supported) {
				print_line("AMD FSR Frame Generation: not available for this GPU.");
			}
		}
	}
	return supported;
}

Error FSRFrameGenerationVulkan::create(SwapChain &r_swap_chain, const VkSwapchainCreateInfoKHR &p_create_info, VkQueue p_game_queue, uint32_t p_game_queue_family, BinaryMutex *p_game_queue_mutex, VkFormat p_hudless_format) {
	ERR_FAIL_COND_V(!is_available(), ERR_UNAVAILABLE);
	const FSRUpscalingVulkan::Functions &ffx = api->get_functions();
	game_queue = p_game_queue;
	game_queue_mutex = p_game_queue_mutex;

	// The swapchain.
	ffxCreateContextDescFrameGenerationSwapChainVersionVK swapchain_version = {};
	swapchain_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
	swapchain_version.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;

	ffxCreateContextDescFrameGenerationSwapChainVK swapchain_desc = {};
	swapchain_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
	swapchain_desc.header.pNext = &swapchain_version.header;
	swapchain_desc.physicalDevice = physical_device;
	swapchain_desc.device = device;
	r_swap_chain.vk_swapchain = VK_NULL_HANDLE;
	swapchain_desc.swapchain = &r_swap_chain.vk_swapchain;
	swapchain_desc.createInfo = p_create_info;
	swapchain_desc.createInfo.oldSwapchain = VK_NULL_HANDLE;
	swapchain_desc.gameQueue = { p_game_queue, p_game_queue_family, _submit_to_game_queue };
	swapchain_desc.asyncComputeQueue = { async_compute_queue.queue, async_compute_queue.family, nullptr };
	swapchain_desc.presentQueue = { present_queue.queue, present_queue.family, nullptr };
	swapchain_desc.imageAcquireQueue = { acquire_queue.queue, acquire_queue.family, nullptr };

	ffxContext swapchain_context = nullptr;
	ffxReturnCode_t result = ffx.create_context(&swapchain_context, &swapchain_desc.header, nullptr);
	ERR_FAIL_COND_V_MSG(result != FFX_API_RETURN_OK, ERR_CANT_CREATE, vformat("AMD FSR Frame Generation: could not create the swapchain (error %d).", result));
	r_swap_chain.swap_chain_context = swapchain_context;
	r_swap_chain.width = p_create_info.imageExtent.width;
	r_swap_chain.height = p_create_info.imageExtent.height;

	// The functions right after the swapchain, so that it can be destroyed (it holds the window's
	// surface) when something after fails.
	ffxQueryDescSwapchainReplacementFunctionsVK functions = {};
	functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
	result = ffx.query(&swapchain_context, &functions.header);
	r_swap_chain.destroy_swapchain = (void *)functions.pOutDestroySwapchainFFXAPI;
	if (result != FFX_API_RETURN_OK || functions.pOutGetSwapchainImagesKHR == nullptr || functions.pOutAcquireNextImageKHR == nullptr || functions.pOutQueuePresentKHR == nullptr || functions.pOutDestroySwapchainFFXAPI == nullptr) {
		_destroy(r_swap_chain);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "AMD FSR Frame Generation: the swapchain did not give its functions.");
	}
	r_swap_chain.get_swapchain_images = functions.pOutGetSwapchainImagesKHR;
	r_swap_chain.acquire_next_image = functions.pOutAcquireNextImageKHR;
	r_swap_chain.queue_present = functions.pOutQueuePresentKHR;
	r_swap_chain.dispatch_function = ffx.dispatch;

	// The frame generation.
	ffxCreateBackendVKDesc backend = {};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backend.deviceContext.vkDevice = device;
	backend.deviceContext.vkPhysicalDevice = physical_device;
	backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

	ffxCreateContextDescFrameGenerationVersion version = {};
	version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
	version.header.pNext = &backend.header;
	version.version = FFX_FRAMEGENERATION_VERSION;

	ffxCreateContextDescFrameGenerationHudless hudless = {};
	hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
	hudless.header.pNext = &version.header;
	hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(p_hudless_format);

	ffxCreateContextDescFrameGeneration desc = {};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	desc.header.pNext = &hudless.header;
	// Reversed Z; motion vectors at render size, without the jitter (as the upscalers take them).
	desc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
	if (OS::get_singleton()->is_stdout_verbose()) {
		desc.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
	}
	// Rendering never goes above the display size (downsampling falls back to bilinear).
	desc.displaySize = { p_create_info.imageExtent.width, p_create_info.imageExtent.height };
	desc.maxRenderSize = desc.displaySize;
	desc.backBufferFormat = ffxApiGetSurfaceFormatVK(p_create_info.imageFormat);

	ffxContext frame_generation_context = nullptr;
	result = ffx.create_context(&frame_generation_context, &desc.header, nullptr);
	if (result != FFX_API_RETURN_OK) {
		_destroy(r_swap_chain);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("AMD FSR Frame Generation: could not create the frame generation (error %d).", result));
	}
	r_swap_chain.frame_generation_context = frame_generation_context;
	r_swap_chain.frame_id = 0;
	r_swap_chain.prepared = false;

	ffxConfigureDescGlobalDebug debug = {};
	debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
	debug.fpMessage = _on_ffx_message;
	debug.debugLevel = OS::get_singleton()->is_stdout_verbose() ? FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS : FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
	ffx.configure(&frame_generation_context, &debug.header);

	print_verbose(vformat("AMD FSR Frame Generation: swapchain %dx%d.", p_create_info.imageExtent.width, p_create_info.imageExtent.height));
	return OK;
}

void FSRFrameGenerationVulkan::_configure_disabled(SwapChain &p_swap_chain) {
	ffxConfigureDescFrameGeneration config = {};
	config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	config.swapChain = p_swap_chain.vk_swapchain;
	config.frameGenerationEnabled = false;
	config.frameID = ++p_swap_chain.frame_id;
	ffxContext context = p_swap_chain.frame_generation_context;
	const ffxReturnCode_t result = api->get_functions().configure(&context, &config.header);
	if (result != FFX_API_RETURN_OK) {
		ERR_PRINT_ONCE(vformat("AMD FSR Frame Generation: could not turn the generation off (error %d).", result));
	}
}

// In the order the API asks: the generation off, its presents done, the generation context, the
// swapchain context, and then the swapchain.
void FSRFrameGenerationVulkan::_destroy(SwapChain &r_swap_chain) {
	const FSRUpscalingVulkan::Functions &ffx = api->get_functions();
	if (r_swap_chain.frame_generation_context != nullptr) {
		_configure_disabled(r_swap_chain);
	}
	if (r_swap_chain.swap_chain_context != nullptr) {
		ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait = {};
		wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
		ffxContext context = r_swap_chain.swap_chain_context;
		ffx.dispatch(&context, &wait.header);
	}
	if (r_swap_chain.frame_generation_context != nullptr) {
		ffxContext context = r_swap_chain.frame_generation_context;
		ffx.destroy_context(&context, nullptr);
	}
	if (r_swap_chain.swap_chain_context != nullptr) {
		ffxContext context = r_swap_chain.swap_chain_context;
		ffx.destroy_context(&context, nullptr);
	}
	PFN_vkDestroySwapchainFFXAPI destroy_swapchain = (PFN_vkDestroySwapchainFFXAPI)r_swap_chain.destroy_swapchain;
	if (r_swap_chain.vk_swapchain != VK_NULL_HANDLE && destroy_swapchain != nullptr) {
		destroy_swapchain(device, r_swap_chain.vk_swapchain, nullptr, nullptr);
	}
	r_swap_chain = SwapChain();
}

void FSRFrameGenerationVulkan::destroy(SwapChain &r_swap_chain) {
	if (r_swap_chain.swap_chain_context != nullptr) {
		_destroy(r_swap_chain);
	}
}

static FfxApiResource _resource(VkImage p_image, uint32_t p_format, uint32_t p_width, uint32_t p_height, uint32_t p_state) {
	FfxApiResourceDescription description = {};
	description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	description.format = p_format;
	description.width = p_width;
	description.height = p_height;
	description.depth = 1;
	description.mipCount = 1;
	description.flags = FFX_API_RESOURCE_FLAGS_NONE;
	description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
	return ffxApiGetResourceVK((void *)p_image, description, p_state);
}

void FSRFrameGenerationVulkan::dispatch(SwapChain &p_swap_chain, VkCommandBuffer p_command_buffer, const FrameInputs &p_inputs) {
	ERR_FAIL_NULL(p_swap_chain.frame_generation_context);
	// The image before the UI must be the backbuffer's (a viewport stretched or letterboxed into the
	// window is not).
	if (p_inputs.display_width != p_swap_chain.width || p_inputs.display_height != p_swap_chain.height || p_inputs.render_width > p_swap_chain.width || p_inputs.render_height > p_swap_chain.height) {
		WARN_PRINT_ONCE(vformat("AMD FSR Frame Generation: the 3D image (%dx%d) is not the size of the window (%dx%d); frames are not generated.", p_inputs.display_width, p_inputs.display_height, p_swap_chain.width, p_swap_chain.height));
		return;
	}
	const FSRUpscalingVulkan::Functions &ffx = api->get_functions();
	ffxContext context = p_swap_chain.frame_generation_context;
	p_swap_chain.frame_id++;

	ffxConfigureDescFrameGeneration config = {};
	config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	config.swapChain = p_swap_chain.vk_swapchain;
	config.frameGenerationCallback = _generate_frame;
	config.frameGenerationCallbackUserContext = &p_swap_chain;
	config.frameGenerationEnabled = true;
	config.allowAsyncWorkloads = false;
	// The image before the UI: the generation finds the UI by comparing it with the backbuffer, and
	// leaves it out of the motion.
	config.HUDLessColor = _resource(p_inputs.hudless, p_inputs.hudless_format, p_inputs.display_width, p_inputs.display_height, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	config.generationRect = { 0, 0, int32_t(p_inputs.display_width), int32_t(p_inputs.display_height) };
	config.frameID = p_swap_chain.frame_id;
	ffxReturnCode_t result = ffx.configure(&context, &config.header);
	ERR_FAIL_COND_MSG(result != FFX_API_RETURN_OK, vformat("AMD FSR Frame Generation: could not configure the frame (error %d).", result));
	p_swap_chain.prepared = true;

	ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
	prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
	prepare.frameID = p_swap_chain.frame_id;
	prepare.commandList = p_command_buffer;
	prepare.renderSize = { p_inputs.render_width, p_inputs.render_height };
	prepare.jitterOffset = { p_inputs.jitter_x, p_inputs.jitter_y };
	// Godot's motion vectors are in UV units, pointing to where the pixel was in the previous frame.
	prepare.motionVectorScale = { float(p_inputs.render_width), float(p_inputs.render_height) };
	prepare.frameTimeDelta = p_inputs.frame_time_delta * 1000.0f;
	prepare.reset = p_inputs.reset;
	prepare.cameraNear = p_inputs.z_near;
	prepare.cameraFar = p_inputs.z_far;
	prepare.cameraFovAngleVertical = p_inputs.fovy;
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.depth = _resource(p_inputs.depth, p_inputs.depth_format, p_inputs.render_width, p_inputs.render_height, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	prepare.motionVectors = _resource(p_inputs.motion_vectors, p_inputs.motion_vectors_format, p_inputs.render_width, p_inputs.render_height, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	for (int i = 0; i < 3; i++) {
		prepare.cameraPosition[i] = p_inputs.camera_position[i];
		prepare.cameraUp[i] = p_inputs.camera_up[i];
		prepare.cameraRight[i] = p_inputs.camera_right[i];
		prepare.cameraForward[i] = p_inputs.camera_forward[i];
	}
	result = ffx.dispatch(&context, &prepare.header);
	if (result != FFX_API_RETURN_OK) {
		ERR_PRINT_ONCE(vformat("AMD FSR Frame Generation: could not prepare the frame (error %d).", result));
	}
}

void FSRFrameGenerationVulkan::before_present(SwapChain &p_swap_chain) {
	if (!p_swap_chain.prepared && p_swap_chain.frame_generation_context != nullptr) {
		_configure_disabled(p_swap_chain);
	}
	p_swap_chain.prepared = false;
}

#endif // WINDOWS_ENABLED
