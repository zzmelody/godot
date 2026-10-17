/*<<----- VEYA_COOKER: shared immutable render inputs for bounded static fragments. */
#pragma once
#include "asset_files.h"
#include "atomic_save.h"
#include "output_lock.h"
#include "virtual_geometry_mesh.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"

namespace CookerRender {
struct PathOrder {
 bool operator()(const Ref<Resource> &a, const Ref<Resource> &b) const { return a->get_path() < b->get_path(); }
};
inline Error save(const Ref<PackedScene> &p_scene, const String &p_directory, Array &r_outputs, uint64_t p_limit,
		const std::map<ObjectID, CookerVirtualGeometry::BuildOutput> *p_geometry_builds = nullptr) {
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
			const Ref<VirtualGeometryMesh> geometry = mesh;
			// Published standalone roots retain their material/container ownership.
			// Dependent fragments borrow them instead of republishing their pages.
			if (geometry.is_valid() && !geometry->is_built_in() && !geometry->get_path().is_empty() &&
					CookerFiles::is_generated(geometry->get_path()) && FileAccess::exists(geometry->get_path()) &&
					(p_geometry_builds == nullptr || p_geometry_builds->count(geometry->get_instance_id()) == 0)) {
				continue;
			}
			resources[mesh->get_instance_id()] = mesh;
			for (int i = 0; i < mesh->get_surface_count(); ++i) {
				const Ref<Material> material = mesh->surface_get_material(i);
				if (material.is_valid()) resources[material->get_instance_id()] = material;
			}
		}
	}
	memdelete(root);
	Vector<Ref<Shader>> shaders;
	for (const KeyValue<ObjectID, Ref<Resource>> &entry : resources) {
		const Ref<ShaderMaterial> material = entry.value;
		if (material.is_valid() && material->get_shader().is_valid()) shaders.push_back(material->get_shader());
	}
	for (const Ref<Shader> &shader : shaders) resources[shader->get_instance_id()] = shader;
	ERR_FAIL_COND_V(resources.size() > 256, ERR_INVALID_DATA);
	Vector<Ref<Resource>> ordered;
	for (const KeyValue<ObjectID, Ref<Resource>> &entry : resources) ordered.push_back(entry.value);
	ordered.sort_custom<PathOrder>();
	const String directory = p_directory.path_join("render-resources");
	Error error = DirAccess::make_dir_recursive_absolute(directory);
	ERR_FAIL_COND_V(error != OK, error);
	uint64_t total_bytes = 0;
	HashSet<ObjectID> published_geometry;
	HashSet<String> published_containers;
	auto publish_geometry = [&](const Ref<Resource> &p_resource, const String &p_path) -> Error {
		if (Ref<VirtualGeometryMesh>(p_resource).is_null() || published_geometry.has(p_resource->get_instance_id())) {
			return OK;
		}
		if (total_bytes > p_limit) {
			return ERR_OUT_OF_MEMORY;
		}
		Array containers;
		const std::map<ObjectID, CookerVirtualGeometry::BuildOutput> empty;
		const Error result = CookerVirtualGeometry::publish_resource_containers(p_resource,
				p_geometry_builds != nullptr ? *p_geometry_builds : empty, p_path, p_limit - total_bytes, containers);
		if (result != OK) {
			return result;
		}
		for (const Variant &entry : containers) {
			const Dictionary record = entry;
			const String path = record["path"];
			if (!published_containers.has(path)) {
				published_containers.insert(path);
				total_bytes += uint64_t(int64_t(record["bytes"]));
				r_outputs.push_back(path);
			}
		}
		published_geometry.insert(p_resource->get_instance_id());
		return OK;
	};
	HashMap<String, Ref<Resource>> canonical;
	HashMap<ObjectID, Ref<Resource>> aliases;
	// Publish shaders, then materials, then meshes. Scenes can reuse the same
	// shader instead of parsing and compiling a private copy per material.
	for (int phase = 0; phase < 3; ++phase) {
		for (const Ref<Resource> &resource : ordered) {
			const Ref<Material> material = resource;
			const Ref<Shader> shader = resource;
			const int resource_phase = shader.is_valid() ? 0 : material.is_valid() ? 1 : 2;
			if (resource_phase != phase) continue;
			if (const Ref<ShaderMaterial> shader_material = resource; shader_material.is_valid()) {
				const Ref<Shader> original = shader_material->get_shader();
				if (original.is_valid() && aliases.has(original->get_instance_id())) shader_material->set_shader(aliases[original->get_instance_id()]);
			}
			if (const Ref<Mesh> mesh = resource; mesh.is_valid()) {
				for (int surface = 0; surface < mesh->get_surface_count(); ++surface) {
					const Ref<Material> original = mesh->surface_get_material(surface);
					if (original.is_valid() && aliases.has(original->get_instance_id())) mesh->surface_set_material(surface, aliases[original->get_instance_id()]);
				}
			}
			String path = resource->get_path();
			if (!path.begins_with(directory + "/")) {
				// Recipe-created materials and meshes can have no source path.
				// A fixed staging path keeps serialization independent of ObjectID
				// and discovery order; the saved bytes provide their final identity.
				const String stem = path.is_empty() ? (shader.is_valid() ? "anonymous-shader" : material.is_valid() ? "anonymous-material" : "anonymous-mesh") : path.sha256_text();
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
				error = publish_geometry(resource, path);
				ERR_FAIL_COND_V(error != OK, error);
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
			}
			error = publish_geometry(resource, path);
			ERR_FAIL_COND_V(error != OK, error);
			if (canonical.has(path)) {
				aliases[resource->get_instance_id()] = canonical[path];
			} else {
				resource->set_path(path, true);
				canonical[path] = resource;
				aliases[resource->get_instance_id()] = resource;
			}
			r_outputs.push_back(path);
		}
	}
	// set_path(take_over=true) on two equivalent objects clears the first
	// object's path. Rebind the scene to one owner instead of embedding it again.
	root = p_scene->instantiate();
	ERR_FAIL_NULL_V(root, ERR_INVALID_DATA);
	pending.push_back(root);
	while (!pending.is_empty()) {
		Node *node = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		for (int child = 0; child < node->get_child_count(); ++child) pending.push_back(node->get_child(child));
		if (auto *instance = Object::cast_to<MeshInstance3D>(node)) {
			const Ref<Mesh> original = instance->get_mesh();
			if (original.is_valid() && aliases.has(original->get_instance_id())) instance->set_mesh(aliases[original->get_instance_id()]);
			for (int surface = 0; surface < instance->get_surface_override_material_count(); ++surface) {
				const Ref<Material> original_material = instance->get_surface_override_material(surface);
				if (original_material.is_valid() && aliases.has(original_material->get_instance_id())) instance->set_surface_override_material(surface, aliases[original_material->get_instance_id()]);
			}
		}
		if (auto *batch = Object::cast_to<MultiMeshInstance3D>(node); batch && batch->get_multimesh().is_valid()) {
			const Ref<Mesh> original = batch->get_multimesh()->get_mesh();
			if (original.is_valid() && aliases.has(original->get_instance_id())) batch->get_multimesh()->set_mesh(aliases[original->get_instance_id()]);
		}
		if (auto *geometry = Object::cast_to<GeometryInstance3D>(node)) {
			const Ref<Material> original = geometry->get_material_override();
			if (original.is_valid() && aliases.has(original->get_instance_id())) geometry->set_material_override(aliases[original->get_instance_id()]);
			const Ref<Material> overlay = geometry->get_material_overlay();
			if (overlay.is_valid() && aliases.has(overlay->get_instance_id())) geometry->set_material_overlay(aliases[overlay->get_instance_id()]);
		}
	}
	error = p_scene->pack(root);
	memdelete(root);
	ERR_FAIL_COND_V(error != OK, error);
	return OK;
}
} // namespace CookerRender
/*>>----- VEYA_COOKER */
