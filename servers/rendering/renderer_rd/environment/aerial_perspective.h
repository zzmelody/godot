#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"

namespace RendererRD {

// The render thread owns all resources. RD tracks copy/compute/sample hazards
// and deferred destruction; no CPU readback or host fence is used as readiness.
class AerialPerspective {
public:
	class View : public RenderBufferCustomDataRD {
		GDCLASS(View, RenderBufferCustomDataRD);

	public:
		RID volumes[2];
		RID uniform_buffer;
		RID medium_buffer;
		RID environment;
		RID probe;
		int capture_face = -1;
		int eye = 0;
		uint64_t revision = 0;
		uint64_t dispatches = 0;
		PackedByteArray key;
		Vector3 origin;
		Projection projection;
		bool orthographic = false;
		int64_t generation = 0;
		String state = "DISABLED";
		String reason;
		int front = 0;
		virtual void configure(RenderSceneBuffersRD *) override {}
		virtual void free_data() override;
		View();
		~View();
	};

private:
	struct Source : public RefCounted {
		RID textures[2];
		RID originals[2];
		int64_t generation = 0;
		uint64_t bytes = 0;
		~Source();
	};
	struct Environment {
		Dictionary accepted;
		float medium[32] = {};
		Vector3 scale = Vector3(1, 1, 1);
		Ref<Source> source;
		uint64_t revision = 0;
		int64_t generation = 0;
		bool enabled = false;
		int debug = 0;
		String state = "DISABLED";
		String reason;
	};
	static AerialPerspective *singleton;
	HashMap<RID, Environment> environments;
	HashSet<View *> views;
	Ref<View> capture_view;
	Ref<View> current[2];
	RID shader;
	RID pipeline;
	RID sampler;
	String shader_error;
	uint64_t serial = 0;
	uint64_t total_dispatches = 0;
	uint64_t total_source_copies = 0;
	uint64_t scene_depth_copies = 0;
	uint64_t scene_depth_copy_resolves = 0;
	bool initialize();
	void detach(RID p_environment);
	void fail(Environment &r_environment, RID p_environment, const String &p_reason);
	bool render_view(const RenderDataRD *p_data, Ref<View> p_view, int p_eye);

public:
	static AerialPerspective *get_singleton() { return singleton; }
	void set_state(RID p_environment, const Dictionary &p_state);
	Dictionary get_state(RID p_environment) const;
	Dictionary get_status(RID p_environment) const;
	static Dictionary get_capabilities();
	// Renderer-wide actual Mobile copy calls, including AP-disabled frames.
	void record_scene_depth_copy(bool p_resolve) {
		scene_depth_copies++;
		scene_depth_copy_resolves += p_resolve ? 1 : 0;
	}
	void free_environment(RID p_environment);
	void prepare(const RenderDataRD *p_data);
	RID get_texture(int p_eye) const;
	Vector4 get_parameters() const;
	AerialPerspective();
	~AerialPerspective();
};
} //namespace RendererRD
