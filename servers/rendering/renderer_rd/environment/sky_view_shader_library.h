#pragma once

// Canonical SkyView compute source. Native probe captures and the main-view
// provider use the exact same declarations, atmosphere library and integral.
inline constexpr const char *SKY_VIEW_SHADER_DECLARATIONS = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 1) uniform sampler2D multiscatter_lut;
layout(rgba16f, set = 0, binding = 2) uniform writeonly image2D sky_view_lut;

// Exactly 128 bytes — the size Vulkan guarantees on every device, and what
// several mobile GPUs report as their hard maximum. A new term reuses a spare
// component; it does NOT append a ninth vec4. Both spare components (the old
// ground albedo, dead here because this bake runs Ground=false — see the note in
// atmosphere_radiance()) now carry the moon, so the block is FULL: 32 of 32
// floats. A ninth term needs a UBO, not another component.
layout(push_constant, std430) uniform SkyViewParams {
    vec4 planet;              // radius, atmosphere height, moon elongation, rayleigh scale
    vec4 rayleigh;            // normalised RGB coefficient, density height
    vec4 mie_scattering;      // scale, normalised RGB coefficient
    vec4 mie_absorption;      // scale, normalised RGB coefficient
    vec4 mie_shape_absorption; // density height, anisotropy, absorption tip altitude/value
    vec4 absorption;          // one-side width, scale, normalised coefficient R/G
    vec4 art_absorption;      // coefficient B, multiscatter, view altitude, samples
    vec4 sun;                 // arc yaw/pitch, illuminance, moon illuminance
} params;

)GLSL";

inline constexpr const char *SKY_VIEW_SHADER_BODY = R"GLSL(
// UE's FromSubUvsToUnit (SkyAtmosphereCommon.ush:39). Applied before parsing
// the canonical [0,1] mapping so bake and sample agree at the zenith / seam.
vec2 sky_view_sub_to_unit(vec2 uv_sub) {
    vec2 W = vec2(192.0, 104.0);
    return (uv_sub - 0.5 / W) * W / (W - 1.0);
}

// UE UvToSkyViewLutParams (SkyAtmosphereCommon.ush:207-230). The split sits on
// the GEOMETRIC horizon, which is pi/2 only at view_altitude 0 — see the mirror
// comment on sky_view_uv() in sky_atmosphere.gdshader.
vec3 view_dir_from_uv(vec2 coord) {
    vec2 uv = clamp(sky_view_sub_to_unit(coord), vec2(0.0), vec2(1.0));
    float v = uv.y;
    float r = view_radius();
    float v_horizon = sqrt(max(0.0, r * r - planet_radius() * planet_radius()));
    float beta = acos(clamp(v_horizon / max(r, EPS), 0.0, 1.0));
    float zenith_horizon = PI - beta;
    float theta;
    if (v < 0.5) {
        float c = v * 2.0;
        theta = (1.0 - (1.0 - c) * (1.0 - c)) * zenith_horizon;
    } else {
        float c = (v - 0.5) * 2.0;
        theta = zenith_horizon + c * c * beta;
    }
    float azimuth = (uv.x - 0.5) * 2.0 * PI;
    float st = sin(theta);
    return vec3(st * cos(azimuth), cos(theta), st * sin(azimuth));
}

vec3 atmosphere_radiance(vec3 view_d) {
    vec3 sun_d = sun_direction();
    float r0 = view_radius();
    float mu = view_d.y;
    float cos_theta = dot(view_d, sun_d);
    float ph_r = phase_rayleigh(cos_theta);
    float ph_m = phase_mie(cos_theta, params.mie_shape_absorption.y);
    // The second light's phase terms, hoisted out of the march exactly as the
    // sun's are — they depend on the view/light angle, which the march does not
    // change (UE hoists both the same way, SkyAtmosphere.usf:586-590).
    vec3 moon_d = moon_direction();
    float cos_theta_m = dot(view_d, moon_d);
    float ph_r_m = phase_rayleigh(cos_theta_m);
    float ph_m_m = phase_mie(cos_theta_m, params.mie_shape_absorption.y);
    float t_max = ray_length(r0, mu);
    // UE's adaptive sample count (SkyAtmosphere.usf:568-575). params.art_absorption.w
    // is MaxSampleCount, not a fixed step count. kMinSampleCount = 4, kDistanceToSampleMax
    // = 150 km — see sky_atmosphere.h for the CPU-side constants.
    const int kMinSampleCount = 4;
    const float kDistanceToSampleMaxInv = 1.0 / 150.0;
    int max_samples = clamp(int(params.art_absorption.w + 0.5), kMinSampleCount, 128);
    float t_fraction = clamp(t_max * kDistanceToSampleMaxInv, 0.0, 1.0);
    int steps = max(kMinSampleCount,
                    int(round(float(kMinSampleCount) +
                              float(max_samples - kMinSampleCount) * t_fraction)));

    vec3 thru = vec3(1.0);
    vec3 sum = vec3(0.0);
    float t_prev = 0.0;
    for (int i = 0; i < 128; ++i) {
        if (i >= steps) break;
        float t_next = t_max * pow(float(i + 1) / float(steps), MARCH_BIAS);
        float dt = t_next - t_prev;
        float t = 0.5 * (t_prev + t_next);
        t_prev = t_next;
        float nr = radius_at(r0, mu, t);
        float alt = max(0.0, nr - planet_radius());
        float sun_mu = (t * cos_theta + r0 * sun_d.y) / max(nr, EPS);
        float moon_mu = (t * cos_theta_m + r0 * moon_d.y) / max(nr, EPS);
        vec3 ext;
        vec3 source = atmosphere_sample_source(nr, alt, sun_mu, moon_mu,
                vec2(ph_r, ph_m), vec2(ph_r_m, ph_m_m), ext);
        vec3 step_t = exp(-ext * dt);
        sum += thru * source *
               (vec3(1.0) - step_t) / max(ext, vec3(EPS));
        thru *= step_t;
    }
    // Ground=false in SkyView LUT bake (SkyAtmosphere.usf:1394). MS LUT still
    // carries the bounce contribution via the CPU bake's Ground=true path.
    return min(max(sum, vec3(0.0)), vec3(64000.0));
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= 192 || pixel.y >= 104) return;
    vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(192.0, 104.0);
    vec3 view_d = view_dir_from_uv(uv);
    imageStore(sky_view_lut, pixel, vec4(atmosphere_radiance(view_d), 1.0));
}
)GLSL";
