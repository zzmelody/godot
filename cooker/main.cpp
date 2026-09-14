/*<<----- VEYA_COOKER: native batch asset entry. The ordinary editor main is not linked. */
#include "main/main.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/core_globals.h"
#include "core/debugger/engine_debugger.h"
#include "core/extension/gdextension_manager.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/io/dir_access.h"
#include "core/io/file_access_pack.h"
#include "core/io/json.h"
#include "core/io/image.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "core/object/script_language.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/register_core_types.h"
#include "core/string/translation_server.h"
#include "core/version.h"
#include "drivers/register_driver_types.h"
#include "editor/import/3d/resource_importer_obj.h"
#include "editor/import/3d/resource_importer_scene.h"
#include "main/performance.h"
#include "modules/fbx/editor/editor_scene_importer_ufbx.h"
#include "modules/gltf/editor/editor_scene_importer_gltf.h"
#include "modules/gltf/gltf_state.h"
#include "modules/register_module_types.h"
#include "platform/register_platform_apis.h"
#include "scene/property_list_helper.h"
#include "scene/register_scene_types.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/portable_compressed_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/particle_process_material.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/3d/navigation_mesh_source_geometry_data_3d.h"
#include "scene/resources/navigation_mesh.h"
#include "scene/theme/theme_db.h"
#include "servers/navigation_3d/navigation_server_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/register_server_types.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/text/text_server_dummy.h"
#include "cooker/asset_files.h"

namespace {
Engine *engine = nullptr;
ProjectSettings *settings = nullptr;
PackedData *packed_data = nullptr;
TranslationServer *translations = nullptr;
TextServerManager *text_servers = nullptr;
MessageQueue *messages = nullptr;
ThemeDB *theme_db = nullptr;
Performance *performance = nullptr;
InputMap *input_map = nullptr;
Input *input = nullptr;
RenderingServer *renderer = nullptr;
PhysicsServer3DManager *physics_manager = nullptr;
PhysicsServer3D *physics = nullptr;
String command;
String job_path;
bool initialized = false;

Dictionary capabilities() {
	Dictionary result;
	result["tool"] = "veya-asset-cooker";
	result["protocol_version"] = 1;
	result["godot"] = GODOT_VERSION_FULL_NAME;
	result["renderer"] = "dummy";
	result["script_languages"] = ScriptServer::get_language_count();
	Dictionary classes;
	for (const char *name : { "Node2D", "Control", "EditorNode", "GDScript", "ArrayMesh", "ImporterMesh", "PackedScene", "Skeleton3D", "Skin", "AnimationLibrary", "StandardMaterial3D", "ShaderMaterial", "GPUParticles3D", "ParticleProcessMaterial", "NavigationMesh", "HeightMapShape3D", "PCKPacker", "Image", "Texture2D" }) {
		classes[name] = ClassDB::class_exists(name);
	}
	result["classes"] = classes;
	return result;
}

Error read_job(Dictionary &r_job) {
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(job_path, FileAccess::READ, &error);
	ERR_FAIL_COND_V(file.is_null(), error);
	ERR_FAIL_COND_V_MSG(file->get_length() > 4 * 1024 * 1024, ERR_PARAMETER_RANGE_ERROR, "Job exceeds 4 MiB.");
	Ref<JSON> json;
	json.instantiate();
	error = json->parse(file->get_as_text());
	ERR_FAIL_COND_V_MSG(error != OK, error, json->get_error_message());
	ERR_FAIL_COND_V(json->get_data().get_type() != Variant::DICTIONARY, ERR_INVALID_DATA);
	r_job = json->get_data();
	return OK;
}

void inspect_node(Node *p_node, Dictionary &r_result) {
	r_result["node_count"] = int64_t(r_result.get("node_count", 0)) + 1;
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_node);
	if (instance && instance->get_mesh().is_valid()) {
		Ref<Mesh> mesh = instance->get_mesh();
		r_result["mesh_count"] = int64_t(r_result.get("mesh_count", 0)) + 1;
		r_result["surface_count"] = int64_t(r_result.get("surface_count", 0)) + mesh->get_surface_count();
		for (int i = 0; i < mesh->get_surface_count(); i++) {
			Ref<BaseMaterial3D> material = mesh->surface_get_material(i);
			if (material.is_valid()) {
				Ref<Texture2D> texture = material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
				if (texture.is_valid()) {
					r_result["material_texture_count"] = int64_t(r_result.get("material_texture_count", 0)) + 1;
					Ref<Image> image = texture->get_image();
					if (image.is_valid()) {
						r_result["material_texture_bytes"] = int64_t(r_result.get("material_texture_bytes", 0)) + image->get_data().size();
					}
				}
			}
			Array arrays = mesh->surface_get_arrays(i);
			if (arrays.size() == Mesh::ARRAY_MAX) {
				PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
				r_result["vertex_count"] = int64_t(r_result.get("vertex_count", 0)) + vertices.size();
			}
		}
	}
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_node);
	if (skeleton) {
		 r_result["bone_count"] = int64_t(r_result.get("bone_count", 0)) + skeleton->get_bone_count();
	}
	AnimationPlayer *player = Object::cast_to<AnimationPlayer>(p_node);
	if (player) {
		LocalVector<StringName> animations;
		player->get_animation_list(&animations);
		r_result["animation_count"] = int64_t(r_result.get("animation_count", 0)) + animations.size();
	}
	CollisionShape3D *collider = Object::cast_to<CollisionShape3D>(p_node);
	if (collider && collider->get_shape().is_valid()) {
		r_result["collider_count"] = int64_t(r_result.get("collider_count", 0)) + 1;
	}
	GPUParticles3D *particles = Object::cast_to<GPUParticles3D>(p_node);
	if (particles) {
		Dictionary properties;
		properties["amount"] = particles->get_amount();
		properties["lifetime"] = particles->get_lifetime();
		Ref<ParticleProcessMaterial> process = particles->get_process_material();
		if (process.is_valid()) {
			Vector3 gravity = process->get_gravity();
			properties["gravity"] = Array({ gravity.x, gravity.y, gravity.z });
		}
		r_result["particles"] = properties;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		inspect_node(p_node->get_child(i), r_result);
	}
}

