/*<<----- VEYA_COOKER: minimal server registry for offline rendering data, 3D physics and navigation only. */
#include "servers/register_server_types.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/navigation_3d/navigation_server_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/physics_3d/physics_server_3d_dummy.h"
#include "servers/physics_3d/physics_server_3d_extension.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_include_db.h"
#include "servers/rendering/shader_types.h"
#include "servers/rendering/storage/render_data.h"
#include "servers/rendering/storage/render_data_extension.h"
#include "servers/rendering/storage/render_scene_buffers.h"
#include "servers/rendering/storage/render_scene_data.h"

namespace {
ShaderTypes *shader_types = nullptr;

PhysicsServer3D *create_dummy_physics_server_3d() {
	return memnew(PhysicsServer3DDummy);
}

bool has_server_feature(const String &p_feature) {
	RenderingServer *server = RenderingServer::get_singleton();
	return server && server->has_os_feature(p_feature);
}
} // namespace

void register_server_types() {
	OS::get_singleton()->benchmark_begin_measure("Servers", "Register Cooker Types");

	shader_types = memnew(ShaderTypes);
	OS::get_singleton()->set_has_server_feature_callback(has_server_feature);

	GDREGISTER_ABSTRACT_CLASS(RenderingServer);
	GDREGISTER_ABSTRACT_CLASS(RenderingDevice);
	GDREGISTER_CLASS(ShaderIncludeDB);
	GDREGISTER_CLASS(RDTextureFormat);
	GDREGISTER_CLASS(RDTextureView);
	GDREGISTER_CLASS(RDAttachmentFormat);
	GDREGISTER_CLASS(RDFramebufferPass);
	GDREGISTER_CLASS(RDSamplerState);
	GDREGISTER_CLASS(RDVertexAttribute);
	GDREGISTER_CLASS(RDUniform);
	GDREGISTER_CLASS(RDPipelineRasterizationState);
	GDREGISTER_CLASS(RDPipelineMultisampleState);
	GDREGISTER_CLASS(RDPipelineDepthStencilState);
	GDREGISTER_CLASS(RDPipelineColorBlendStateAttachment);
	GDREGISTER_CLASS(RDPipelineColorBlendState);
	GDREGISTER_CLASS(RDShaderSource);
	GDREGISTER_CLASS(RDShaderSPIRV);
	GDREGISTER_CLASS(RDShaderFile);
	GDREGISTER_CLASS(RDPipelineSpecializationConstant);
	GDREGISTER_CLASS(RDAccelerationStructureGeometry);
	GDREGISTER_CLASS(RDAccelerationStructureInstance);
	GDREGISTER_CLASS(RDPipelineShader);
	GDREGISTER_CLASS(RDHitGroup);
	GDREGISTER_ABSTRACT_CLASS(RenderData);
	GDREGISTER_CLASS(RenderDataExtension);
	GDREGISTER_ABSTRACT_CLASS(RenderSceneData);
	GDREGISTER_CLASS(RenderSceneDataExtension);
	GDREGISTER_CLASS(RenderSceneBuffersConfiguration);
	GDREGISTER_ABSTRACT_CLASS(RenderSceneBuffers);
	GDREGISTER_CLASS(RenderSceneBuffersExtension);

	GDREGISTER_CLASS(NavigationServer3DManager);
	GDREGISTER_ABSTRACT_CLASS(NavigationServer3D);
	GDREGISTER_CLASS(NavigationPathQueryParameters3D);
	GDREGISTER_CLASS(NavigationPathQueryResult3D);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, NavigationServer3DManager::setting_property_name, PROPERTY_HINT_ENUM, "DEFAULT"), "DEFAULT");
	NavigationServer3DManager::get_singleton()->register_server("Dummy", callable_mp_static(NavigationServer3DManager::create_dummy_server_callback));

	GDREGISTER_CLASS(PhysicsServer3DManager);
	GDREGISTER_ABSTRACT_CLASS(PhysicsServer3D);
	GDREGISTER_VIRTUAL_CLASS(PhysicsServer3DExtension);
	GDREGISTER_ABSTRACT_CLASS(PhysicsDirectBodyState3D);
	GDREGISTER_VIRTUAL_CLASS(PhysicsDirectBodyState3DExtension);
	GDREGISTER_ABSTRACT_CLASS(PhysicsDirectSpaceState3D);
	GDREGISTER_VIRTUAL_CLASS(PhysicsDirectSpaceState3DExtension);
	GDREGISTER_VIRTUAL_CLASS(PhysicsServer3DRenderingServerHandler);
	GDREGISTER_NATIVE_STRUCT(PhysicsServer3DExtensionRayResult, "Vector3 position;Vector3 normal;RID rid;ObjectID collider_id;Object *collider;int shape;int face_index");
	GDREGISTER_NATIVE_STRUCT(PhysicsServer3DExtensionShapeResult, "RID rid;ObjectID collider_id;Object *collider;int shape");
	GDREGISTER_NATIVE_STRUCT(PhysicsServer3DExtensionShapeRestInfo, "Vector3 point;Vector3 normal;RID rid;ObjectID collider_id;int shape;Vector3 linear_velocity");
	GDREGISTER_NATIVE_STRUCT(PhysicsServer3DExtensionMotionCollision, "Vector3 position;Vector3 normal;Vector3 collider_velocity;Vector3 collider_angular_velocity;real_t depth;int local_shape;ObjectID collider_id;RID collider;int collider_shape");
	GDREGISTER_NATIVE_STRUCT(PhysicsServer3DExtensionMotionResult, "Vector3 travel;Vector3 remainder;real_t collision_depth;real_t collision_safe_fraction;real_t collision_unsafe_fraction;PhysicsServer3DExtensionMotionCollision collisions[32];int collision_count");
	GDREGISTER_CLASS(PhysicsRayQueryParameters3D);
	GDREGISTER_CLASS(PhysicsPointQueryParameters3D);
	GDREGISTER_CLASS(PhysicsShapeQueryParameters3D);
	GDREGISTER_CLASS(PhysicsTestMotionParameters3D);
	GDREGISTER_CLASS(PhysicsTestMotionResult3D);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, PhysicsServer3DManager::setting_property_name, PROPERTY_HINT_ENUM, "DEFAULT"), "DEFAULT");
	PhysicsServer3DManager::get_singleton()->register_server("Dummy", callable_mp_static(create_dummy_physics_server_3d));

	OS::get_singleton()->benchmark_end_measure("Servers", "Register Cooker Types");
}

void unregister_server_types() {
	OS::get_singleton()->benchmark_begin_measure("Servers", "Unregister Cooker Types");
	memdelete(shader_types);
	shader_types = nullptr;
	OS::get_singleton()->benchmark_end_measure("Servers", "Unregister Cooker Types");
}

void register_server_singletons() {
	Engine::get_singleton()->add_singleton(Engine::Singleton("RenderingServer", RenderingServer::get_singleton(), "RenderingServer"));
	Engine::get_singleton()->add_singleton(Engine::Singleton("NavigationServer3D", NavigationServer3D::get_singleton(), "NavigationServer3D"));
	Engine::get_singleton()->add_singleton(Engine::Singleton("PhysicsServer3D", PhysicsServer3D::get_singleton(), "PhysicsServer3D"));
}
/*>>----- VEYA_COOKER */
