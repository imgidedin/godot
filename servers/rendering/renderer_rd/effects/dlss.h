/**************************************************************************/
/*  dlss.h                                                                */
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
#define DLSS_ENABLED
#endif

#ifdef DLSS_ENABLED

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_allocator.h"
#include "servers/display/display_server_enums.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// NVIDIA DLSS Super Resolution through NVIDIA Streamline (loaded by the Vulkan driver,
// drivers/vulkan/dlss_vulkan.h), recorded into Godot's command buffer from a driver callback.
// One context per render buffers: a Streamline viewport.
class DLSSContext {
	friend class DLSSEffect;
	uint32_t viewport = 0; // sl::ViewportHandle.
	uint32_t mode = 0; // sl::DLSSMode.
	int preset = 0; // Index of rendering/scaling_3d/dlss_preset the options were set with.
	Size2i target_size;
};

class DLSSEffect {
	struct CallbackArgs;
	struct FrameGenerationArgs;

	PagedAllocator<CallbackArgs, true, 16> args_allocator;
	PagedAllocator<FrameGenerationArgs, true, 16> frame_generation_args_allocator;

	uint32_t next_viewport = 1;

	// A viewport's resources can only go once the GPU is done with the frames that used them.
	struct PendingFree {
		DLSSContext *context = nullptr;
		uint64_t frame = 0;
	};
	LocalVector<PendingFree> pending_frees;
	void _process_pending_frees(bool p_all);

	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);
	static void frame_generation_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, FrameGenerationArgs *p_userdata);

public:
	// The DLSS quality mode whose render size range holds p_internal_size (DLAA at native size).
	DLSSContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	void free_context(DLSSContext *p_context);
	// Once per rendered frame: frees the viewports the GPU is done with.
	void process_pending_frees() { _process_pending_frees(false); }

	struct Parameters {
		DLSSContext *context = nullptr;
		Size2i internal_size;
		RID color;
		RID depth;
		RID velocity;
		RID output;
		Vector2 jitter; // In pixels.
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f; // In radians.
		// Camera, without jitter; projections already in Vulkan's clip space.
		Projection projection;
		Projection prev_projection;
		Transform3D transform;
		Transform3D prev_transform;
		bool reset = false;
	};

	void upscale(const Parameters &p_params);

	// DLSS Frame Generation from this frame, after upscale() (with its camera constants): the inputs
	// are tagged until the present, where it generates the frame before this one.
	struct FrameGenerationParameters {
		DisplayServerEnums::WindowID screen = DisplayServerEnums::INVALID_WINDOW_ID;
		DLSSContext *context = nullptr;
		Size2i internal_size;
		RID depth;
		RID velocity;
		RID hudless; // The image before the UI, target size.
	};

	void generate_frames(const FrameGenerationParameters &p_params);

	DLSSEffect();
	~DLSSEffect();
};

} // namespace RendererRD

#endif // DLSS_ENABLED
