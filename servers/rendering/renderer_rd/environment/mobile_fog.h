#pragma once
#include "servers/rendering/renderer_rd/shaders/environment/mobile_fog.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {
// Composes native height fog/AP after screen-space AO, with no depth prepass.
class MobileFog {
public:
	class MobileFogView : public RenderBufferCustomDataRD {
		GDCLASS(MobileFogView, RenderBufferCustomDataRD);

	public:
		MobileFog *owner = nullptr;
		RenderSceneBuffersRD *buffers = nullptr;
		RID scene, radiance, next_radiance, ap[2];
		Projection projection[2];
		bool active = false, array = false, orthographic = false;
		uint32_t applied = 0;
		float luminance = 1, roughness_lod = 7;
		virtual void configure(RenderSceneBuffersRD *p_buffers) override {
			buffers = p_buffers;
			active = false;
		}
		virtual void free_data() override { active = false; }
		virtual bool composite_mobile_ao(RID p_ao, RID p_guide, int p_algorithm, int p_view, float p_radius, float p_intensity, float p_max_distance) override;
	};

private:
	MobileFogShaderRD shader;
	RID version, nearest_sampler, linear_sampler;
	HashMap<uint64_t, RID> pipelines;
	bool initialized = false;
	void initialize();
	bool composite(MobileFogView *p_view, RID p_ao, RID p_guide, int p_algorithm, int p_eye, float p_radius, float p_intensity, float p_max_distance);

public:
	bool prepare(const RenderDataRD *p_data, bool p_requested, RID p_radiance, RID p_next, RID p_ap0, RID p_ap1, bool p_array, int p_roughness_layers, float p_luminance);
	RID get_framebuffer(Ref<RenderSceneBuffersRD> p_buffers, bool p_resolve_depth);
	void finish(Ref<RenderSceneBuffersRD> p_buffers);
	~MobileFog();
};
} //namespace RendererRD
