// Apply each completed map's capture orientation before octahedral encoding.
// Layer, derivative LOD and border semantics remain identical to native sampling.
vec4 sample_sky_radiance(vec3 direction, float layer, float lod) {
	uint fogged_maps = uint(scene_data_block.data.sky_capture_data.w);
	vec3 old_direction = (fogged_maps & 1u) != 0u ? scene_data_block.data.sky_capture_old_xform * direction : direction;
	vec2 border = vec2(scene_data_block.data.radiance_border_size, 1.0 - 2.0 * scene_data_block.data.radiance_border_size);
	vec2 old_uv = vec3_to_oct_with_border(old_direction, border);
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	vec4 old_sample = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(old_uv, layer), lod);
#else
	vec4 old_sample = textureLod(sampler2D(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), old_uv, lod);
#endif
	if (scene_data_block.data.sky_capture_data.x == 0.0) { return old_sample; }
	vec3 old_rgb = scene_data_block.data.sky_capture_data.z > 0.0 ? old_sample.rgb : scene_data_block.data.sky_capture_fallback.rgb;
	vec3 result = old_rgb;
	if (scene_data_block.data.sky_capture_data.y > 0.0) {
		vec3 next_direction = (fogged_maps & 2u) != 0u ? scene_data_block.data.sky_capture_next_xform * direction : direction;
		vec2 next_uv = vec3_to_oct_with_border(next_direction, border);
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		vec3 next_rgb = textureLod(sampler2DArray(radiance_octmap_next, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(next_uv, layer), lod).rgb;
#else
		vec3 next_rgb = textureLod(sampler2D(radiance_octmap_next, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), next_uv, lod).rgb;
#endif
		result = mix(old_rgb, next_rgb, scene_data_block.data.sky_capture_data.y);
	}
	return vec4(result, 1.0);
}

bool native_ap_enabled() {
#ifdef USE_MULTIVIEW
 return scene_data.aerial_perspective[ViewIndex] > 0.0;
#else
 return scene_data.aerial_perspective.x > 0.0;
#endif
}
vec4 native_ap_process(vec3 vertex) {
 vec3 origin_view=vec3(0.0);
#ifdef USE_MULTIVIEW
 origin_view=scene_data.eye_offset[ViewIndex].xyz;
#endif
 vec2 uv=gl_FragCoord.xy*scene_data.screen_pixel_size;
 if(scene_data.height_fog_view.x>0.0) {
  vec2 ndc=uv*2.0-1.0;
  vec4 a=inv_projection_matrix*vec4(ndc,1.0,1.0);
  vec4 b=inv_projection_matrix*vec4(ndc,0.5,1.0);
  vec3 near_p=a.xyz/a.w;
  vec3 ray=b.xyz/b.w-near_p;
  origin_view+=near_p-ray*(near_p.z/ray.z);
 }
 float distance=length(vertex-origin_view);
 if(distance<=100.0) return vec4(0.0,0.0,0.0,1.0);
 // Sqrt locates the quadratic slice interval. Interpolate transport by
 // actual path distance inside it, not sqrt(distance): the latter biases
 // near-field radiance even when both cumulative slice integrals are exact.
 float depth=clamp(distance,100.0,96000.0);
 float slice=min(floor(sqrt((depth-100.0)/95900.0)*15.0),14.0);
 float d0=100.0+95900.0*(slice/15.0)*(slice/15.0);
 float d1=100.0+95900.0*((slice+1.0)/15.0)*((slice+1.0)/15.0);
 float weight=clamp((depth-d0)/(d1-d0),0.0,1.0);
 vec3 coord=vec3(clamp(uv,vec2(0.5/32.0),vec2(31.5/32.0)),(0.5+slice+weight)/16.0);
#ifdef USE_MULTIVIEW
 if(ViewIndex==1) return textureLod(sampler3D(aerial_perspective_volume_eye1,DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP),coord,0.0);
#endif
 return textureLod(sampler3D(aerial_perspective_volume,DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP),coord,0.0);
}

vec4 native_height_fog_process(vec3 vertex) {
 vec3 origin_view = vec3(0.0);
#ifdef USE_MULTIVIEW
 origin_view = scene_data.eye_offset[ViewIndex].xyz;
#endif
 if (scene_data.height_fog_view.x > 0.0) {
  vec2 ndc = gl_FragCoord.xy * scene_data.screen_pixel_size * 2.0 - 1.0;
  vec4 near_h = inv_projection_matrix * vec4(ndc, 1.0, 1.0);
  vec4 far_h = inv_projection_matrix * vec4(ndc, 0.0, 1.0);
  vec3 near_point = near_h.xyz / near_h.w;
  vec3 parallel_ray = far_h.xyz / far_h.w - near_point;
  origin_view += near_point - parallel_ray * (near_point.z / parallel_ray.z);
 }
 mat4 world_from_view = transpose(mat4(scene_data.inv_view_matrix[0], scene_data.inv_view_matrix[1], scene_data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
 vec3 segment = vertex - origin_view;
 float distance_to_surface = length(segment);
 vec3 view_ray = distance_to_surface > 0.0 ? segment / distance_to_surface : vec3(0.0, 0.0, -1.0);
 vec3 ray = normalize(mat3(world_from_view) * view_ray);
 vec3 origin = (world_from_view * vec4(origin_view, 1.0)).xyz;
 bool capture_ready = scene_data.height_fog_view.y == 0.0 && scene_data.height_fog_view.z > 0.0;
 vec3 capture_color = vec3(0.0);
 if (capture_ready && scene_data.height_fog.source.w > 0.0) {
  vec3 radiance_ray = scene_data.radiance_inverse_xform * view_ray;
  float roughness_layer = scene_data.height_fog.options.x * MAX_ROUGHNESS_LOD;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
  float low_layer = floor(roughness_layer);
  vec3 low_color = sample_sky_radiance(radiance_ray, low_layer, 0.0).rgb;
  vec3 high_color = sample_sky_radiance(radiance_ray, min(low_layer + 1.0, MAX_ROUGHNESS_LOD), 0.0).rgb;
  capture_color = mix(low_color, high_color, fract(roughness_layer));
#else
  capture_color = sample_sky_radiance(radiance_ray, 0.0, roughness_layer).rgb;
#endif
 }
 return height_fog_integrate(scene_data.height_fog, origin, ray, distance_to_surface, false, capture_color, capture_ready);
}
