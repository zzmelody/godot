#[vertex]
#version 450
#VERSION_DEFINES
void main() {
 vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
 gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
#[fragment]
#version 450
#VERSION_DEFINES
#define MAX_VIEWS 2
#include "../scene_data_inc.glsl"
#include "../oct_inc.glsl"
layout(set=0,binding=0,std140) uniform SceneBlock { SceneData data; } scene_data_block;
#ifdef MSAA_INPUT
layout(set=0,binding=1) uniform sampler2DMS depth_tex;
layout(set=0,binding=2) uniform sampler2DMS fog_mask;
#else
layout(set=0,binding=1) uniform sampler2D depth_tex;
layout(set=0,binding=2) uniform sampler2D fog_mask;
#endif
#ifdef AO_SSAO
layout(set=0,binding=3) uniform sampler2D ao_tex;
layout(set=0,binding=4) uniform usampler2D guide_tex;
#elif defined(AO_GTAO)
layout(set=0,binding=3) uniform usampler2D ao_tex;
layout(set=0,binding=4) uniform sampler2D guide_tex;
#endif
layout(set=0,binding=5) uniform sampler linear_sampler;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set=0,binding=6) uniform texture2DArray radiance_octmap;
layout(set=0,binding=7) uniform texture2DArray radiance_octmap_next;
#else
layout(set=0,binding=6) uniform texture2D radiance_octmap;
layout(set=0,binding=7) uniform texture2D radiance_octmap_next;
#endif
layout(set=0,binding=8) uniform texture3D aerial_perspective_volume;
layout(set=0,binding=9) uniform texture3D aerial_perspective_volume_eye1;
layout(push_constant,std430) uniform Params {
 mat4 inv_proj;
 vec4 effect; // radius, intensity, max distance, luminance multiplier
 vec4 camera; // orthographic, projection[1][1], eye, roughness max LOD
} pc;
#define scene_data scene_data_block.data
#define inv_projection_matrix pc.inv_proj
#define DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP linear_sampler
#define MAX_ROUGHNESS_LOD pc.camera.w
#define USE_MULTIVIEW
#define ViewIndex int(pc.camera.z)
#include "mobile_fog_transport_inc.glsl"
layout(location=0) out vec4 color;
float ao_visibility(ivec2 xy, vec2 uv, float raw, vec3 p) {
 if(raw <= 0.0000001 || -p.z >= pc.effect.z) return 1.0;
#ifdef AO_SSAO
 ivec2 ao_size = textureSize(ao_tex,0);
 vec2 low = uv * scene_data.viewport_size / 2.0 - 0.5;
 ivec2 base = ivec2(floor(low));
 vec2 f = fract(low);
 float pixel_size = 2.0 * (pc.camera.x > 0.5 ? 1.0 : -p.z) / abs(pc.camera.y) / scene_data.viewport_size.y;
 float sigma = max(pc.effect.x * 0.05, pixel_size * 2.0);
 float sum=0.0, weight=0.0, nearest_error=1e30, nearest_ao=1.0;
 for(int i=0;i<4;i++) {
  ivec2 offset=ivec2(i&1,i>>1);
  ivec2 q=clamp(base+offset,ivec2(0),ao_size-1);
  float z=uintBitsToFloat(texelFetch(guide_tex,q,0).x);
  if(z>=pc.effect.z) continue;
  float a=texelFetch(ao_tex,q,0).r;
  float error=abs(z+p.z);
  if(error<nearest_error){nearest_error=error;nearest_ao=a;}
  vec2 bilinear=mix(vec2(1)-f,f,vec2(offset));
  float w=bilinear.x*bilinear.y*max(0.0,1.0-(error/sigma)*(error/sigma));
  sum+=a*w;weight+=w;
 }
 float fallback=nearest_error<max(pc.effect.x*.5,pixel_size*8.)?nearest_ao:1.;
 return weight>0.0001?sum/weight:fallback;
#elif defined(AO_GTAO)
 float visibility=float(texelFetch(ao_tex,xy,0).r)/255.0;
 float z=texelFetch(guide_tex,xy,0).r;
#ifdef MSAA_INPUT
 if(abs(-p.z-z)>max(pc.effect.x*.05,z*.001)) visibility=1.0;
#endif
 if(z>=pc.effect.z) visibility=1.0;
 return pow(clamp(visibility,0.0,1.0),pc.effect.y);
#else
 return 1.0;
#endif
}
void main() {
 ivec2 xy=ivec2(gl_FragCoord.xy);
#ifdef MSAA_INPUT
 float raw=texelFetch(depth_tex,xy,gl_SampleID).r;
 float eligible=texelFetch(fog_mask,xy,gl_SampleID).r;
 vec2 uv=(vec2(xy)+gl_SamplePosition)*scene_data.screen_pixel_size;
#else
 float raw=texelFetch(depth_tex,xy,0).r;
 float eligible=texelFetch(fog_mask,xy,0).r;
 vec2 uv=(vec2(xy)+0.5)*scene_data.screen_pixel_size;
#endif
 if(raw<=0.0000001) {color=vec4(0,0,0,1);return;}
 vec4 h=pc.inv_proj*vec4(uv*2.0-1.0,raw,1.0);
 vec3 vertex=h.xyz/h.w;
 float visibility=ao_visibility(xy,uv,raw,vertex);
 color=vec4(scene_data.height_fog.options.z,scene_data.aerial_perspective.x,scene_data.height_fog.density.x*100.0,0);return;
 vec4 transport=vec4(0,0,0,1);
 if(eligible>0.5) {
  if(scene_data.height_fog.options.z>0.0) transport=native_height_fog_process(vertex);
  if(native_ap_enabled()) {
   vec4 ap=native_ap_process(vertex);
   transport=vec4(transport.rgb+transport.a*ap.rgb,transport.a*ap.a);
   if(scene_data.aerial_perspective.z>0.0) transport=ap;
  }
  float debug_mode=scene_data.aerial_perspective.z>0.0?scene_data.aerial_perspective.z:scene_data.height_fog.options.y;
  if(debug_mode==1.0){color=vec4(vec3(transport.a)/pc.effect.w,0);return;}
  if(debug_mode==2.0){color=vec4(transport.rgb/pc.effect.w,0);return;}
 }
 // Hardware blending: source RGB + destination RGB * source alpha.
 // Preserve target alpha independently (ZERO, ONE).
 color=vec4(transport.rgb*scene_data.emissive_exposure_normalization/pc.effect.w,visibility*transport.a);
}
