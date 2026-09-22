

#if defined(GODOT_USE_TRACY)

#include <tracy/TracyC.h>

#if defined(_WIN32)
#define VEYA_TRACY_EXPORT extern "C" __declspec(dllexport)
#else
#define VEYA_TRACY_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct VeyaTracyZoneContext {
	uint32_t id = 0;
	int32_t active = 0;
};

struct VeyaTracySourceLocation {
	const char *name;
	const char *function;
	const char *file;
	uint32_t line;
	uint32_t color;
};

static_assert(sizeof(VeyaTracyZoneContext) == sizeof(TracyCZoneCtx));
static_assert(sizeof(VeyaTracySourceLocation) == sizeof(___tracy_source_location_data));
static_assert(alignof(VeyaTracySourceLocation) == alignof(___tracy_source_location_data));

VEYA_TRACY_EXPORT VeyaTracyZoneContext veya_tracy_v1_zone_begin(const VeyaTracySourceLocation *p_source_location, int32_t p_callstack_depth) {
	const auto *source_location = reinterpret_cast<const ___tracy_source_location_data *>(p_source_location);
	const TracyCZoneCtx context = p_callstack_depth > 0
			? ___tracy_emit_zone_begin_callstack(source_location, p_callstack_depth, 1)
			: ___tracy_emit_zone_begin(source_location, 1);
	return { context.id, context.active };
}

VEYA_TRACY_EXPORT void veya_tracy_v1_zone_end(VeyaTracyZoneContext p_context) {
	___tracy_emit_zone_end({ p_context.id, p_context.active });
}

VEYA_TRACY_EXPORT void veya_tracy_v1_zone_text(VeyaTracyZoneContext p_context, const char *p_text, size_t p_size) {
	___tracy_emit_zone_text({ p_context.id, p_context.active }, p_text, p_size);
}

VEYA_TRACY_EXPORT void veya_tracy_v1_zone_value(VeyaTracyZoneContext p_context, uint64_t p_value) {
	___tracy_emit_zone_value({ p_context.id, p_context.active }, p_value);
}

VEYA_TRACY_EXPORT void veya_tracy_v1_plot_int(const char *p_name, int64_t p_value) {
	___tracy_emit_plot_int(p_name, p_value);
}

VEYA_TRACY_EXPORT int32_t veya_tracy_v1_is_connected() {
	return ___tracy_connected();
}

#endif
