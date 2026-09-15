#ifndef HEIGHT_FOG_INC_GLSL
#define HEIGHT_FOG_INC_GLSL

// Canonical linear radiance. CPU transport is RendererEnvironmentStorage::HeightFogData.
struct HeightFogData {
	vec4 density;
	vec4 control;
	vec4 source;
	vec4 options;
	vec4 direction0;
	vec4 color0;
	vec4 direction1;
	vec4 color1;
};

float height_fog_log_phi(float x) {
	if (x < 0.001) {
		return log(1.0 - x * 0.5 + x * x / 6.0 - x * x * x / 24.0);
	}
	return x > 50.0 ? -log(min(x, 1e30)) : log((1.0 - exp(-x)) / x);
}

// Use the denser endpoint and a positive phi argument; this avoids cancellation
// of two overflowing exponentials on long downward rays.
float height_fog_log_tau(HeightFogData fog, vec3 origin, vec3 ray, float distance_to_surface, bool infinite_ray) {
	float beta = fog.density.x;
	float falloff = fog.density.y;
	float start = fog.density.w;
	if (beta <= 0.0 || (!infinite_ray && distance_to_surface <= start)) {
		return -1e30;
	}
	if (falloff <= 0.0) {
		return infinite_ray ? 1e30 : log(beta) + log(distance_to_surface - start);
	}
	float start_height = origin.y + ray.y * start;
	float log_density = log(beta);
	float log_tau;
	if (infinite_ray) {
		if (falloff <= 0.0 || ray.y <= 0.0) {
			return 1e30;
		}
		log_tau = log_density - clamp(falloff * (start_height - fog.density.z), -1e30, 1e30) - log(falloff) - log(ray.y);
	} else {
		float remaining = distance_to_surface - start;
		float end_height = origin.y + ray.y * distance_to_surface;
		float dense_height = min(start_height, end_height);
		float x = min(falloff * abs(ray.y) * remaining, 1e30);
		log_tau = log_density - clamp(falloff * (dense_height - fog.density.z), -1e30, 1e30) + log(remaining) + height_fog_log_phi(x);
	}
	return log_tau;
}

// Returns accumulated in-scattering L and transmittance T. Optional capture
// scattering is disabled by the caller for every reflection capture view.
vec4 height_fog_integrate(HeightFogData fog, vec3 origin, vec3 ray, float distance_to_surface, bool infinite_ray, vec3 capture_color, bool capture_ready) {
	if (fog.control.w == 0.0 || fog.control.x == 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	float log_tau = height_fog_log_tau(fog, origin, ray, distance_to_surface, infinite_ray);
	float tau = exp(clamp(log_tau, -80.0, 69.0));
	float transmittance = max(exp(-tau), 1.0 - fog.control.x);
	float strength = capture_ready ? fog.source.w : 0.0;
	vec3 source = mix(fog.source.rgb, capture_color, strength);
	vec3 scattering = source * (1.0 - transmittance);
	float remaining = max(distance_to_surface - fog.density.w, 0.0);
	float directional_fraction = remaining > 0.0 ? max(1.0 - fog.control.y / remaining, 0.0) : 0.0;
	float directional_tau = infinite_ray ? tau : (directional_fraction > 0.0 ? exp(clamp(log_tau + log(directional_fraction), -80.0, 69.0)) : 0.0);
	float directional_opacity = (1.0 - exp(-directional_tau)) * (1.0 - strength);
	vec3 directional = fog.color0.rgb * pow(max(dot(ray, fog.direction0.xyz), 0.0), fog.control.z);
	directional += fog.color1.rgb * pow(max(dot(ray, fog.direction1.xyz), 0.0), fog.control.z);
	scattering += directional * directional_opacity * (1.0 / (4.0 * 3.141592653589793));
	return vec4(scattering, transmittance);
}

#endif
