/**************************************************************************/
/*  dlss.cpp                                                              */
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

#include "dlss.h"

#ifdef DLSS_ENABLED

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/string/print_string.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"

using namespace RendererRD;

struct DLSSEffect::CallbackArgs {
	DLSSEffect *owner = nullptr;
	sl::FrameToken *frame_token = nullptr;
	uint32_t viewport = 0;
	Size2i internal_size;
	Size2i target_size;
	RDD::TextureID color;
	RDD::TextureID depth;
	RDD::TextureID velocity;
	RDD::TextureID output;
};

static DLSSVulkan &_get_dlss() {
	RenderingDeviceDriverVulkan *driver = static_cast<RenderingDeviceDriverVulkan *>(RD::get_singleton()->get_device_driver());
	return driver->get_dlss();
}

static const char *_mode_name(sl::DLSSMode p_mode) {
	switch (p_mode) {
		case sl::DLSSMode::eDLAA:
			return "DLAA";
		case sl::DLSSMode::eMaxQuality:
			return "Quality";
		case sl::DLSSMode::eBalanced:
			return "Balanced";
		case sl::DLSSMode::eMaxPerformance:
			return "Performance";
		case sl::DLSSMode::eUltraPerformance:
			return "Ultra Performance";
		default:
			return "?";
	}
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	_process_pending_frees(false);

	DLSSVulkan &dlss = _get_dlss();
	ERR_FAIL_COND_V(!dlss.is_available(), nullptr);
	const DLSSVulkan::Functions &sl_functions = dlss.get_functions();

	sl::DLSSOptions options;
	options.outputWidth = p_target_size.width;
	options.outputHeight = p_target_size.height;

	// DLSS works per quality mode, each with an optimal render size and a range around it (the
	// ranges overlap): the mode whose optimal size is closest to the size Godot renders at (from the
	// 3D scale, clamped to DLSS's range in RendererViewport), if its range holds it. Native size is DLAA.
	const sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
	sl::DLSSMode chosen = sl::DLSSMode::eOff;
	sl::DLSSOptimalSettings chosen_settings;
	int64_t chosen_distance = INT64_MAX;
	for (sl::DLSSMode mode : modes) {
		options.mode = mode;
		sl::DLSSOptimalSettings settings;
		if (sl_functions.dlss_get_optimal_settings(options, settings) != sl::Result::eOk) {
			continue;
		}
		const int64_t distance = Math::abs((int64_t)settings.optimalRenderWidth - (int64_t)p_internal_size.width) + Math::abs((int64_t)settings.optimalRenderHeight - (int64_t)p_internal_size.height);
		if (distance < chosen_distance) {
			chosen_distance = distance;
			chosen = mode;
			chosen_settings = settings;
		}
	}
	ERR_FAIL_COND_V_MSG(chosen == sl::DLSSMode::eOff, nullptr, "DLSS: no quality mode for this output size.");
	const uint32_t width = p_internal_size.width;
	const uint32_t height = p_internal_size.height;
	const bool in_range = width >= MIN(chosen_settings.renderWidthMin, chosen_settings.optimalRenderWidth) && width <= MAX(chosen_settings.renderWidthMax, chosen_settings.optimalRenderWidth) && height >= MIN(chosen_settings.renderHeightMin, chosen_settings.optimalRenderHeight) && height <= MAX(chosen_settings.renderHeightMax, chosen_settings.optimalRenderHeight);
	ERR_FAIL_COND_V_MSG(!in_range, nullptr, vformat("DLSS: %dx%d is outside the render sizes of the %s mode for %dx%d.", p_internal_size.width, p_internal_size.height, _mode_name(chosen), p_target_size.width, p_target_size.height));
	options.mode = chosen;

	// The model (preset) for every mode: the project's choice, or NVIDIA's default per mode.
	static const sl::DLSSPreset presets[] = { sl::DLSSPreset::eDefault, sl::DLSSPreset::ePresetJ, sl::DLSSPreset::ePresetK, sl::DLSSPreset::ePresetL, sl::DLSSPreset::ePresetM };
	const int preset_index = CLAMP(int(GLOBAL_GET("rendering/scaling_3d/dlss_preset")), 0, int(std::size(presets)) - 1);
	const sl::DLSSPreset preset = presets[preset_index];
	options.dlaaPreset = preset;
	options.qualityPreset = preset;
	options.balancedPreset = preset;
	options.performancePreset = preset;
	options.ultraPerformancePreset = preset;
	options.ultraQualityPreset = preset;
	// Godot's 3D buffer is linear HDR before tonemapping and before the camera exposure.
	options.colorBuffersHDR = sl::Boolean::eTrue;
	options.useAutoExposure = sl::Boolean::eTrue;

	DLSSContext *context = memnew(DLSSContext);
	context->viewport = next_viewport++;
	context->mode = (uint32_t)chosen;
	context->target_size = p_target_size;

	const sl::Result result = sl_functions.dlss_set_options(sl::ViewportHandle(context->viewport), options);
	if (result != sl::Result::eOk) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, vformat("DLSS: could not set the options (Streamline error %d).", (int)result));
	}
	static const char *preset_names[] = { "default", "J", "K", "L", "M" };
	print_verbose(vformat("DLSS: %s, preset %s, %dx%d to %dx%d.", _mode_name(chosen), preset_names[preset_index], p_internal_size.width, p_internal_size.height, p_target_size.width, p_target_size.height));
	return context;
}

