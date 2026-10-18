#pragma once

#include <stdint.h>

// Veya's external temporal-upscaler ABI. Provider callbacks run on the render
// thread. Registration completes during extension initialization, before frames.
// Handles are borrowed. record() may only append commands to command_buffer.
// It must restore input layouts and leave output in GENERAL. A nonzero return
// aborts submission. destroy() runs only after the last GPU use has completed.
#define VEYA_TEMPORAL_ABI_VERSION 1u
#define VEYA_TEMPORAL_IMAGE_COUNT 6u

typedef struct VeyaTemporalImage {
	uint64_t rid;
	uint64_t image;
	uint32_t format;
	uint32_t width;
	uint32_t height;
} VeyaTemporalImage;

typedef struct VeyaTemporalConfig {
	uint32_t struct_size;
	uint32_t render_width;
	uint32_t render_height;
	uint32_t output_width;
	uint32_t output_height;
	uint64_t device;
	uint64_t physical_device;
} VeyaTemporalConfig;

typedef struct VeyaTemporalFrame {
	uint32_t struct_size;
	uint32_t reset;
	uint64_t sequence;
	uint64_t command_buffer;
	float jitter_x;
	float jitter_y;
	float near_meters;
	float far_meters;
	float vertical_fov_radians;
	float delta_seconds;
	float sharpness;
	float reprojection[16];
	// color, depth, motion, reactive, composition, output. prepare() may replace
	// depth/motion/reactive/composition RIDs with its RD-owned conversion images.
	VeyaTemporalImage images[VEYA_TEMPORAL_IMAGE_COUNT];
} VeyaTemporalFrame;

typedef struct VeyaTemporalProvider {
	uint32_t abi_version;
	uint32_t struct_size;
	int32_t (*resolve_size)(uint32_t width, uint32_t height, float scale, uint32_t *render_width, uint32_t *render_height);
	void *(*create)(const VeyaTemporalConfig *config);
	void (*destroy)(void *context);
	int32_t (*jitter)(void *context, uint64_t sequence, float *x, float *y);
	int32_t (*prepare)(void *context, VeyaTemporalFrame *frame);
	int32_t (*record)(void *context, const VeyaTemporalFrame *frame);
} VeyaTemporalProvider;

// Resolve once through GDExtensionInterfaceGetProcAddress using
// "veya_temporal_upscaler_register_v1". Passing null drains GPU work, destroys
// all provider contexts and unregisters before the extension can unload.
typedef uint32_t (*VeyaTemporalRegister)(const VeyaTemporalProvider *provider);

// "veya_temporal_upscaler_reset_v1" queues a reset for this viewport before
// its next render. The RID is copied; no scene object is retained.
typedef void (*VeyaTemporalReset)(uint64_t viewport_rid);
