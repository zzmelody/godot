#pragma once

#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

namespace RendererRD {
// Render-thread provider for native ReflectionProbe captures. Native ONCE /
// ALWAYS still own scheduling; one prepared immutable view serves all six faces.
class PhysicalSkyView {
	struct Source : public RefCounted {
		RID textures[2];
		RID originals[2];
		int64_t generation = 0;
		uint64_t bytes = 0;
		~Source();
	};
	struct Descriptor {
		Ref<Source> source;
		float medium[32] = {};
		int64_t generation = 0;
		uint64_t revision = 0;
		bool enabled = false;
		String state = "DISABLED";
		String reason;
	};
	struct View : public RefCounted {
		Ref<Source> source;
		RID texture;
		float medium[32] = {};
		~View();
	};
	struct Probe {
		RID sky;
		Ref<View> view;
		Vector3 origin;
		int64_t generation = 0;
	};
	static PhysicalSkyView *singleton;
	HashMap<RID, Descriptor> descriptors;
	HashMap<RID, Probe> probes;
	RID current_probe;
	Probe current;
	RID shader, pipeline, sampler;
	String shader_error;
	uint64_t revision = 0;
	uint64_t source_copies = 0;
	uint64_t dispatches = 0;
	bool initialize();
	void free_compute_resources();
	void fail(RID p_sky, Descriptor &p_descriptor, const String &p_reason);
	void detach(RID p_sky);

public:
	static PhysicalSkyView *get_singleton() { return singleton; }
	static Dictionary get_capabilities();
	static String get_shader_source();
	void set_source(RID p_sky, const Dictionary &p_source);
	Dictionary get_status(RID p_sky) const;
	void free_sky(RID p_sky);
	void free_probe(RID p_probe);
	bool prepare(RID p_sky, RID p_probe, const Vector3 &p_origin);
	void finish(bool p_completed = false);
	RID get_texture(RID p_probe) const;
	Vector4 get_parameters(RID p_probe) const;
	PhysicalSkyView();
	~PhysicalSkyView();
};
} // namespace RendererRD
