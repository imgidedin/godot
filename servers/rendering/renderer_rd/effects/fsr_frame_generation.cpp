/**************************************************************************/
/*  fsr_frame_generation.cpp                                              */
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

#include "fsr_frame_generation.h"

#ifdef FSR_UPSCALING_ENABLED

#include "drivers/vulkan/rendering_device_driver_vulkan.h"

#include <thirdparty/amd-ffx-api/api/include/vk/ffx_api_vk.h>

using namespace RendererRD;

struct FSRFrameGenerationEffect::CallbackArgs {
	FSRFrameGenerationEffect *owner = nullptr;
	DisplayServerEnums::WindowID screen = DisplayServerEnums::INVALID_WINDOW_ID;
	FSRFrameGenerationVulkan::FrameInputs inputs;
};

static void _set_vector(float *r_to, const Vector3 &p_from) {
	r_to[0] = p_from.x;
	r_to[1] = p_from.y;
	r_to[2] = p_from.z;
}

void FSRFrameGenerationEffect::dispatch(const Parameters &p_params) {
	RD *rd = RD::get_singleton();
	CallbackArgs *args = args_allocator.alloc();
	args->owner = this;
	args->screen = p_params.screen;
	FSRFrameGenerationVulkan::FrameInputs &inputs = args->inputs;
	inputs = FSRFrameGenerationVulkan::FrameInputs();
	inputs.depth = (VkImage)rd->get_driver_resource(RD::DRIVER_RESOURCE_VULKAN_IMAGE, p_params.depth);
	inputs.motion_vectors = (VkImage)rd->get_driver_resource(RD::DRIVER_RESOURCE_VULKAN_IMAGE, p_params.velocity);
	inputs.hudless = (VkImage)rd->get_driver_resource(RD::DRIVER_RESOURCE_VULKAN_IMAGE, p_params.hudless);
	inputs.depth_format = ffxApiGetSurfaceFormatVK((VkFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.depth));
	inputs.motion_vectors_format = ffxApiGetSurfaceFormatVK((VkFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.velocity));
	inputs.hudless_format = ffxApiGetSurfaceFormatVK((VkFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.hudless));
	inputs.render_width = p_params.internal_size.width;
	inputs.render_height = p_params.internal_size.height;
	inputs.display_width = p_params.target_size.width;
	inputs.display_height = p_params.target_size.height;
	inputs.jitter_x = p_params.jitter.x;
	inputs.jitter_y = p_params.jitter.y;
	inputs.frame_time_delta = p_params.delta_time;
	inputs.z_near = p_params.z_near;
	inputs.z_far = p_params.z_far;
	inputs.fovy = p_params.fovy;
	_set_vector(inputs.camera_position, p_params.camera.origin);
	_set_vector(inputs.camera_up, p_params.camera.basis.get_column(1).normalized());
	_set_vector(inputs.camera_right, p_params.camera.basis.get_column(0).normalized());
	_set_vector(inputs.camera_forward, -p_params.camera.basis.get_column(2).normalized());
	inputs.reset = p_params.reset;

	// The render graph puts them in SHADER_READ_ONLY_OPTIMAL (COMPUTE_READ) before the callback runs.
	RD::CallbackResource resources[3] = {
		{ p_params.depth, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.velocity, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.hudless, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
	};
	rd->driver_callback_add((RDD::DriverCallback)FSRFrameGenerationEffect::callback, args, VectorView<RD::CallbackResource>(resources, 3));
}

void FSRFrameGenerationEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	// The window's swap chain now (none if the window was closed since the frame was recorded).
	const RDD::SwapChainID swap_chain = RD::get_singleton()->screen_get_driver_swap_chain(p_userdata->screen);
	if (swap_chain.id != 0) {
		static_cast<RenderingDeviceDriverVulkan *>(p_driver)->swap_chain_dispatch_frame_generation(swap_chain, p_command_buffer, p_userdata->inputs);
	}
	p_userdata->owner->args_allocator.free(p_userdata);
}

FSRFrameGenerationEffect::FSRFrameGenerationEffect() {}

FSRFrameGenerationEffect::~FSRFrameGenerationEffect() {}

#endif // FSR_UPSCALING_ENABLED