Error check_scene_types(const Ref<PackedScene> &p_scene, int p_depth = 0) {
	ERR_FAIL_COND_V_MSG(p_depth > 64, ERR_INVALID_DATA, "Scene nesting exceeds 64 levels.");
	Ref<SceneState> state = p_scene->get_state();
	for (int i = 0; i < state->get_node_count(); i++) {
		StringName type = state->get_node_type(i);
		ERR_FAIL_COND_V_MSG(!type.is_empty() && !ClassDB::class_exists(type), ERR_UNAVAILABLE, "Unsupported scene node type: " + String(type));
		Ref<PackedScene> nested = state->get_node_instance(i);
		if (nested.is_valid()) {
			Error error = check_scene_types(nested, p_depth + 1);
			ERR_FAIL_COND_V(error != OK, error);
		}
	}
	return OK;
}

Error execute_job(Dictionary &r_result) {
	Dictionary job;
	Error error = read_job(job);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V(!job.has("operation") || job["operation"].get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	for (const char *name : { "options" }) {
		ERR_FAIL_COND_V(job.has(name) && job[name].get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
	}
	for (const char *name : { "mipmaps", "normal_map", "lightmap_uv" }) {
		ERR_FAIL_COND_V(job.has(name) && job[name].get_type() != Variant::BOOL, ERR_INVALID_PARAMETER);
	}
	String operation = job.get("operation", "");
	for (const char *name : { "type", "compression", "collision_output" }) {
		ERR_FAIL_COND_V(job.has(name) && job[name].get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	}
	if (operation == "import-scene" || operation == "import-texture" || operation == "save-resource" || operation == "process-mesh" || operation == "bake-navigation" || operation == "pack") {
		ERR_FAIL_COND_V(!job.has("output"), ERR_INVALID_PARAMETER);
	}
	if (job.has("source")) {
		ERR_FAIL_COND_V(job["source"].get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		error = CookerFiles::check_path(job["source"]);
		ERR_FAIL_COND_V(error != OK, error);
	}
	if (job.has("output")) {
		ERR_FAIL_COND_V(job["output"].get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		error = CookerFiles::check_path(job["output"], true, operation == "pack");
		ERR_FAIL_COND_V(error != OK, error);
	}
	if (operation == "pack") {
		return CookerFiles::pack(job, r_result);
	}
	if (operation == "process-mesh") {
		String source = job.get("source", "");
		String output = job.get("output", "");
		HashSet<String> dependencies;
		error = CookerFiles::collect(source, dependencies);
		ERR_FAIL_COND_V(error != OK, error);
		Ref<Mesh> source_mesh = ResourceLoader::load(source);
		ERR_FAIL_COND_V(source_mesh.is_null(), ERR_INVALID_DATA);
		Ref<ImporterMesh> mesh = ImporterMesh::from_mesh(source_mesh);
		ERR_FAIL_COND_V(mesh.is_null(), ERR_INVALID_DATA);
		mesh->optimize_indices();
		if (bool(job.get("lightmap_uv", false))) {
			Vector<uint8_t> source_cache;
			Vector<uint8_t> result_cache;
			error = mesh->lightmap_unwrap_cached(Transform3D(), 0.1, source_cache, result_cache);
			ERR_FAIL_COND_V(error != OK, error);
		}
		// Unwrap rebuilds surfaces; generate LODs after the final vertex/index layout.
		mesh->generate_lods(60.0, Array());
		mesh->create_shadow_mesh();
		Ref<ArrayMesh> result = mesh->get_mesh();
		ERR_FAIL_COND_V(result.is_null(), ERR_CANT_CREATE);
		int lod_count = 0;
		for (int i = 0; i < mesh->get_surface_count(); i++) {
			lod_count += mesh->get_surface_lod_count(i);
		}
		r_result["lod_count"] = lod_count;
		String collision_output = job.get("collision_output", "");
		if (!collision_output.is_empty()) {
			error = CookerFiles::check_path(collision_output, true);
			ERR_FAIL_COND_V(error != OK || collision_output == output, ERR_INVALID_PARAMETER);
			Ref<MeshConvexDecompositionSettings> decomposition;
			decomposition.instantiate();
			Vector<Ref<Shape3D>> hulls = mesh->convex_decompose(decomposition);
			ERR_FAIL_COND_V(hulls.is_empty(), ERR_CANT_CREATE);
			StaticBody3D *body = memnew(StaticBody3D);
			body->set_name("ConvexCollision");
			for (const Ref<Shape3D> &hull : hulls) {
				CollisionShape3D *shape = memnew(CollisionShape3D);
				shape->set_shape(hull);
				body->add_child(shape, true);
				shape->set_owner(body);
			}
			Ref<PackedScene> scene;
			scene.instantiate();
			error = scene->pack(body);
			memdelete(body);
			ERR_FAIL_COND_V(error != OK, error);
			error = DirAccess::make_dir_recursive_absolute(settings->globalize_path(collision_output.get_base_dir()));
			ERR_FAIL_COND_V(error != OK, error);
			error = ResourceSaver::save(scene, collision_output, ResourceSaver::FLAG_COMPRESS);
			ERR_FAIL_COND_V(error != OK, error);
			r_result["collision_output"] = collision_output;
			r_result["hull_count"] = hulls.size();
		}
		error = CookerFiles::check_path(output, true);
		ERR_FAIL_COND_V(error != OK, error);
		error = DirAccess::make_dir_recursive_absolute(settings->globalize_path(output.get_base_dir()));
		ERR_FAIL_COND_V(error != OK, error);
		error = ResourceSaver::save(result, output, ResourceSaver::FLAG_COMPRESS);
		ERR_FAIL_COND_V(error != OK, error);
		r_result["output"] = output;
		return OK;
	}
	if (operation == "bake-navigation") {
		for (const char *name : { "cell_size", "cell_height", "agent_height", "agent_radius", "agent_max_climb" }) {
			if (job.has(name)) {
				ERR_FAIL_COND_V(job[name].get_type() != Variant::FLOAT && job[name].get_type() != Variant::INT, ERR_INVALID_PARAMETER);
				double value = job[name];
				ERR_FAIL_COND_V(!Math::is_finite(value) || value < 0.01 || value > 1000.0, ERR_INVALID_PARAMETER);
			}
		}
		ERR_FAIL_COND_V(!job.has("vertices") || job["vertices"].get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		Array points = job["vertices"];
		ERR_FAIL_COND_V(points.is_empty() || points.size() % 3 != 0, ERR_INVALID_PARAMETER);
		PackedVector3Array faces;
		for (const Variant &point : points) {
			ERR_FAIL_COND_V(point.get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
			Array coordinates = point;
			ERR_FAIL_COND_V(coordinates.size() != 3, ERR_INVALID_PARAMETER);
			for (const Variant &coordinate : coordinates) {
				ERR_FAIL_COND_V(coordinate.get_type() != Variant::FLOAT && coordinate.get_type() != Variant::INT, ERR_INVALID_PARAMETER);
			}
			Vector3 vertex(coordinates[0], coordinates[1], coordinates[2]);
			ERR_FAIL_COND_V(!vertex.is_finite(), ERR_INVALID_PARAMETER);
			faces.push_back(vertex);
		}
		String output = job.get("output", "");
		ERR_FAIL_COND_V(output.is_empty(), ERR_INVALID_PARAMETER);
		Ref<NavigationMesh> mesh;
		mesh.instantiate();
		mesh->set_cell_size(job.get("cell_size", 0.25));
		mesh->set_cell_height(job.get("cell_height", 0.25));
		mesh->set_agent_height(job.get("agent_height", 1.5));
		mesh->set_agent_radius(job.get("agent_radius", 0.5));
		mesh->set_agent_max_climb(job.get("agent_max_climb", 0.5));
		Ref<NavigationMeshSourceGeometryData3D> geometry;
		geometry.instantiate();
		geometry->add_faces(faces, Transform3D());
		NavigationServer3D::get_singleton()->bake_from_source_geometry_data(mesh, geometry);
		ERR_FAIL_COND_V_MSG(mesh->get_polygon_count() == 0, ERR_CANT_CREATE, "Navigation bake produced no polygons.");
		error = DirAccess::make_dir_recursive_absolute(settings->globalize_path(output.get_base_dir()));
		ERR_FAIL_COND_V(error != OK, error);
		error = ResourceSaver::save(mesh, output, ResourceSaver::FLAG_COMPRESS);
		ERR_FAIL_COND_V(error != OK, error);
		r_result["output"] = output;
		r_result["polygon_count"] = mesh->get_polygon_count();
		return OK;
	}
	if (operation == "import-texture") {
		String source = job.get("source", "");
		String output = job.get("output", "");
		String compression = job.get("compression", "lossless");
		ERR_FAIL_COND_V(source.is_empty() || output.is_empty(), ERR_INVALID_PARAMETER);
		PortableCompressedTexture2D::CompressionMode mode;
		if (compression == "lossless") {
			mode = PortableCompressedTexture2D::COMPRESSION_MODE_LOSSLESS;
		} else if (compression == "basisu") {
			mode = PortableCompressedTexture2D::COMPRESSION_MODE_BASIS_UNIVERSAL;
		} else if (compression == "s3tc") {
			mode = PortableCompressedTexture2D::COMPRESSION_MODE_S3TC;
		} else if (compression == "bptc") {
			mode = PortableCompressedTexture2D::COMPRESSION_MODE_BPTC;
		} else if (compression == "etc2") {
			mode = PortableCompressedTexture2D::COMPRESSION_MODE_ETC2;
		} else if (compression == "astc") {
			mode = PortableCompressedTexture2D::COMPRESSION_MODE_ASTC;
		} else {
			ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, "Unknown texture compression: " + compression);
		}
		Ref<Image> image;
		image.instantiate();
		error = image->load(source);
		ERR_FAIL_COND_V(error != OK, error);
		bool normal = job.get("normal_map", false);
		if (bool(job.get("mipmaps", true))) {
			error = image->generate_mipmaps(normal);
			ERR_FAIL_COND_V(error != OK, error);
		}
		Ref<PortableCompressedTexture2D> texture;
		texture.instantiate();
		texture->set_keep_compressed_buffer(true);
		texture->create_from_image(image, mode, normal);
		ERR_FAIL_COND_V(texture->get_width() != image->get_width() || texture->get_height() != image->get_height(), ERR_CANT_CREATE);
		error = DirAccess::make_dir_recursive_absolute(settings->globalize_path(output.get_base_dir()));
		ERR_FAIL_COND_V(error != OK, error);
		error = ResourceSaver::save(texture, output, ResourceSaver::FLAG_COMPRESS);
		ERR_FAIL_COND_V(error != OK, error);
		r_result["output"] = output;
		r_result["width"] = image->get_width();
		r_result["height"] = image->get_height();
		r_result["mipmap_count"] = image->get_mipmap_count();
		return OK;
	}
	if (operation == "import-scene") {
		String source = job.get("source", "");
		String output = job.get("output", "");
		String type = job.get("type", "PackedScene");
		ERR_FAIL_COND_V(source.is_empty() || output.is_empty(), ERR_INVALID_PARAMETER);
		String extension = source.get_extension().to_lower();
		ERR_FAIL_COND_V(extension != "gltf" && extension != "glb" && extension != "fbx" && extension != "obj", ERR_UNAVAILABLE);
		ERR_FAIL_COND_V(type != "PackedScene" && type != "ArrayMesh" && type != "AnimationLibrary" && type != "MeshLibrary", ERR_INVALID_PARAMETER);
		Ref<ResourceImporterScene> importer;
		importer.instantiate();
		importer->set_scene_import_type(type);
		List<ResourceImporter::ImportOption> definitions;
		importer->get_import_options(source, &definitions);
		HashMap<StringName, Variant> options;
		for (const ResourceImporter::ImportOption &option : definitions) {
			options[option.option.name] = option.default_value;
		}
		Dictionary overrides = job.get("options", Dictionary());
		// External subresource saves need an explicit output manifest before they can be enabled.
		ERR_FAIL_COND_V_MSG(overrides.has("_subresources") && !Dictionary(overrides["_subresources"]).is_empty(), ERR_UNAVAILABLE, "External subresource overrides are not supported by this batch protocol.");
		Array keys = overrides.keys();
		for (const Variant &key : keys) {
			StringName name = key;
			ERR_FAIL_COND_V_MSG(!options.has(name), ERR_INVALID_PARAMETER, "Unknown import option: " + String(name));
			ERR_FAIL_COND_V_MSG(!Variant::can_convert_strict(overrides[key].get_type(), options[name].get_type()), ERR_INVALID_PARAMETER, "Wrong import option type: " + String(name));
			options[name] = overrides[key];
		}
		ERR_FAIL_COND_V_MSG(String(options["import_script/path"]) != "", ERR_UNAVAILABLE, "Post-import scripts are not supported.");
		// No EditorFileSystem is present to reimport extracted images.
		options["gltf/embedded_image_handling"] = GLTFState::HANDLE_BINARY_IMAGE_MODE_EMBED_AS_UNCOMPRESSED;
		options["fbx/embedded_image_handling"] = GLTFState::HANDLE_BINARY_IMAGE_MODE_EMBED_AS_UNCOMPRESSED;
		options["fbx/importer"] = EditorSceneFormatImporterUFBX::FBX_IMPORTER_UFBX;
		String base = output.get_basename();
		error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(base.get_base_dir()));
		ERR_FAIL_COND_V(error != OK, error);
		List<String> variants;
		List<String> generated;
		Variant metadata;
		error = importer->import(ResourceUID::INVALID_ID, source, base, options, &variants, &generated, &metadata);
		ERR_FAIL_COND_V(error != OK, error);
		r_result["output"] = base + "." + importer->get_save_extension();
		return OK;
	}
	if (operation == "validate-resource" || operation == "save-resource") {
		String path = job.get("source", "");
		String expected = job.get("type", "");
		HashSet<String> dependencies;
		error = CookerFiles::collect(path, dependencies);
		ERR_FAIL_COND_V(error != OK, error);
		Ref<Resource> resource = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
		ERR_FAIL_COND_V(resource.is_null(), error == OK ? ERR_INVALID_DATA : error);
		ERR_FAIL_COND_V(!expected.is_empty() && !resource->is_class(expected), ERR_INVALID_DATA);
		r_result["type"] = resource->get_class();
		Ref<Mesh> mesh_resource = resource;
		if (mesh_resource.is_valid()) {
			int vertices = 0;
			int uv2_vertices = 0;
			int lods = 0;
			for (int i = 0; i < mesh_resource->get_surface_count(); i++) {
				lods += mesh_resource->surface_get_lods(i).size();
				Array arrays = mesh_resource->surface_get_arrays(i);
				vertices += PackedVector3Array(arrays[Mesh::ARRAY_VERTEX]).size();
				if (arrays[Mesh::ARRAY_TEX_UV2].get_type() == Variant::PACKED_VECTOR2_ARRAY) {
					uv2_vertices += PackedVector2Array(arrays[Mesh::ARRAY_TEX_UV2]).size();
				}
			}
			r_result["vertex_count"] = vertices;
			r_result["uv2_count"] = uv2_vertices;
			r_result["lod_count"] = lods;
		}
		Ref<MultiMesh> multi = resource;
		if (multi.is_valid()) {
			r_result["instance_count"] = multi->get_instance_count();
			Array positions;
			for (int i = 0; i < multi->get_instance_count(); i++) {
				Vector3 origin = multi->get_instance_transform(i).origin;
				positions.push_back(Array({ origin.x, origin.y, origin.z }));
			}
			r_result["positions"] = positions;
		}
		Ref<ShaderMaterial> shader_material = resource;
		if (shader_material.is_valid()) {
			Dictionary uniforms;
			List<PropertyInfo> properties;
			shader_material->get_property_list(&properties);
			for (const PropertyInfo &property : properties) {
				if (property.name.begins_with("shader_parameter/")) {
					uniforms[property.name.trim_prefix("shader_parameter/")] = shader_material->get(property.name);
				}
			}
			r_result["shader_parameters"] = uniforms;
		}
		Ref<NavigationMesh> navigation = resource;
		if (navigation.is_valid()) {
			r_result["polygon_count"] = navigation->get_polygon_count();
			r_result["vertex_count"] = navigation->get_vertices().size();
		}
		Ref<HeightMapShape3D> heightmap = resource;
		if (heightmap.is_valid()) {
			r_result["width"] = heightmap->get_map_width();
			r_result["depth"] = heightmap->get_map_depth();
			r_result["sample_count"] = heightmap->get_map_data().size();
		}
		Ref<Texture2D> texture = resource;
		if (texture.is_valid()) {
			r_result["width"] = texture->get_width();
			r_result["height"] = texture->get_height();
			Ref<Image> pixels = texture->get_image();
			ERR_FAIL_COND_V(pixels.is_null() || pixels->is_empty(), ERR_INVALID_DATA);
			r_result["pixel_bytes"] = pixels->get_data().size();
		}
		Ref<PackedScene> scene = resource;
		if (scene.is_valid()) {
			error = check_scene_types(scene);
			ERR_FAIL_COND_V(error != OK, error);
			Node *root = scene->instantiate();
			ERR_FAIL_NULL_V(root, ERR_CANT_CREATE);
			r_result["root_type"] = root->get_class();
			inspect_node(root, r_result);
			memdelete(root);
		}
		if (operation == "save-resource") {
			String output = job.get("output", "");
			ERR_FAIL_COND_V(output.is_empty(), ERR_INVALID_PARAMETER);
			error = DirAccess::make_dir_recursive_absolute(settings->globalize_path(output.get_base_dir()));
			ERR_FAIL_COND_V(error != OK, error);
			error = ResourceSaver::save(resource, output, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_OMIT_EDITOR_PROPERTIES);
			ERR_FAIL_COND_V(error != OK, error);
			r_result["output"] = output;
		}
		return OK;
	}
	ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, "Unknown operation: " + operation);
}
} // namespace

bool Main::is_cmdline_tool() { return true; }
const Vector<String> &Main::get_forwardable_cli_arguments(CLIScope p_scope) {
	static const Vector<String> empty;
	return empty;
}
int Main::test_entrypoint(int argc, char *argv[], bool &tests_need_run) {
	tests_need_run = false;
	return EXIT_SUCCESS;
}
String Main::get_locale_override() { return "en"; }
bool Main::iteration() { return true; }
bool Main::is_iterating() { return false; }
void Main::force_redraw() {}
void Main::setup_boot_logo() {}
Error Main::setup2(bool p_show_boot_logo) { return OK; }

Error Main::setup(const char *execpath, int argc, char *argv[], bool p_second_phase) {
	Thread::make_main_thread();
	set_current_thread_safe_for_nodes(true);
	OS::get_singleton()->initialize();
	CoreGlobals::print_ready = true;
	engine = memnew(Engine);
	engine->set_editor_hint(true);
	register_core_types();
	register_core_driver_types();
	packed_data = memnew(PackedData);
	settings = memnew(ProjectSettings);
	register_core_settings();
	performance = memnew(Performance);
	input_map = memnew(InputMap);
	input = memnew(Input);
	translations = memnew(TranslationServer);
	text_servers = memnew(TextServerManager);
	Ref<TextServerDummy> text;
	text.instantiate();
	text_servers->add_interface(text);
	text_servers->set_primary_interface(text);
	physics_manager = memnew(PhysicsServer3DManager);
	NavigationServer3DManager::initialize_server_manager();
	register_early_core_singletons();
	initialize_modules(MODULE_INITIALIZATION_LEVEL_CORE);
	register_core_extensions();
	register_core_singletons();
	WorkerThreadPool::get_singleton()->init(8);
	register_server_types();
	initialize_modules(MODULE_INITIALIZATION_LEVEL_SERVERS);
	GDExtensionManager::get_singleton()->initialize_extensions(GDExtension::INITIALIZATION_LEVEL_SERVERS);
	messages = memnew(MessageQueue);
	RasterizerDummy::make_current();
	renderer = memnew(RenderingServerDefault);
	renderer->init();
	renderer->set_render_loop_enabled(false);
	theme_db = memnew(ThemeDB);
	NavigationServer3DManager::initialize_server();
	physics = physics_manager->new_default_server();
	ERR_FAIL_NULL_V(physics, ERR_CANT_CREATE);
	physics->init();
	register_scene_types();
	register_driver_types();
	register_scene_singletons();
	initialize_modules(MODULE_INITIALIZATION_LEVEL_SCENE);
	GDExtensionManager::get_singleton()->initialize_extensions(GDExtension::INITIALIZATION_LEVEL_SCENE);
	GDREGISTER_CLASS(EditorSceneFormatImporter);
	GDREGISTER_CLASS(EditorScenePostImport);
	GDREGISTER_CLASS(EditorScenePostImportPlugin);
	GDREGISTER_CLASS(ResourceImporterScene);
	GDREGISTER_CLASS(EditorSceneFormatImporterGLTF);
	GDREGISTER_CLASS(EditorSceneFormatImporterUFBX);
	GDREGISTER_CLASS(EditorOBJImporter);
	Ref<EditorSceneFormatImporterGLTF> gltf;
	gltf.instantiate();
	ResourceImporterScene::add_scene_importer(gltf);
	Ref<EditorSceneFormatImporterUFBX> fbx;
	fbx.instantiate();
	ResourceImporterScene::add_scene_importer(fbx);
	Ref<EditorOBJImporter> obj;
	obj.instantiate();
	ResourceImporterScene::add_scene_importer(obj);
	register_platform_apis();
	ClassDB::set_current_api(ClassDB::API_NONE);
	initialized = true;
	command = "capabilities";
	String project_path = ".";
	for (int i = 0; i < argc; i++) {
		String arg = String::utf8(argv[i]);
		if (arg == "--capabilities") {
			command = "capabilities";
		} else if ((arg == "--path" || arg == "--job") && i + 1 < argc) {
			String value = String::utf8(argv[++i]);
			if (arg == "--path") {
				project_path = value;
			} else {
				job_path = value;
				command = "job";
			}
		} else if (arg == "--headless" || arg == "--editor" || arg == "--quit") {
			// Always headless, always tools-enabled, always exits after one batch.
		} else {
			ERR_PRINT("Unsupported Cooker argument: " + arg);
			command = "invalid";
		}
	}
	if (command == "job") {
		Error error = settings->setup(project_path, "", false, true);
		if (error != OK) {
			command = "invalid";
			ERR_PRINT("Cooker requires an existing project.godot in --path.");
		}
	}
	return OK;
}

int Main::start() {
	Dictionary result;
	Error error = OK;
	if (command == "capabilities") {
		result = capabilities();
	} else if (command == "job") {
		error = execute_job(result);
	} else {
		error = ERR_INVALID_PARAMETER;
	}
	result["ok"] = error == OK;
	result["error"] = int(error);
	print_line(JSON::stringify(result));
	OS::get_singleton()->set_exit_code(error == OK ? EXIT_SUCCESS : EXIT_FAILURE);
	return EXIT_SUCCESS; // No MainLoop; OS::run returns immediately and preserves the job's exit code.
}

void Main::cleanup(bool p_force) {
	if (!initialized) {
		return;
	}
	command = String();
	job_path = String();
	ResourceImporterScene::clean_up_importer_plugins();
	ResourceLoader::remove_custom_loaders();
	ResourceSaver::remove_custom_savers();
	PropertyListHelper::clear_base_helpers();
	messages->flush();
	GDExtensionManager::get_singleton()->deinitialize_extensions(GDExtension::INITIALIZATION_LEVEL_SCENE);
	uninitialize_modules(MODULE_INITIALIZATION_LEVEL_SCENE);
	unregister_platform_apis();
	unregister_driver_types();
	unregister_scene_types();
	memdelete(theme_db);
	physics->finish();
	memdelete(physics);
	NavigationServer3DManager::finalize_server();
	NavigationServer3DManager::finalize_server_manager();
	renderer->sync();
	renderer->global_shader_parameters_clear();
	renderer->finish();
	memdelete(renderer);
	messages->flush();
	memdelete(messages);
	GDExtensionManager::get_singleton()->deinitialize_extensions(GDExtension::INITIALIZATION_LEVEL_SERVERS);
	uninitialize_modules(MODULE_INITIALIZATION_LEVEL_SERVERS);
	unregister_server_types();
	EngineDebugger::deinitialize();
	OS::get_singleton()->finalize();
	memdelete(packed_data);
	memdelete(translations);
	memdelete(text_servers);
	memdelete(physics_manager);
	memdelete(input);
	memdelete(input_map);
	memdelete(performance);
	memdelete(settings);
	unregister_core_driver_types();
	unregister_core_extensions();
	uninitialize_modules(MODULE_INITIALIZATION_LEVEL_CORE);
	memdelete(engine);
	unregister_core_types();
	OS::get_singleton()->finalize_core();
	CoreGlobals::print_ready = false;
	Thread::release_main_thread();
	initialized = false;
}
/*>>----- VEYA_COOKER */
