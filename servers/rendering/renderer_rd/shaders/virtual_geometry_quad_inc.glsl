// Material work uses physical subgroup quads, never an assumed workgroup lane
// order. Every invocation must call vg_quad_address before any early return.
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_quad : require

shared uint vg_subgroup_quads[64];
shared uint vg_quad_error;

bool vg_quad_address(out uint quad, out uint lane) {
	if (gl_LocalInvocationIndex == 0u) {
		vg_quad_error = 0u;
	}
	barrier();
	uvec4 active_mask = subgroupBallot(true);
	uint count = subgroupBallotBitCount(active_mask);
	uint slot = gl_SubgroupInvocationID >> 5u;
	uint nibble = (active_mask[slot] >> (gl_SubgroupInvocationID & 28u)) & 15u;
	if (nibble != 15u) {
		atomicOr(vg_quad_error, 1u);
	}
	if (subgroupElect()) {
		vg_subgroup_quads[gl_SubgroupID] = count / 4u;
	}
	barrier();
	uint prefix = 0u;
	uint total = 0u;
	for (uint index = 0u; index < gl_NumSubgroups; ++index) {
		if (index < gl_SubgroupID) {
			prefix += vg_subgroup_quads[index];
		}
		total += vg_subgroup_quads[index];
	}
	lane = gl_SubgroupInvocationID & 3u;
	quad = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 16u + prefix + subgroupBallotExclusiveBitCount(active_mask) / 4u;
	return vg_quad_error == 0u && total == 16u;
}

#define VG_QUAD_DERIVATIVES(T) \
	T vg_dFdxFine(T value) { \
		T other = subgroupQuadSwapHorizontal(value); \
		return (gl_SubgroupInvocationID & 1u) != 0u ? value - other : other - value; \
	} \
	T vg_dFdyFine(T value) { \
		T other = subgroupQuadSwapVertical(value); \
		return (gl_SubgroupInvocationID & 2u) != 0u ? value - other : other - value; \
	} \
	T vg_dFdxCoarse(T value) { \
		return subgroupQuadBroadcast(value, 1u) - subgroupQuadBroadcast(value, 0u); \
	} \
	T vg_dFdyCoarse(T value) { \
		return subgroupQuadBroadcast(value, 2u) - subgroupQuadBroadcast(value, 0u); \
	} \
	T vg_fwidth(T value) { \
		return abs(vg_dFdxFine(value)) + abs(vg_dFdyFine(value)); \
	} \
	T vg_fwidthCoarse(T value) { \
		return abs(vg_dFdxCoarse(value)) + abs(vg_dFdyCoarse(value)); \
	}

VG_QUAD_DERIVATIVES(float)
VG_QUAD_DERIVATIVES(vec2)
VG_QUAD_DERIVATIVES(vec3)
VG_QUAD_DERIVATIVES(vec4)
#undef VG_QUAD_DERIVATIVES

// Implicit fragment sampling must become explicit gradients in compute. Keep
// array layers out of the gradient. Explicit LOD/gradient calls stay unchanged.
#define VG_QUAD_SAMPLING
// The same generated helpers can be called by vertex() and fragment(). Vertex
// sampling has no screen derivatives, including during partial-quad motion work.
bool vg_vertex_evaluation = false;

#define VG_QUAD_TEXTURE(S, R, C, G) \
	R vg_texture(S image, C uv) { \
		if (vg_vertex_evaluation) { return textureLod(image, uv, 0.0); } \
		return textureGrad(image, uv, vg_dFdxFine(G), vg_dFdyFine(G)); \
	} \
	R vg_texture(S image, C uv, float bias) { \
		if (vg_vertex_evaluation) { return textureLod(image, uv, 0.0); } \
		float scale = exp2(bias); \
		return textureGrad(image, uv, vg_dFdxFine(G) * scale, vg_dFdyFine(G) * scale); \
	}

VG_QUAD_TEXTURE(sampler2D, vec4, vec2, uv)
VG_QUAD_TEXTURE(sampler2DArray, vec4, vec3, uv.xy)
VG_QUAD_TEXTURE(sampler3D, vec4, vec3, uv)
VG_QUAD_TEXTURE(samplerCube, vec4, vec3, uv)
VG_QUAD_TEXTURE(samplerCubeArray, vec4, vec4, uv.xyz)
VG_QUAD_TEXTURE(isampler2D, ivec4, vec2, uv)
VG_QUAD_TEXTURE(isampler2DArray, ivec4, vec3, uv.xy)
VG_QUAD_TEXTURE(isampler3D, ivec4, vec3, uv)
VG_QUAD_TEXTURE(usampler2D, uvec4, vec2, uv)
VG_QUAD_TEXTURE(usampler2DArray, uvec4, vec3, uv.xy)
VG_QUAD_TEXTURE(usampler3D, uvec4, vec3, uv)
#undef VG_QUAD_TEXTURE

