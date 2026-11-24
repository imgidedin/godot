/**************************************************************************/
/*  fsr_upscaling.cpp                                                     */
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

#include "fsr_upscaling.h"

#ifdef FSR_UPSCALING_ENABLED

#include "core/os/os.h"
#include "core/string/print_string.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"

#include <thirdparty/amd-ffx-api/api/include/vk/ffx_api_vk.h>
#include <thirdparty/amd-ffx-api/upscalers/include/ffx_upscale.h>

using namespace RendererRD;

struct FSRUpscalingEffect::CallbackArgs {
	FSRUpscalingEffect *owner = nullptr;
	FSRUpscalingContext *context = nullptr;
	ffxDispatchDescUpscale dispatch = {};
};

static FSRUpscalingVulkan &_get_fsr_upscaling() {
	RenderingDeviceDriverVulkan *driver = static_cast<RenderingDeviceDriverVulkan *>(RD::get_singleton()->get_device_driver());
	return driver->get_fsr_upscaling();
}

static void _on_ffx_message(uint32_t p_type, const wchar_t *p_message) {
	const String message = String::utf16((const char16_t *)p_message);
	if (p_type == FFX_API_MESSAGE_TYPE_ERROR) {
		ERR_PRINT("AMD FSR Upscaling: " + message);
	} else {
		WARN_PRINT("AMD FSR Upscaling: " + message);
	}
}

// The resource as the AMD FSR API sees it: the VkImage, its size and format, and the state (layout)
// Godot's render graph leaves it in for the callback (see the usages in upscale()).
static FfxApiResource _get_resource(RID p_texture, uint32_t p_usage, uint32_t p_state) {
	RD *rd = RD::get_singleton();
	const RD::TextureFormat format = rd->texture_get_format(p_texture);

	FfxApiResourceDescription description = {};
	description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	description.format = ffxApiGetSurfaceFormatVK((VkFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture));
	description.width = format.width;
	description.height = format.height;
	description.depth = 1;
	description.mipCount = 1;
	description.flags = FFX_API_RESOURCE_FLAGS_NONE;
	description.usage = p_usage;

	return ffxApiGetResourceVK((void *)rd->get_driver_resource(RD::DRIVER_RESOURCE_VULKAN_IMAGE, p_texture), description, p_state);
}

FSRUpscalingContext *FSRUpscalingEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	_process_pending_frees(false);

	FSRUpscalingVulkan &fsr = _get_fsr_upscaling();
	ERR_FAIL_COND_V(!fsr.is_available(), nullptr);

	ffxCreateBackendVKDesc backend = {};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backend.deviceContext.vkDevice = fsr.get_device();
	backend.deviceContext.vkPhysicalDevice = fsr.get_physical_device();
	backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

	ffxCreateContextDescUpscale desc = {};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	desc.header.pNext = &backend.header;
	// Godot's 3D buffer is linear HDR before tonemapping (and before the camera exposure, so the
	// runtime estimates the exposure itself), with reversed Z.
	desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	if (OS::get_singleton()->is_stdout_verbose()) {
		// The runtime checks the dispatch parameters and reports problems through _on_ffx_message.
		desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
	}
	desc.maxRenderSize = { uint32_t(p_internal_size.width), uint32_t(p_internal_size.height) };
	desc.maxUpscaleSize = { uint32_t(p_target_size.width), uint32_t(p_target_size.height) };
	desc.fpMessage = _on_ffx_message;

	ffxContext ffx_context = nullptr;
	const ffxReturnCode_t result = fsr.get_functions().create_context(&ffx_context, &desc.header, nullptr);
	ERR_FAIL_COND_V_MSG(result != FFX_API_RETURN_OK, nullptr, vformat("AMD FSR Upscaling: could not create the upscaler (error %d).", result));

	ffxQueryGetProviderVersion version = {};
	version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
	if (fsr.get_functions().query(&ffx_context, &version.header) == FFX_API_RETURN_OK && version.versionName != nullptr) {
		print_verbose(vformat("AMD FSR Upscaling: %s, %dx%d to %dx%d.", version.versionName, p_internal_size.width, p_internal_size.height, p_target_size.width, p_target_size.height));
	}

	FSRUpscalingContext *context = memnew(FSRUpscalingContext);
	context->ffx_context = ffx_context;
	return context;
}

