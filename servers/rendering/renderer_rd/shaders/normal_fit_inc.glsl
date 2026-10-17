// Best-fit normal encoding shared by scene and virtual geometry depth outputs.
// Kaplanyan, CryENGINE 3, Advances in Real-Time Rendering (SIGGRAPH 2010).
vec3 encode24(vec3 v, texture2D fitting_table, sampler fitting_sampler) {
	vec3 unsigned_normal = abs(v);
	float major = max(unsigned_normal.z, max(unsigned_normal.x, unsigned_normal.y));
	vec2 uv = unsigned_normal.z < major ? (unsigned_normal.y < major ? unsigned_normal.yz : unsigned_normal.xz) : unsigned_normal.xy;
	uv /= major;
	uv = uv.x < uv.y ? uv.yx : uv.xy;
	uv.y /= uv.x;
	return (v / major) * texture(sampler2D(fitting_table, fitting_sampler), uv).r;
}
