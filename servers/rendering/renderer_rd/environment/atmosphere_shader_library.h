#pragma once

// Canonical GPU atmosphere math shared by native AP and the SkyView provider.
inline constexpr const char *ATMOSPHERE_SHADER_LIBRARY = R"GLSL(
const float PI = 3.14159265358979323846;
const float EPS = 1e-6;
const float MARCH_BIAS = 2.0;
float planet_radius() { return params.planet.x; }
float atmosphere_height() { return params.planet.y; }
float top_radius() { return planet_radius() + atmosphere_height(); }
// Already clamped into the shell CPU-side (AtmosphereParams::view_altitude_km).
float view_radius() { return planet_radius() + params.art_absorption.z; }
float sun_illuminance() { return params.sun.z; }
// The port of Core's sun_dir_on_arc (sun_direction.cpp). Both bodies sit on one
// great circle whose heading is params.sun.x, so the second light is the same
// formula at a pitch displaced by the elongation — no rotation axis, and
// therefore no axis whose sign can be reconstructed wrongly (the defect
// tests/fixtures/moon_arc_reverses_bug pins). assert_sky_pixel's
// `mode: reference` is what catches this and the CPU body drifting apart.
vec3 dir_on_arc(float pitch) {
    float cp = cos(pitch);
    return vec3(sin(params.sun.x) * cp, sin(pitch), cos(params.sun.x) * cp);
}
vec3 sun_direction() { return dir_on_arc(params.sun.y); }
float moon_illuminance() { return params.sun.w; }
// Minus, not plus: the moon TRAILS the sun along the arc by the elongation.
vec3 moon_direction() { return dir_on_arc(params.sun.y - params.planet.z); }

float ray_top(float r, float mu) {
    float top = top_radius();
    float disc = r * r * (mu * mu - 1.0) + top * top;
    return max(0.0, -r * mu + sqrt(max(disc, 0.0)));
}

float ray_ground(float r, float mu) {
    if (mu > 0.0) return -1.0;
    float bot = planet_radius();
    float disc = r * r * (mu * mu - 1.0) + bot * bot;
    if (disc < 0.0) return -1.0;
    return -r * mu - sqrt(disc);
}

bool hits_ground(float r, float mu) {
    if (mu >= 0.0) return false;
    float bot = planet_radius();
    return r * r * (mu * mu - 1.0) + bot * bot >= 0.0;
}

float ray_length(float r, float mu) {
    return hits_ground(r, mu) ? max(0.0, ray_ground(r, mu)) : ray_top(r, mu);
}

float radius_at(float r, float mu, float t) {
    return sqrt(max(0.0, t * t + 2.0 * r * mu * t + r * r));
}

vec2 trans_uv(float r, float mu) {
    float bot = planet_radius();
    float top = top_radius();
    r = clamp(r, bot, top);
    float H = max(sqrt(max(0.0, top * top - bot * bot)), EPS);
    float rho = sqrt(max(0.0, r * r - bot * bot));
    float d = ray_top(r, mu);
    float d_min = top - r;
    float d_max = rho + H;
    float span = d_max - d_min;
    float u = span > EPS ? (d - d_min) / span : 0.0;
    return clamp(vec2(u, rho / H), vec2(0.0), vec2(1.0));
}

vec3 sky_trans(float r, float mu) {
    if (hits_ground(r, mu)) return vec3(0.0);
    vec3 encoded = texture(transmittance_lut, trans_uv(r, mu)).rgb;
    return encoded * encoded;
}

void medium(float altitude, out vec3 scat, out vec3 ext,
            out vec3 ray_s, out vec3 mie_s) {
    float alt = max(0.0, altitude);
    float rayleigh_height = max(params.rayleigh.w, EPS);
    float mie_height = max(params.mie_shape_absorption.x, EPS);
    float r_den = exp(-alt / rayleigh_height);
    float m_den = exp(-alt / mie_height);
    float tip_value = max(params.mie_shape_absorption.w, 0.0);
    float slope = tip_value / max(params.absorption.x, 1e-3);
    float absorption_den = clamp(
        tip_value - abs(alt - params.mie_shape_absorption.z) * slope, 0.0, 1.0);

    mie_s = params.mie_scattering.yzw * params.mie_scattering.x * m_den;
    vec3 mie_a = params.mie_absorption.yzw * params.mie_absorption.x * m_den;
    ray_s = params.rayleigh.rgb * params.planet.w * r_den;
    vec3 absorption_coefficient = vec3(params.absorption.zw,
                                       params.art_absorption.x);
    vec3 layer_absorption = absorption_coefficient * params.absorption.y *
                            absorption_den;
    scat = ray_s + mie_s;
    ext = scat + mie_a + layer_absorption;
}

float phase_rayleigh(float c) {
    return 3.0 / (16.0 * PI) * (1.0 + c * c);
}

float phase_mie(float c, float g) {
    g = clamp(g, -0.99, 0.99);
    float denom = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) /
           (4.0 * PI * max(denom * sqrt(max(denom, 0.0)), EPS));
}


// Shared SkyView/AP sample source. Both callers supply the same per-sample
// radial light cosines and view-dependent Rayleigh/Mie phase terms.
vec3 atmosphere_sample_source(float nr, float alt, float sun_mu, float moon_mu,
        vec2 sun_phase, vec2 moon_phase, out vec3 ext) {
    vec3 scat, ray_s, mie_s;
    medium(alt, scat, ext, ray_s, mie_s);
    vec3 sun_t = sky_trans(nr, sun_mu);
    vec2 ms_uv = vec2((sun_mu + 1.0) * 0.5,
                     clamp(alt / max(atmosphere_height(), EPS), 0.0, 1.0));
    vec3 ms = texture(multiscatter_lut, ms_uv).rgb;
    float illuminance = sun_illuminance();
    vec3 single = (ray_s * sun_phase.x + mie_s * sun_phase.y) * sun_t * illuminance;
    vec3 multi = scat * ms * params.art_absorption.y * illuminance;
    if (moon_illuminance() > 0.0) {
        vec3 moon_t = sky_trans(nr, moon_mu);
        vec2 ms_uv_m = vec2((moon_mu + 1.0) * 0.5,
                           clamp(alt / max(atmosphere_height(), EPS), 0.0, 1.0));
        vec3 ms_m = texture(multiscatter_lut, ms_uv_m).rgb;
        float moon_lux = moon_illuminance();
        single += (ray_s * moon_phase.x + mie_s * moon_phase.y) * moon_t * moon_lux;
        multi += scat * ms_m * params.art_absorption.y * moon_lux;
    }
    return single + multi;
}
)GLSL";