void DLSSEffect::free_context(DLSSContext *p_context) {
	if (p_context == nullptr) {
		return;
	}
	pending_frees.push_back({ p_context, RD::get_singleton()->get_frames_drawn() });
}

void DLSSEffect::_process_pending_frees(bool p_all) {
	if (pending_frees.is_empty()) {
		return;
	}
	const uint64_t frames_drawn = RD::get_singleton()->get_frames_drawn();
	const uint64_t frame_delay = RD::get_singleton()->get_frame_delay();
	uint32_t i = 0;
	while (i < pending_frees.size()) {
		if (p_all || frames_drawn > pending_frees[i].frame + frame_delay) {
			DLSSVulkan &dlss = _get_dlss();
			if (dlss.is_available()) {
				dlss.get_functions().free_resources(sl::kFeatureDLSS, sl::ViewportHandle(pending_frees[i].context->viewport));
			}
			memdelete(pending_frees[i].context);
			pending_frees.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

// Godot's matrices are column-major for column vectors; Streamline's are row-major for row vectors:
// the same numbers in the same order.
static sl::float4x4 _to_sl(const Projection &p_matrix) {
	sl::float4x4 matrix;
	for (int i = 0; i < 4; i++) {
		matrix.row[i] = sl::float4(p_matrix.columns[i].x, p_matrix.columns[i].y, p_matrix.columns[i].z, p_matrix.columns[i].w);
	}
	return matrix;
}

static sl::float3 _to_sl(const Vector3 &p_vector) {
	return sl::float3(p_vector.x, p_vector.y, p_vector.z);
}

void DLSSEffect::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(p_params.context);
	DLSSVulkan &dlss = _get_dlss();
	ERR_FAIL_COND(!dlss.is_available());
	const DLSSVulkan::Functions &sl_functions = dlss.get_functions();

	const uint64_t frames_drawn = RD::get_singleton()->get_frames_drawn();
	if (frame_token_frame != frames_drawn) {
		sl::FrameToken *token = nullptr;
		const uint32_t frame_index = uint32_t(frames_drawn);
		ERR_FAIL_COND_MSG(sl_functions.get_new_frame_token(token, &frame_index) != sl::Result::eOk, "DLSS: no frame token from Streamline.");
		frame_token = token;
		frame_token_frame = frames_drawn;
	}
	sl::FrameToken *token = (sl::FrameToken *)frame_token;
	const sl::ViewportHandle viewport(p_params.context->viewport);

	// Camera: current clip to the previous one, as FSR 2's reprojection in Godot.
	const Projection view_to_clip = p_params.projection;
	const Projection clip_to_view = view_to_clip.inverse();
	const Projection clip_to_prev_clip = p_params.prev_projection * Projection(p_params.prev_transform.affine_inverse() * p_params.transform) * clip_to_view;

	sl::Constants constants;
	constants.cameraViewToClip = _to_sl(view_to_clip);
	constants.clipToCameraView = _to_sl(clip_to_view);
	constants.clipToPrevClip = _to_sl(clip_to_prev_clip);
	constants.prevClipToClip = _to_sl(clip_to_prev_clip.inverse());
	constants.jitterOffset = sl::float2(p_params.jitter.x, p_params.jitter.y);
	// Godot's motion vectors are in UV units (a whole screen is 1), pointing to the previous position.
	constants.mvecScale = sl::float2(1.0f, 1.0f);
	constants.cameraPos = _to_sl(p_params.transform.origin);
	constants.cameraRight = _to_sl(p_params.transform.basis.get_column(0).normalized());
	constants.cameraUp = _to_sl(p_params.transform.basis.get_column(1).normalized());
	constants.cameraFwd = _to_sl(-p_params.transform.basis.get_column(2).normalized());
	constants.cameraNear = p_params.z_near;
	constants.cameraFar = p_params.z_far;
	constants.cameraFOV = p_params.fovy;
	constants.cameraAspectRatio = float(p_params.internal_size.width) / float(p_params.internal_size.height);
	constants.depthInverted = sl::Boolean::eTrue;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.motionVectors3D = sl::Boolean::eFalse;
	constants.reset = p_params.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	constants.orthographicProjection = p_params.projection.is_orthogonal() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	constants.motionVectorsDilated = sl::Boolean::eFalse;
	constants.motionVectorsJittered = sl::Boolean::eFalse;
	const sl::Result result = sl_functions.set_constants(constants, *token, viewport);
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, vformat("DLSS: could not set the camera constants (Streamline error %d).", (int)result));

	RD *rd = RD::get_singleton();
	CallbackArgs *args = args_allocator.alloc();
	args->owner = this;
	args->frame_token = token;
	args->viewport = p_params.context->viewport;
	args->internal_size = p_params.internal_size;
	args->target_size = p_params.context->target_size;
	args->color = rd->texture_get_driver_id(p_params.color);
	args->depth = rd->texture_get_driver_id(p_params.depth);
	args->velocity = rd->texture_get_driver_id(p_params.velocity);
	args->output = rd->texture_get_driver_id(p_params.output);

	// The render graph puts each texture in the layout of its usage before the callback runs:
	// sampled ones in SHADER_READ_ONLY_OPTIMAL, the storage one in GENERAL (see callback()).
	RD::CallbackResource resources[4] = {
		{ p_params.color, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.depth, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.velocity, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_params.output, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
	};
	rd->driver_callback_add((RDD::DriverCallback)DLSSEffect::callback, args, VectorView<RD::CallbackResource>(resources, 4));
}

static sl::Resource _get_resource(RDD::TextureID p_texture, VkImageLayout p_layout) {
	const RenderingDeviceDriverVulkan::TextureNativeInfo info = RenderingDeviceDriverVulkan::texture_get_native_info(p_texture);
	sl::Resource resource(sl::ResourceType::eTex2d, (void *)info.image, nullptr, (void *)info.view, (uint32_t)p_layout);
	resource.width = info.width;
	resource.height = info.height;
	resource.nativeFormat = (uint32_t)info.format;
	resource.mipLevels = info.mip_levels;
	resource.arrayLayers = info.array_layers;
	resource.flags = info.flags;
	resource.usage = info.usage;
	return resource;
}

void DLSSEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	sl::CommandBuffer *command_buffer = (sl::CommandBuffer *)RenderingDeviceDriverVulkan::command_buffer_get_vk(p_command_buffer);
	const DLSSVulkan::Functions &sl_functions = _get_dlss().get_functions();
	const sl::ViewportHandle viewport(p_userdata->viewport);

	sl::Resource color = _get_resource(p_userdata->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sl::Resource depth = _get_resource(p_userdata->depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sl::Resource velocity = _get_resource(p_userdata->velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sl::Resource output = _get_resource(p_userdata->output, VK_IMAGE_LAYOUT_GENERAL);
	const sl::Extent render_extent = { 0, 0, uint32_t(p_userdata->internal_size.width), uint32_t(p_userdata->internal_size.height) };
	const sl::Extent output_extent = { 0, 0, uint32_t(p_userdata->target_size.width), uint32_t(p_userdata->target_size.height) };
	// Valid until evaluated: they are used right below, so Streamline needs no copies.
	const sl::ResourceTag tags[4] = {
		sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent),
		sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent),
		sl::ResourceTag(&velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent),
		sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &output_extent),
	};

	sl::Result result = sl_functions.set_tag_for_frame(*p_userdata->frame_token, viewport, tags, 4, command_buffer);
	if (result == sl::Result::eOk) {
		const sl::BaseStructure *inputs[] = { &viewport };
		result = sl_functions.evaluate_feature(sl::kFeatureDLSS, *p_userdata->frame_token, inputs, 1, command_buffer);
	}
	if (result != sl::Result::eOk) {
		ERR_PRINT_ONCE(vformat("DLSS: evaluation failed (Streamline error %d).", (int)result));
	}
	p_userdata->owner->args_allocator.free(p_userdata);
}

DLSSEffect::DLSSEffect() {}

DLSSEffect::~DLSSEffect() {
	// Shutting down: Streamline frees what is left when it shuts down, with the device still alive.
	for (const PendingFree &pending : pending_frees) {
		memdelete(pending.context);
	}
	pending_frees.clear();
}

#endif // DLSS_ENABLED
