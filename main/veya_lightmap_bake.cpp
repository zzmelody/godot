#include "veya_lightmap_bake.h"

#if defined(TOOLS_ENABLED) && !defined(_3D_DISABLED)
#include "cooker/asset_files.h"
#include "core/io/json.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "core/version.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

namespace {
class BakeTree : public SceneTree {
	String source, output;
	Error cook(Dictionary &report) {
		ERR_FAIL_COND_V(RenderingDevice::get_singleton() == nullptr, ERR_UNAVAILABLE);
		Error error = CookerFiles::check_path(source);
		ERR_FAIL_COND_V(error != OK || !CookerFiles::is_generated(source) || source.get_extension() != "scn", ERR_INVALID_PARAMETER);
		error = CookerFiles::check_path(output, true);
		ERR_FAIL_COND_V(error != OK || output.get_extension() != "scn" || source == output || FileAccess::exists(output), ERR_INVALID_PARAMETER);
		HashSet<String> dependencies;
		error = CookerFiles::collect(source, dependencies);
		ERR_FAIL_COND_V(error != OK, error);
		Ref<PackedScene> packed = ResourceLoader::load(source);
		ERR_FAIL_COND_V(packed.is_null(), ERR_INVALID_DATA);
		Node *root = packed->instantiate();
		ERR_FAIL_COND_V(root == nullptr, ERR_CANT_CREATE);
		get_root()->add_child(root);
		Vector<Node *> nodes; nodes.push_back(root);
		Vector<MeshInstance3D *> meshes;
		Vector<Node *> bake_lights;
		LightmapGI *gi = nullptr;
		bool valid = Object::cast_to<Node3D>(root) != nullptr;
		uint64_t vertices = 0;
		for (int i = 0; i < nodes.size() && valid; i++) {
			Node *node = nodes[i];
			valid = node->get_script().get_type() == Variant::NIL && nodes.size() <= 10000;
			if (auto *lightmap = Object::cast_to<LightmapGI>(node)) {
				valid = valid && gi == nullptr && lightmap->get_light_data().is_null(); gi = lightmap;
			} else if (auto *mesh = Object::cast_to<MeshInstance3D>(node)) {
				valid = valid && mesh->get_mesh().is_valid() && mesh->get_skin().is_null() && mesh->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC;
			if (valid) {
					meshes.push_back(mesh);
					for (int surface = 0; surface < mesh->get_mesh()->get_surface_count(); surface++) vertices += mesh->get_mesh()->surface_get_array_len(surface);
					valid = vertices <= 500000;
				}
			} else if (auto *light = Object::cast_to<OmniLight3D>(node)) {
				valid = valid && light->has_meta("_veya_cooker_bake_only") && light->get_bake_mode() == Light3D::BAKE_STATIC;
				bake_lights.push_back(light);
			} else valid = valid && node->get_class() == "Node3D";
			for (int child = 0; child < node->get_child_count(); child++) nodes.push_back(node->get_child(child));
		}
		if (!valid || !gi || meshes.is_empty() || bake_lights.size() > 32 || gi->get_max_texture_size() > 4096 || gi->get_generate_probes() > LightmapGI::GENERATE_PROBES_SUBDIV_16) {
			get_root()->remove_child(root); memdelete(root); return ERR_INVALID_DATA;
		}
		// UV2 is unique per transformed architectural mesh, including scale.
		const float texel_size = float(gi->get_meta("_veya_cooker_texel_size", .15));
		if (!Math::is_finite(texel_size) || texel_size < .025 || texel_size > 1) {
			get_root()->remove_child(root); memdelete(root); return ERR_INVALID_DATA;
		}
		for (MeshInstance3D *node : meshes) {
			Ref<ImporterMesh> mesh = ImporterMesh::from_mesh(node->get_mesh());
			Vector<uint8_t> cache, unwrapped;
			error = mesh.is_valid() ? mesh->lightmap_unwrap_cached(gi->get_global_transform().affine_inverse() * node->get_global_transform(), texel_size, cache, unwrapped) : ERR_INVALID_DATA;
			if (error != OK) break;
			Ref<ArrayMesh> result = mesh->get_mesh(); result->set_path(String()); node->set_mesh(result);
		}
		const String data_path = output.get_basename() + ".bake-tmp.res";
		const String scene_path = output.get_basename() + ".bake-tmp.scn";
		const bool collision = FileAccess::exists(data_path) || FileAccess::exists(scene_path);
		if (collision) error = ERR_ALREADY_EXISTS;
		if (error == OK) {
			gi->set_meta("_veya_cooker_portable_bake", true);
			RenderingServer::get_singleton()->sync();
			const auto result = gi->bake(root, data_path);
			report["bake_error"] = int(result);
			if (result != LightmapGI::BAKE_ERROR_OK) error = ERR_CANT_CREATE;
		}
		if (error == OK) {
			const Ref<LightmapGIData> data = gi->get_light_data();
			report["meshes"] = meshes.size(); report["users"] = data->get_user_count();
			report["texture_arrays"] = data->get_lightmap_textures().size(); report["probes"] = data->get_capture_points().size();
			data->set_path(String()); gi->remove_meta("_veya_cooker_portable_bake"); gi->remove_meta("_veya_cooker_texel_size");
			for (Node *light : bake_lights) { light->get_parent()->remove_child(light); memdelete(light); }
			// Flatten external scene ownership so new UV2 meshes and bindings are
			// saved into this output, rather than discarded as instance changes.
			for (Node *node : nodes) {
				if (bake_lights.has(node)) continue;
				node->set_scene_file_path(String());
				if (node != root) node->set_owner(root);
			}
			packed.instantiate(); error = packed->pack(root);
			if (error == OK) error = ResourceSaver::save(packed, scene_path, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
			if (error == OK) error = DirAccess::rename_absolute(scene_path, output);
		}
		get_root()->remove_child(root); memdelete(root);
		if (!collision && FileAccess::exists(data_path)) DirAccess::remove_absolute(data_path);
		if (!collision && FileAccess::exists(scene_path)) DirAccess::remove_absolute(scene_path);
		return error;
	}
public:
	BakeTree(String p_source, String p_output) : source(p_source), output(p_output) {}
	void initialize() override {
		SceneTree::initialize();
		Dictionary report; report["engine_revision"] = GODOT_VERSION_HASH;
		report["renderer"] = "forward_plus";
		const Error error = cook(report); report["error"] = int(error);
		if (error == OK) report["output_sha256"] = FileAccess::get_sha256(output);
		print_line("VEYA_LIGHTMAP_RESULT=" + JSON::stringify(report));
		quit(error == OK ? 0 : 1);
	}
};
}

bool VeyaLightmapBake::start(MainLoop *&r_loop, int &r_status) {
	String source, output; bool requested = false;
	for (const String &arg : OS::get_singleton()->get_cmdline_user_args()) {
		if (arg.begins_with("--veya-bake-lightmap=")) { source = arg.trim_prefix("--veya-bake-lightmap="); requested = true; }
		if (arg.begins_with("--veya-bake-output=")) { output = arg.trim_prefix("--veya-bake-output="); requested = true; }
	}
	if (!requested) return false;
	if (source.is_empty() || output.is_empty()) { r_status = EXIT_FAILURE; return true; }
	r_loop = memnew(BakeTree(source, output));
	r_status = EXIT_SUCCESS; return true;
}
#else
bool VeyaLightmapBake::start(MainLoop *&, int &) { return false; }
#endif
