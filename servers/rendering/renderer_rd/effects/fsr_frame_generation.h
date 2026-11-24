/**************************************************************************/
/*  fsr_frame_generation.h                                                */
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

#include "servers/rendering/renderer_rd/effects/fsr_upscaling.h" // FSR_UPSCALING_ENABLED: the same platforms.

#ifdef FSR_UPSCALING_ENABLED

#include "core/math/transform_3d.h"
#include "core/templates/paged_allocator.h"
#include "servers/display/display_server_enums.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// AMD FSR Frame Generation's inputs for one frame (the swap chain side is in the Vulkan driver,
// drivers/vulkan/fsr_frame_generation_vulkan.h), recorded into Godot's command buffer from a driver
// callback once the frame is rendered and before its UI.
class FSRFrameGenerationEffect {
	struct CallbackArgs;

	PagedAllocator<CallbackArgs, true, 16> args_allocator;

	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	struct Parameters {
		DisplayServerEnums::WindowID screen = DisplayServerEnums::INVALID_WINDOW_ID;
		Size2i internal_size;
		Size2i target_size;
		RID depth; // R32_SFLOAT, reversed Z.
		RID velocity;
		RID hudless; // The image before the UI, target size.
		Vector2 jitter; // In pixels.
		float delta_time = 0.0f; // In seconds.
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f; // In radians.
		Transform3D camera;
		bool reset = false;
	};

	void dispatch(const Parameters &p_params);

	FSRFrameGenerationEffect();
	~FSRFrameGenerationEffect();
};

} // namespace RendererRD

#endif // FSR_UPSCALING_ENABLED
