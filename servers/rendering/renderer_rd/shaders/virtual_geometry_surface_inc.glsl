// Static surface input reconstruction. The caller provides geometry and scene.
// Material evaluation, deformation, previous positions and output are separate.
#include "virtual_geometry_attributes_inc.glsl"

struct VGBarycentrics {
	vec3 value;
	vec3 dx;
	vec3 dy;
	bool front_facing;
};

bool vg_finite(vec3 value) {
	return !any(isnan(value)) && !any(isinf(value));
}

// Solve the two homogeneous screen equations before dividing by their sum.
// This equals perspective interpolation, including an original corner at W=0.
// Pixel coordinates use Godot's increasing framebuffer Y, as does vg_screen.
bool vg_barycentrics(vec4 c0, vec4 c1, vec4 c2, vec2 pixel, vec2 size, inout VGBarycentrics result) {
	if (any(lessThanEqual(size, vec2(0.0)))) {
		return false;
	}
	vec3 x = vec3(c0.x, c1.x, c2.x);
	vec3 y = vec3(c0.y, c1.y, c2.y);
	vec3 w = vec3(c0.w, c1.w, c2.w);
	float scale = max(max(max(abs(x.x), abs(x.y)), abs(x.z)), max(max(max(abs(y.x), abs(y.y)), abs(y.z)), max(max(abs(w.x), abs(w.y)), abs(w.z))));
	if (scale == 0.0 || isnan(scale) || isinf(scale)) {
		return false;
	}
	x /= scale;
	y /= scale;
	w /= scale;
	vec2 ndc = pixel / size * 2.0 - 1.0;
	vec3 numerators = cross(x - ndc.x * w, y - ndc.y * w);
	float denominator = dot(numerators, vec3(1.0));
	if (denominator == 0.0 || isnan(denominator) || isinf(denominator)) {
		return false;
	}
	VGBarycentrics candidate;
	candidate.value = numerators / denominator;
	vec3 nx = -cross(w, y) * (2.0 / size.x);
	vec3 ny = -cross(x, w) * (2.0 / size.y);
	candidate.dx = (nx - candidate.value * dot(nx, vec3(1.0))) / denominator;
	candidate.dy = (ny - candidate.value * dot(ny, vec3(1.0))) / denominator;
	candidate.front_facing = dot(cross(x, y), w) > 0.0;
	if (!vg_finite(candidate.value) || !vg_finite(candidate.dx) || !vg_finite(candidate.dy)) {
		return false;
	}
	result = candidate;
	return true;
}

struct VGSurface {
	VGBarycentrics barycentrics;
	uint source;
	vec3 local_position;
	vec3 world_position;
	vec3 view_position;
	vec3 world_dx;
	vec3 world_dy;
	vec3 normal;
	vec3 tangent;
	vec3 binormal;
	vec4 color;
	vec2 uv[4];
	vec2 uv_dx[4];
	vec2 uv_dy[4];
	float depth;
	float reciprocal_w;
};

vec3 vg_frame_tangent(vec3 normal) {
	float side = normal.z >= 0.0 ? 1.0 : -1.0;
	float divisor = 1.0 + side * normal.z;
	return vec3(1.0 - normal.x * normal.x / divisor, -normal.x * normal.y / divisor, -side * normal.x);
}