void FSRUpscalingEffect::_destroy_context(FSRUpscalingContext *p_context) {
	ffxContext ffx_context = p_context->ffx_context;
	_get_fsr_upscaling().get_functions().destroy_context(&ffx_context, nullptr);
	memdelete(p_context);
}

void FSRUpscalingEffect::free_context(FSRUpscalingContext *p_context) {
	if (p_context == nullptr) {
		return;
	}
	pending_frees.push_back({ p_context, RD::get_singleton()->get_frames_drawn() });
}

void FSRUpscalingEffect::_process_pending_frees(bool p_all) {
	const uint64_t frames_drawn = RD::get_singleton()->get_frames_drawn();
	const uint64_t frame_delay = RD::get_singleton()->get_frame_delay();
	uint32_t i = 0;
	while (i < pending_frees.size()) {
		if (p_all || frames_drawn > pending_frees[i].frame + frame_delay) {
			_destroy_context(pending_frees[i].context);
			pending_frees.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

void FSRUpscalingEffect::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(p_params.context);
	_process_pending_frees(false);

	CallbackArgs *args = args_allocator.alloc();
	args->owner = this;
	args->context = p_params.context;

	ffxDispatchDescUpscale &dispatch = args->dispatch;
	dispatch = {};
	dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dispatch.color = _get_resource(p_params.color, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	// Read-only, not DEPTHTARGET: that one makes the runtime view it as D32_SFLOAT, and this is R32_SFLOAT.
	dispatch.depth = _get_resource(p_params.depth, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.motionVectors = _get_resource(p_params.velocity, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.output = _get_resource(p_params.output, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	dispatch.jitterOffset = { p_params.jitter.x, p_params.jitter.y };
	// Godot's motion vectors are in UV units, pointing to where the pixel was in the previous frame.
	dispatch.motionVectorScale = { float(p_params.internal_size.width), float(p_params.internal_size.height) };
	dispatch.renderSize = { uint32_t(p_params.internal_size.width), uint32_t(p_params.internal_size.height) };
	dispatch.upscaleSize = { dispatch.output.description.width, dispatch.output.description.height };
	dispatch.enableSharpening = p_params.sharpness > 0.0f;
	dispatch.sharpness = p_params.sharpness;
	dispatch.frameTimeDelta = p_params.delta_time * 1000.0f;
	dispatch.preExposure = 1.0f;
	dispatch.reset = p_params.reset;
	dispatch.cameraNear = p_params.z_near;
	dispatch.cameraFar = p_params.z_far;
	dispatch.cameraFovAngleVertical = p_params.fovy;
	dispatch.viewSpaceToMetersFactor = 1.0f;

	// The render graph puts each texture in the layout of its usage before the callback runs:
	// sampled textures in SHADER_READ_ONLY_OPTIMAL (COMPUTE_READ), storage in GENERAL (UNORDERED_ACCESS).
	RD::CallbackResource resources[4] = {
		{ p_params.color, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.depth, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.velocity, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.output, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
	};
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)FSRUpscalingEffect::callback, args, VectorView<RD::CallbackResource>(resources, 4));
}

void FSRUpscalingEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	p_userdata->dispatch.commandList = RenderingDeviceDriverVulkan::command_buffer_get_vk(p_command_buffer);
	ffxContext ffx_context = p_userdata->context->ffx_context;
	const ffxReturnCode_t result = _get_fsr_upscaling().get_functions().dispatch(&ffx_context, &p_userdata->dispatch.header);
	if (result != FFX_API_RETURN_OK) {
		ERR_PRINT_ONCE(vformat("AMD FSR Upscaling: dispatch failed (error %d).", result));
	}
	p_userdata->owner->args_allocator.free(p_userdata);
}

FSRUpscalingEffect::FSRUpscalingEffect() {}

FSRUpscalingEffect::~FSRUpscalingEffect() {
	if (!pending_frees.is_empty()) {
		// Shutting down: no more frames will be drawn to retire them.
		vkDeviceWaitIdle(_get_fsr_upscaling().get_device());
		_process_pending_frees(true);
	}
}

#endif // FSR_UPSCALING_ENABLED
