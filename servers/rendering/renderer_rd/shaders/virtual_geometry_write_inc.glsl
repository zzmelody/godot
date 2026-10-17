// Reverse-Z and the corresponding source identity are one atomic update.
layout(set = 0, binding = 6, std430) buffer Visibility { uint64_t data[]; } visibility;

void vg_write_pixel(ivec2 pixel, float depth, uint payload) {
	if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, ivec2(raster_info.raster.xy)))) { return; }
	uint64_t value = (uint64_t(floatBitsToUint(clamp(depth, 0.0, 1.0))) << 32) | uint64_t(payload);
	atomicMax(visibility.data[uint(pixel.y) * raster_info.raster.x + uint(pixel.x)], value);
}
