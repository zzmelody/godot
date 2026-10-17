#pragma once

#include "core/extension/veya_temporal_upscaler.h"
#include "core/templates/list.h"
#include "servers/rendering/rendering_device.h"

class ExternalTemporalUpscaler {
	static VeyaTemporalProvider provider;
	static List<ExternalTemporalUpscaler *> contexts;
	List<ExternalTemporalUpscaler *>::Element *element = nullptr;
	void *context = nullptr;
	uint64_t sequence = 0;
	bool reset = true;
	bool retiring = false;
	struct FrameSlot {
		ExternalTemporalUpscaler *owner = nullptr;
		VeyaTemporalFrame frame = {};
		bool pending = false;
	};
	FrameSlot slots[8];
	static void record(RenderingDeviceDriver *p_driver, RenderingDeviceDriver::CommandBufferID p_buffer, void *p_userdata);
	static void cancel(void *p_userdata);
	static void destroy(void *p_userdata);

public:
	static uint32_t register_provider(const VeyaTemporalProvider *p_provider);
	static bool available();
	static bool resolve_size(const Size2i &p_output, float p_scale, Size2i &r_render);
	static ExternalTemporalUpscaler *create(const Size2i &p_internal, const Size2i &p_output);
	bool get_jitter(Vector2 &r_jitter);
	void reset_history() {
		reset = true;
		sequence = 0;
	}
	Error process(VeyaTemporalFrame p_frame);
	void retire();
	~ExternalTemporalUpscaler();
};