bool vg_surface(uint payload, vec2 pixel, mat4 projection, vec2 size, vec2 jitter, inout VGSurface result) {
	if ((payload >> 7u) == 0u) {
		return false;
	}
	uvec2 visible = vg_visible_reference((payload >> 7u) - 1u);
	uvec3 vertices = vg_triangle(visible.y, payload & 127u);
	VGVertex v[3];
	vec4 clips[3];
	mat4 model = vg_source_matrix(visible.x, 0u);
	mat4 model_view = vg_source_matrix(visible.x, 12u);
	for (uint corner = 0u; corner < 3u; ++corner) {
		v[corner] = vg_vertex(visible.y, vertices[corner]);
#ifdef VG_EVALUATE_VERTEX
		VG_EVALUATE_VERTEX(v[corner], visible.x, corner);
#endif
		vec4 clip = projection * model_view * vec4(v[corner].position, 1.0);
		clip.xy += jitter * clip.w;
		clips[corner] = vg_snap_clip(clip, size);
	}
	VGSurface candidate;
	if (!vg_barycentrics(clips[0], clips[1], clips[2], pixel, size, candidate.barycentrics)) {
		return false;
	}
	vec3 b = candidate.barycentrics.value;
	vec3 bx = candidate.barycentrics.dx;
	vec3 by = candidate.barycentrics.dy;
	vec3 local_normal = v[0].normal * b.x + v[1].normal * b.y + v[2].normal * b.z;
	if (dot(local_normal, local_normal) <= 1e-20) {
		return false;
	}
	local_normal = normalize(local_normal);
	vec3 local_tangent;
	float handedness = 1.0;
	uint record = vg_record(visible.y);
	if ((geometry.data[record + 35u] & 1u) != 0u) {
		vec4 tangent = v[0].tangent * b.x + v[1].tangent * b.y + v[2].tangent * b.z;
		local_tangent = tangent.xyz;
		handedness = tangent.w < 0.0 ? -1.0 : 1.0;
	} else {
		vec3 first = cross(local_normal, v[2].position - v[0].position);
		vec3 second = cross(v[1].position - v[0].position, local_normal);
		vec2 uv1 = v[1].uv[0] - v[0].uv[0];
		vec2 uv2 = v[2].uv[0] - v[0].uv[0];
		local_tangent = first * uv1.x + second * uv2.x;
		if (dot(local_tangent, local_tangent) > 1e-20) {
			vec3 local_binormal = cross(local_normal, normalize(local_tangent));
			handedness = dot(first * uv1.y + second * uv2.y, local_binormal) < 0.0 ? -1.0 : 1.0;
		}
	}
	if (dot(local_tangent, local_tangent) <= 1e-20) {
		local_tangent = vg_frame_tangent(local_normal);
	}
	candidate.normal = normalize(mat3(vg_source_matrix(visible.x, 24u)) * local_normal);
	vec3 world_tangent = mat3(model) * local_tangent;
	world_tangent -= candidate.normal * dot(candidate.normal, world_tangent);
	if (dot(world_tangent, world_tangent) <= 1e-20) {
		return false;
	}
	candidate.tangent = normalize(world_tangent);
	float orientation = uintBitsToFloat(scene.data[vg_source_word(visible.x) + 35u]);
	candidate.binormal = cross(candidate.normal, candidate.tangent) * handedness * orientation;
	candidate.source = visible.x;
	candidate.local_position = v[0].position * b.x + v[1].position * b.y + v[2].position * b.z;
	candidate.world_position = (model * vec4(candidate.local_position, 1.0)).xyz;
	candidate.view_position = (model_view * vec4(candidate.local_position, 1.0)).xyz;
	candidate.world_dx = mat3(model) * (v[0].position * bx.x + v[1].position * bx.y + v[2].position * bx.z);
	candidate.world_dy = mat3(model) * (v[0].position * by.x + v[1].position * by.y + v[2].position * by.z);
	candidate.color = v[0].color * b.x + v[1].color * b.y + v[2].color * b.z;
#ifndef VG_EVALUATE_VERTEX
	candidate.color *= vg_source_vector(visible.x, 36u);
#endif
	for (uint uv = 0u; uv < 4u; ++uv) {
		candidate.uv[uv] = v[0].uv[uv] * b.x + v[1].uv[uv] * b.y + v[2].uv[uv] * b.z;
		candidate.uv_dx[uv] = v[0].uv[uv] * bx.x + v[1].uv[uv] * bx.y + v[2].uv[uv] * bx.z;
		candidate.uv_dy[uv] = v[0].uv[uv] * by.x + v[1].uv[uv] * by.y + v[2].uv[uv] * by.z;
		if (!vg_finite(vec3(candidate.uv[uv], 0.0)) || !vg_finite(vec3(candidate.uv_dx[uv], 0.0)) || !vg_finite(vec3(candidate.uv_dy[uv], 0.0))) {
			return false;
		}
	}
	vec4 clip_position = clips[0] * b.x + clips[1] * b.y + clips[2] * b.z;
	candidate.depth = clip_position.z / clip_position.w;
	candidate.reciprocal_w = 1.0 / clip_position.w;
	if (!vg_finite(candidate.world_position) || !vg_finite(candidate.view_position) ||
			!vg_finite(candidate.world_dx) || !vg_finite(candidate.world_dy) || !vg_finite(candidate.normal) ||
			!vg_finite(candidate.tangent) || !vg_finite(candidate.binormal) ||
			any(isnan(candidate.color)) || any(isinf(candidate.color)) || isnan(candidate.depth) || isinf(candidate.depth) || isnan(candidate.reciprocal_w) || isinf(candidate.reciprocal_w)) {
		return false;
	}
	result = candidate;
	return true;
}
