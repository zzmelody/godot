// Reverse-Z and the corresponding source identity are one atomic update.
layout(set = 0, binding = 6, std430) buffer Visibility { uint64_t data[]; } visibility;

void vg_write_pixel(ivec2 pixel, float depth, uint payload) {
	if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, ivec2(raster_info.raster.xy)))) { return; }
	// Clamp can preserve -0. Its sign bit would sort above every positive
	// reverse-Z depth in the unsigned key. Canonicalize the zero sign.
	uint depth_bits = floatBitsToUint(clamp(depth, 0.0, 1.0)) & 0x7fffffffu;
	uint64_t value = (uint64_t(depth_bits) << 32) | uint64_t(payload);
	atomicMax(visibility.data[uint(pixel.y) * raster_info.raster.x + uint(pixel.x)], value);
}
