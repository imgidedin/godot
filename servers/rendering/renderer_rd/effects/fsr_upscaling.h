/**************************************************************************/
/*  fsr_upscaling.h                                                       */
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

#if defined(VULKAN_ENABLED) && defined(WINDOWS_ENABLED)
#define FSR_UPSCALING_ENABLED
#endif

#ifdef FSR_UPSCALING_ENABLED

#include "core/templates/local_vector.h"
#include "core/templates/paged_allocator.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// AMD FSR Upscaling (FSR 4 or FSR 3.1, whichever the GPU runs) through the AMD FSR API, recorded into
// Godot's command buffer from a driver callback. The API itself is in AMD's DLLs, loaded by the
// Vulkan driver (drivers/vulkan/fsr_upscaling_vulkan.h).
class FSRUpscalingContext {
	friend class FSRUpscalingEffect;
	void *ffx_context = nullptr; // ffxContext.
};

class FSRUpscalingEffect {
	struct CallbackArgs;

	PagedAllocator<CallbackArgs, true, 16> args_allocator;

	// A context can only be destroyed once the GPU is done with the frames that used it.
	struct PendingFree {
		FSRUpscalingContext *context = nullptr;
		uint64_t frame = 0;
	};
	LocalVector<PendingFree> pending_frees;

	void _destroy_context(FSRUpscalingContext *p_context);
	void _process_pending_frees(bool p_all);

	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	FSRUpscalingContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	// Destroyed once the frames in flight that may use it are done (see process_pending_frees()).
	void free_context(FSRUpscalingContext *p_context);
	// Once per rendered frame: destroys the freed contexts the GPU is done with.
	void process_pending_frees() { _process_pending_frees(false); }

	struct Parameters {
		FSRUpscalingContext *context = nullptr;
		Size2i internal_size;
		RID color;
		RID depth; // R32_SFLOAT, reversed Z (the runtime cannot view a depth-stencil image as D32_SFLOAT).
		RID velocity;
		RID output;
		Vector2 jitter; // In pixels.
		float sharpness = 0.0f; // 0 (off) to 1.
		float delta_time = 0.0f; // In seconds.
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f; // In radians.
		bool reset = false;
	};

	void upscale(const Parameters &p_params);

	FSRUpscalingEffect();
	~FSRUpscalingEffect();
};

} // namespace RendererRD

#endif // FSR_UPSCALING_ENABLED
