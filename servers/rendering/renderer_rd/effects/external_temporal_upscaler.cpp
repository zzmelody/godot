#include "external_temporal_upscaler.h"

#include "core/os/os.h"
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_device_driver_vulkan.h"
#endif

VeyaTemporalProvider ExternalTemporalUpscaler::provider = {};
List<ExternalTemporalUpscaler *> ExternalTemporalUpscaler::contexts;

uint32_t ExternalTemporalUpscaler::register_provider(const VeyaTemporalProvider *p_provider) {
	if (!p_provider) {
		if (RD::get_singleton() && !contexts.is_empty()) {
			// Unload is the only synchronous drain. Ordinary resize/disable uses
			// the frame's retirement list, without a per-frame device wait.
			RD::get_singleton()->_flush_and_stall_for_all_frames();
			RD::get_singleton()->_stall_for_previous_frames();
		}
		for (ExternalTemporalUpscaler *ctx : contexts) {
			if (ctx->context) {
				provider.destroy(ctx->context);
				ctx->context = nullptr;
			}
		}
		provider = {};
		return 1;
	}
	if (provider.create || p_provider->abi_version != VEYA_TEMPORAL_ABI_VERSION ||
			p_provider->struct_size != sizeof(VeyaTemporalProvider) || !p_provider->resolve_size || !p_provider->create ||
			!p_provider->destroy || !p_provider->jitter || !p_provider->prepare || !p_provider->record) {
		return 0;
	}
	provider = *p_provider;
	return 1;
}

bool ExternalTemporalUpscaler::available() {
#ifdef VULKAN_ENABLED
	return provider.create && RD::get_singleton() && OS::get_singleton()->get_current_rendering_driver_name() == "vulkan";
#else
	return false;
#endif
}

ExternalTemporalUpscaler *ExternalTemporalUpscaler::create(const Size2i &p_internal, const Size2i &p_output) {
	ERR_FAIL_COND_V(!available(), nullptr);
	RD *rd = RD::get_singleton();
	VeyaTemporalConfig config = {};
	config.struct_size = sizeof(config);
	config.render_width = p_internal.x;
	config.render_height = p_internal.y;
	config.output_width = p_output.x;
	config.output_height = p_output.y;
	config.device = rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	config.physical_device = rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	void *candidate = provider.create(&config);
	ERR_FAIL_NULL_V_MSG(candidate, nullptr, "External temporal upscaler context creation failed.");
	ExternalTemporalUpscaler *result = memnew(ExternalTemporalUpscaler);
	result->context = candidate;
	result->element = contexts.push_back(result);
	return result;
}

bool ExternalTemporalUpscaler::resolve_size(const Size2i &p_output, float p_scale, Size2i &r_render) {
	if (!available()) {
		return false;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	if (provider.resolve_size(p_output.x, p_output.y, p_scale, &width, &height) != 0 ||
			width == 0 || height == 0 || width > uint32_t(p_output.x) || height > uint32_t(p_output.y)) {
		return false;
	}
	r_render = Size2i(width, height);
	return true;
}

bool ExternalTemporalUpscaler::get_jitter(Vector2 &r_jitter) {
	ERR_FAIL_COND_V(!context || retiring, false);
	float x = 0;
	float y = 0;
	if (provider.jitter(context, sequence, &x, &y) != 0) {
		return false;
	}
	r_jitter = Vector2(x, y);
	return true;
}

Error ExternalTemporalUpscaler::process(VeyaTemporalFrame p_frame) {
	ERR_FAIL_COND_V(!context || retiring, ERR_UNAVAILABLE);
	FrameSlot *slot = nullptr;
	for (FrameSlot &candidate : slots) {
		if (!candidate.pending) {
			slot = &candidate;
			break;
		}
	}
	ERR_FAIL_NULL_V_MSG(slot, ERR_BUSY, "External upscaler has too many unrecorded frames.");
	p_frame.struct_size = sizeof(p_frame);
	p_frame.reset = reset;
	p_frame.sequence = sequence;
	ERR_FAIL_COND_V(provider.prepare(context, &p_frame) != 0, FAILED);
	RD *rd = RD::get_singleton();
	RD::CallbackResource resources[VEYA_TEMPORAL_IMAGE_COUNT];
	for (uint32_t i = 0; i < VEYA_TEMPORAL_IMAGE_COUNT; i++) {
		RID rid = RID::from_uint64(p_frame.images[i].rid);
		ERR_FAIL_COND_V(!rd->texture_is_valid(rid), ERR_INVALID_PARAMETER);
		RD::TextureFormat format = rd->texture_get_format(rid);
		ERR_FAIL_COND_V(format.texture_type != RD::TEXTURE_TYPE_2D || format.array_layers != 1 || format.samples != RD::TEXTURE_SAMPLES_1, ERR_INVALID_PARAMETER);
		p_frame.images[i].image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, rid);
		p_frame.images[i].format = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, rid);
		p_frame.images[i].width = format.width;
		p_frame.images[i].height = format.height;
		resources[i].rid = rid;
		resources[i].usage = i == 5 ? RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE : RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	}
	slot->owner = this;
	slot->frame = p_frame;
	slot->pending = true;
	Error error = rd->driver_callback_add(record, slot, VectorView<RD::CallbackResource>(resources, VEYA_TEMPORAL_IMAGE_COUNT), cancel);
	if (error != OK) {
		slot->pending = false;
		return error;
	}
	sequence++;
	reset = false;
	return OK;
}

void ExternalTemporalUpscaler::record(RenderingDeviceDriver *p_driver, RenderingDeviceDriver::CommandBufferID p_buffer, void *p_userdata) {
	FrameSlot *slot = static_cast<FrameSlot *>(p_userdata);
#ifdef VULKAN_ENABLED
	RenderingDeviceDriverVulkan *driver = static_cast<RenderingDeviceDriverVulkan *>(p_driver);
	slot->frame.command_buffer = driver->get_native_command_buffer(p_buffer);
	int32_t result = provider.record(slot->owner->context, &slot->frame);
	slot->pending = false;
	// A partially recorded SDK dispatch cannot be replaced with another
	// algorithm during graph replay. Never submit a corrupt frame as success.
	CRASH_COND_MSG(result != 0, "External temporal upscaler recording failed; frame submission aborted.");
#endif
}

void ExternalTemporalUpscaler::cancel(void *p_userdata) {
	FrameSlot *slot = static_cast<FrameSlot *>(p_userdata);
	slot->pending = false;
	slot->owner->reset = true;
}

void ExternalTemporalUpscaler::destroy(void *p_userdata) {
	memdelete(static_cast<ExternalTemporalUpscaler *>(p_userdata));
}

void ExternalTemporalUpscaler::retire() {
	ERR_FAIL_COND(retiring);
	retiring = true;
	RD::get_singleton()->defer_native_destroy(destroy, this);
}

ExternalTemporalUpscaler::~ExternalTemporalUpscaler() {
	if (context) {
		provider.destroy(context);
	}
	if (element) {
		contexts.erase(element);
	}
}