// Vulkan GLSL permits a constructed sampler only at the texture instruction.
// The spatial compiler therefore passes the texture and sampler separately.
#define VG_QUAD_SEPARATE_TEXTURE(T, S, R, C, G) \
	R vg_texture(T image, sampler sampling, C uv) { \
		if (vg_vertex_evaluation) { return textureLod(S(image, sampling), uv, 0.0); } \
		return textureGrad(S(image, sampling), uv, vg_dFdxFine(G), vg_dFdyFine(G)); \
	} \
	R vg_texture(T image, sampler sampling, C uv, float bias) { \
		if (vg_vertex_evaluation) { return textureLod(S(image, sampling), uv, 0.0); } \
		float scale = exp2(bias); \
		return textureGrad(S(image, sampling), uv, vg_dFdxFine(G) * scale, vg_dFdyFine(G) * scale); \
	}

VG_QUAD_SEPARATE_TEXTURE(texture2D, sampler2D, vec4, vec2, uv)
VG_QUAD_SEPARATE_TEXTURE(texture2DArray, sampler2DArray, vec4, vec3, uv.xy)
VG_QUAD_SEPARATE_TEXTURE(texture3D, sampler3D, vec4, vec3, uv)
VG_QUAD_SEPARATE_TEXTURE(textureCube, samplerCube, vec4, vec3, uv)
VG_QUAD_SEPARATE_TEXTURE(textureCubeArray, samplerCubeArray, vec4, vec4, uv.xyz)
VG_QUAD_SEPARATE_TEXTURE(itexture2D, isampler2D, ivec4, vec2, uv)
VG_QUAD_SEPARATE_TEXTURE(itexture2DArray, isampler2DArray, ivec4, vec3, uv.xy)
VG_QUAD_SEPARATE_TEXTURE(itexture3D, isampler3D, ivec4, vec3, uv)
VG_QUAD_SEPARATE_TEXTURE(utexture2D, usampler2D, uvec4, vec2, uv)
VG_QUAD_SEPARATE_TEXTURE(utexture2DArray, usampler2DArray, uvec4, vec3, uv.xy)
VG_QUAD_SEPARATE_TEXTURE(utexture3D, usampler3D, uvec4, vec3, uv)
#undef VG_QUAD_SEPARATE_TEXTURE

// Project before deriving gradients, then apply the optional mip bias. Keep
// all spatial-language float/signed/unsigned overloads in both sampler forms.
#define VG_QUAD_PROJECTED_TEXTURE(T, S, R, C, P) \
	R vg_textureProj(S image, C uv) { \
		return vg_texture(image, P); \
	} \
	R vg_textureProj(S image, C uv, float bias) { \
		return vg_texture(image, P, bias); \
	} \
	R vg_textureProj(T image, sampler sampling, C uv) { \
		return vg_texture(image, sampling, P); \
	} \
	R vg_textureProj(T image, sampler sampling, C uv, float bias) { \
		return vg_texture(image, sampling, P, bias); \
	}

VG_QUAD_PROJECTED_TEXTURE(texture2D, sampler2D, vec4, vec3, uv.xy / uv.z)
VG_QUAD_PROJECTED_TEXTURE(texture2D, sampler2D, vec4, vec4, uv.xy / uv.w)
VG_QUAD_PROJECTED_TEXTURE(itexture2D, isampler2D, ivec4, vec3, uv.xy / uv.z)
VG_QUAD_PROJECTED_TEXTURE(itexture2D, isampler2D, ivec4, vec4, uv.xy / uv.w)
VG_QUAD_PROJECTED_TEXTURE(utexture2D, usampler2D, uvec4, vec3, uv.xy / uv.z)
VG_QUAD_PROJECTED_TEXTURE(utexture2D, usampler2D, uvec4, vec4, uv.xy / uv.w)
VG_QUAD_PROJECTED_TEXTURE(texture3D, sampler3D, vec4, vec4, uv.xyz / uv.w)
VG_QUAD_PROJECTED_TEXTURE(itexture3D, isampler3D, ivec4, vec4, uv.xyz / uv.w)
VG_QUAD_PROJECTED_TEXTURE(utexture3D, usampler3D, uvec4, vec4, uv.xyz / uv.w)
#undef VG_QUAD_PROJECTED_TEXTURE
