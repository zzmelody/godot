/*<<----- VEYA_COOKER: shared immutable render inputs for bounded static fragments. */
#pragma once
#include "asset_files.h"
#include "atomic_save.h"
#include "output_lock.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/packed_scene.h"

namespace CookerRender {
struct PathOrder {
 bool operator()(const Ref<Resource> &a, const Ref<Resource> &b) const { return a->get_path() < b->get_path(); }
};
inline Error save(const Ref<PackedScene> &p_scene, const String &p_directory, Array &r_outputs, uint64_t p_limit) {
	ERR_FAIL_COND_V(p_scene.is_null(), ERR_INVALID_PARAMETER);
	Node *root = p_scene->instantiate();
	ERR_FAIL_NULL_V(root, ERR_INVALID_DATA);
	Vector<Node *> pending;
	pending.push_back(root);
	HashMap<ObjectID, Ref<Resource>> resources;
	while (!pending.is_empty()) {
		Node *node = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		for (int i = 0; i < node->get_child_count(); ++i) pending.push_back(node->get_child(i));
		Ref<Mesh> mesh;
		if (auto *batch = Object::cast_to<MultiMeshInstance3D>(node); batch && batch->get_multimesh().is_valid()) mesh = batch->get_multimesh()->get_mesh();
		if (auto *instance = Object::cast_to<MeshInstance3D>(node)) {
			mesh = instance->get_mesh();
			for (int i = 0; mesh.is_valid() && i < mesh->get_surface_count(); ++i) {
				const Ref<Material> material = instance->get_surface_override_material(i);
				if (material.is_valid()) resources[material->get_instance_id()] = material;
			}
		}
		if (auto *geometry = Object::cast_to<GeometryInstance3D>(node)) {
			for (const Ref<Material> &material : {geometry->get_material_override(), geometry->get_material_overlay()}) {
				if (material.is_valid()) resources[material->get_instance_id()] = material;
			}
		}
		if (mesh.is_valid()) {
			resources[mesh->get_instance_id()] = mesh;
			for (int i = 0; i < mesh->get_surface_count(); ++i) {
				const Ref<Material> material = mesh->surface_get_material(i);
				if (material.is_valid()) resources[material->get_instance_id()] = material;
			}
		}
	}
	memdelete(root);
	ERR_FAIL_COND_V(resources.size() > 256, ERR_INVALID_DATA);
	Vector<Ref<Resource>> ordered;
	for (const KeyValue<ObjectID, Ref<Resource>> &entry : resources) ordered.push_back(entry.value);
	ordered.sort_custom<PathOrder>();
	const String directory = p_directory.path_join("render-resources");
	Error error = DirAccess::make_dir_recursive_absolute(directory);
	ERR_FAIL_COND_V(error != OK, error);
	uint64_t total_bytes = 0;
	// Publish materials first. Meshes then reference them instead of embedding
	// another copy of their shaders and textures in each fragment.
	for (int phase = 0; phase < 2; ++phase) {
		for (const Ref<Resource> &resource : ordered) {
			const Ref<Material> material = resource;
			if (material.is_valid() != (phase == 0)) continue;
			String path = resource->get_path();
			if (!path.begins_with(directory + "/")) {
				// Recipe-created materials and meshes can have no source path.
				// A fixed staging path keeps serialization independent of ObjectID
				// and discovery order; the saved bytes provide their final identity.
				const String stem = path.is_empty() ? (material.is_valid() ? "anonymous-material" : "anonymous-mesh") : path.sha256_text();
				const String temporary = directory.path_join(stem + ".partial.res");
				CookerOutputLock::Guard lock(temporary);
				ERR_FAIL_COND_V(!lock.acquired(), ERR_BUSY);
				CookerAtomicSave::discard_stale(temporary);
				error = ResourceSaver::save(resource, temporary, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
				ERR_FAIL_COND_V(error != OK, error);
				const Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::READ);
				ERR_FAIL_COND_V(file.is_null(), ERR_FILE_CANT_READ);
				total_bytes += file->get_length();
				ERR_FAIL_COND_V(total_bytes > p_limit, ERR_OUT_OF_MEMORY);
				file->close();
				path = directory.path_join(FileAccess::get_md5(temporary) + ".res");
				if (FileAccess::exists(path)) {
					if (FileAccess::get_sha256(path) != FileAccess::get_sha256(temporary)) {
						CookerAtomicSave::discard_stale(temporary);
						return ERR_INVALID_DATA;
					}
					CookerAtomicSave::discard_stale(temporary);
				} else {
					error = CookerAtomicSave::publish(temporary, path);
					ERR_FAIL_COND_V(error != OK, error);
				}
				resource->set_path(path, true);
			}
			r_outputs.push_back(path);
		}
	}
	return OK;
}
} // namespace CookerRender
/*>>----- VEYA_COOKER */
