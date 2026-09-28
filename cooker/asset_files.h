/*<<----- VEYA_COOKER: bounded workspace paths and dependency-closed, unsigned asset packs. */
#pragma once

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/pck_packer.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "scene/resources/shader.h"
#include "scene/resources/shader_include.h"
#include "servers/rendering/shader_preprocessor.h"

#include <filesystem>

namespace CookerFiles {
// This is a workspace boundary, not a sandbox for hostile native asset parsers.
inline Error check_path(const String &p_path, bool p_output = false, bool p_pack = false, bool p_require_exists = true) {
	ERR_FAIL_COND_V_MSG(!p_path.begins_with("res://"), ERR_INVALID_PARAMETER, "Asset paths must start with res://.");
	String relative = p_path.trim_prefix("res://");
	ERR_FAIL_COND_V(relative.is_empty() || relative.contains(":") || relative.contains("\\") || relative.begins_with("/"), ERR_INVALID_PARAMETER);
	for (const String &part : relative.split("/")) {
		ERR_FAIL_COND_V(part.is_empty() || part == "." || part == ".." || part.ends_with(".") || part.ends_with(" "), ERR_INVALID_PARAMETER);
	}
	if (p_output) {
		ERR_FAIL_COND_V_MSG(!relative.begins_with(p_pack ? "content/releases/" : "assets/generated/"), ERR_UNAUTHORIZED, "Output is outside the generated asset namespace.");
	}
	std::error_code ec;
	auto root = std::filesystem::weakly_canonical(std::filesystem::u8path(ProjectSettings::get_singleton()->globalize_path("res://").utf8().get_data()), ec);
	ERR_FAIL_COND_V(bool(ec), ERR_FILE_BAD_PATH);
	auto actual = std::filesystem::weakly_canonical(root / std::filesystem::u8path(relative.utf8().get_data()), ec);
	ERR_FAIL_COND_V(bool(ec), ERR_FILE_BAD_PATH);
	auto within = actual.lexically_relative(root);
	ERR_FAIL_COND_V_MSG(within.empty() || *within.begin() == "..", ERR_UNAUTHORIZED, "Asset path escapes the workspace through a link.");
	if (!p_output && p_require_exists) {
		ERR_FAIL_COND_V_MSG(!FileAccess::exists(p_path), ERR_FILE_NOT_FOUND, "Missing asset: " + p_path);
	}
	return OK;
}

inline bool is_resource(const String &p_path) {
	String extension = p_path.get_extension().to_lower();
	return extension == "tres" || extension == "tscn" || extension == "res" || extension == "scn" || extension == "mesh" || extension == "material" || extension == "anim" || extension == "gdshader" || extension == "gdshaderinc";
}

inline Error collect(const String &p_path, HashSet<String> &r_files, bool p_pack = false, int p_depth = 0) {
	ERR_FAIL_COND_V(p_depth > 64 || r_files.size() > 4096, ERR_PARAMETER_RANGE_ERROR);
	Error error = check_path(p_path);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V_MSG(!is_resource(p_path), ERR_UNAVAILABLE, "Only cooked or declarative resource dependencies are supported: " + p_path);
	ERR_FAIL_COND_V_MSG(p_pack && !p_path.begins_with("res://assets/generated/"), ERR_UNAUTHORIZED, "Pack dependencies must be cooked into assets/generated/ first: " + p_path);
	if (r_files.has(p_path)) {
		return OK;
	}
	r_files.insert(p_path);
	HashSet<StringName> classes;
	ResourceLoader::get_classes_used(p_path, &classes);
	String primary_type = ResourceLoader::get_resource_type(p_path);
	if (!primary_type.is_empty()) {
		classes.insert(primary_type);
	}
	for (const StringName &name : classes) {
		// The upstream text scanner reports an empty [resource] res_type in addition to the header type.
		if (name.is_empty()) {
			continue;
		}
		ERR_FAIL_COND_V_MSG(!ClassDB::class_exists(name) || name == StringName("GDExtension") || ClassDB::is_parent_class(name, "Script"), ERR_UNAVAILABLE, "Unsupported asset class: " + String(name));
	}
	List<String> dependencies;
	ResourceLoader::get_dependencies(p_path, &dependencies, true);
	for (const String &dependency : dependencies) {
		Vector<String> fields = dependency.split("::");
		String path = fields[0];
		if (path.begins_with("uid://")) {
			ERR_FAIL_COND_V_MSG(fields.size() < 3, ERR_INVALID_DATA, "Dependency requires a stable fallback path.");
			path = fields[2];
		}
		if (path.is_relative_path()) {
			path = p_path.get_base_dir().path_join(path).simplify_path();
		}
		error = collect(path, r_files, p_pack, p_depth + 1);
		ERR_FAIL_COND_V(error != OK, error);
	}
	return OK;
}

inline Error collect_shader_includes(const Variant &p_value, HashSet<ObjectID> &r_visited, HashSet<String> &r_files, int p_depth = 0) {
	ERR_FAIL_COND_V(p_depth > 128 || r_visited.size() > 16384, ERR_PARAMETER_RANGE_ERROR);
	if (p_value.get_type() == Variant::ARRAY) {
		for (const Variant &value : Array(p_value)) {
			Error error = collect_shader_includes(value, r_visited, r_files, p_depth + 1);
			ERR_FAIL_COND_V(error != OK, error);
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		Array values = Dictionary(p_value).values();
		return collect_shader_includes(values, r_visited, r_files, p_depth + 1);
	} else if (p_value.get_type() == Variant::OBJECT) {
		Ref<Resource> resource = p_value;
		if (resource.is_null() || r_visited.has(resource->get_instance_id())) {
			return OK;
		}
		r_visited.insert(resource->get_instance_id());
		Ref<Shader> shader = resource;
		if (shader.is_valid()) {
			ShaderPreprocessor preprocessor;
			String expanded;
			HashSet<Ref<ShaderInclude>> includes;
			Error error = preprocessor.preprocess(shader->get_code(), shader->get_path(), expanded, nullptr, nullptr, nullptr, &includes);
			ERR_FAIL_COND_V(error != OK, error);
			for (const Ref<ShaderInclude> &include : includes) {
				error = collect(include->get_path(), r_files, true);
				ERR_FAIL_COND_V(error != OK, error);
			}
		}
		List<PropertyInfo> properties;
		resource->get_property_list(&properties);
		for (const PropertyInfo &property : properties) {
			if (property.usage & PROPERTY_USAGE_STORAGE) {
				Error error = collect_shader_includes(resource->get(property.name), r_visited, r_files, p_depth + 1);
				ERR_FAIL_COND_V(error != OK, error);
			}
		}
	}
	return OK;
}

// Directory roots are declared by Luau; enumeration only transports existing
// cooked resources. It does not infer source art, import or generate content.
inline Error collect_directory(const String &p_path, Array &r_roots, HashSet<String> &r_metadata, int &r_directories, int p_depth = 0) {
	ERR_FAIL_COND_V(p_depth > 16 || ++r_directories > 1024, ERR_PARAMETER_RANGE_ERROR);
	ERR_FAIL_COND_V(!p_path.begins_with("res://assets/generated/"), ERR_UNAUTHORIZED);
	Error error = check_path(p_path, false, false, false);
	ERR_FAIL_COND_V(error != OK, error);
	Ref<DirAccess> directory = DirAccess::open(p_path, &error);
	ERR_FAIL_COND_V(directory.is_null(), error == OK ? ERR_FILE_NOT_FOUND : error);
	// Reject links even when they point back inside the project: no recursion
	// cycles or publication dependent on another mutable directory's lifetime.
	String prefix = "res://";
	for (const String &part : p_path.trim_prefix("res://").split("/")) {
		prefix = prefix.path_join(part);
		ERR_FAIL_COND_V(directory->is_link(ProjectSettings::get_singleton()->globalize_path(prefix)), ERR_UNAUTHORIZED);
	}
	error = directory->list_dir_begin();
	ERR_FAIL_COND_V(error != OK, error);
	Vector<String> children;
	for (String name = directory->get_next(); !name.is_empty(); name = directory->get_next()) {
		if (name == "." || name == "..") continue;
		if (children.size() >= 4096 || directory->is_link(name)) { directory->list_dir_end(); return ERR_PARAMETER_RANGE_ERROR; }
		children.push_back(name);
	}
	directory->list_dir_end();
	children.sort();
	for (const String &name : children) {
		const String path = p_path.path_join(name);
		if (directory->dir_exists(name)) {
			error = collect_directory(path, r_roots, r_metadata, r_directories, p_depth + 1);
			ERR_FAIL_COND_V(error != OK, error);
		} else if (name.ends_with(".scn") || name.ends_with(".res")) {
			r_roots.push_back(path);
		} else if (name == "asset.manifest.json") {
			error = check_path(path);
			ERR_FAIL_COND_V(error != OK, error);
			ERR_FAIL_COND_V(FileAccess::get_file_as_bytes(path).size() > 1024 * 1024, ERR_PARAMETER_RANGE_ERROR);
			r_metadata.insert(path);
		}
		ERR_FAIL_COND_V(r_roots.size() + r_metadata.size() > 4096, ERR_PARAMETER_RANGE_ERROR);
	}
	return OK;
}

inline Error pack(const Dictionary &p_job, Dictionary &r_result) {
	ERR_FAIL_COND_V(p_job.has("files") && p_job["files"].get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_job.has("directories") && p_job["directories"].get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
	Array roots = Array(p_job.get("files", Array())).duplicate();
	const Array directories = p_job.get("directories", Array());
	ERR_FAIL_COND_V(roots.size() > 4096 || directories.size() > 32, ERR_PARAMETER_RANGE_ERROR);
	HashSet<String> metadata;
	int directory_count = 0;
	for (const Variant &directory : directories) {
		ERR_FAIL_COND_V(directory.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		Error error = collect_directory(directory, roots, metadata, directory_count);
		ERR_FAIL_COND_V(error != OK, error);
	}
	ERR_FAIL_COND_V(roots.is_empty(), ERR_INVALID_PARAMETER);
	String output = p_job.get("output", "");
	Error error = check_path(output, true, true);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V(output.get_extension() != "pck", ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(FileAccess::exists(output), ERR_ALREADY_EXISTS, "Use a new release path; existing packs are immutable.");
	HashSet<String> closure;
	HashSet<ObjectID> visited_resources;
	Vector<Ref<Resource>> loaded_roots; // Keep visited ObjectIDs alive through the traversal.
	for (const Variant &root : roots) {
		ERR_FAIL_COND_V(root.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		error = collect(root, closure, true);
		ERR_FAIL_COND_V(error != OK, error);
		Ref<Resource> resource = ResourceLoader::load(root, "", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
		ERR_FAIL_COND_V(resource.is_null(), error == OK ? ERR_INVALID_DATA : error);
		loaded_roots.push_back(resource);
		error = collect_shader_includes(resource, visited_resources, closure);
		ERR_FAIL_COND_V(error != OK, error);
	}
	for (const String &path : metadata) closure.insert(path);
	// Test/preview declarations are opaque bytes and require a separate explicit
	// debug gate. Normal asset releases cannot accidentally include test scripts.
	ERR_FAIL_COND_V(p_job.has("debug_files") && p_job["debug_files"].get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
	const Array debug_files = p_job.get("debug_files", Array());
	ERR_FAIL_COND_V(debug_files.size() > 16, ERR_PARAMETER_RANGE_ERROR);
	if (!debug_files.is_empty()) {
		ERR_FAIL_COND_V(p_job.get("debug", Variant()).get_type() != Variant::BOOL || !bool(p_job["debug"]), ERR_UNAUTHORIZED);
		for (const Variant &value : debug_files) {
			ERR_FAIL_COND_V(value.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
			const String path = value;
			ERR_FAIL_COND_V(!path.ends_with(".luau") || (!path.begins_with("res://tests/runtime/character/") && !path.begins_with("res://content/scripts/runtime/preview/")), ERR_UNAUTHORIZED);
			error = check_path(path);
			ERR_FAIL_COND_V(error != OK, error);
			const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path);
			ERR_FAIL_COND_V(bytes.is_empty() || bytes.size() > 256 * 1024 || bytes.find(0) >= 0, ERR_INVALID_DATA);
			closure.insert(path);
		}
	}
	ERR_FAIL_COND_V(closure.size() > 4096, ERR_PARAMETER_RANGE_ERROR);
	Vector<String> files;
	for (const String &path : closure) {
		files.push_back(path);
	}
	files.sort();
	String absolute = ProjectSettings::get_singleton()->globalize_path(output);
	error = DirAccess::make_dir_recursive_absolute(absolute.get_base_dir());
	ERR_FAIL_COND_V(error != OK, error);
	Ref<PCKPacker> packer;
	packer.instantiate();
	error = packer->pck_start(absolute + ".partial");
	ERR_FAIL_COND_V(error != OK, error);
	Dictionary hashes;
	for (const String &path : files) {
		error = packer->add_file(path, path);
		if (error != OK) {
			break;
		}
		hashes[path] = FileAccess::get_sha256(path);
	}
	if (error == OK) {
		error = packer->flush();
	}
	packer.unref(); // Close before the Windows rename.
	if (error != OK) {
		DirAccess::remove_absolute(absolute + ".partial");
		return error;
	}
	error = DirAccess::rename_absolute(absolute + ".partial", absolute);
	ERR_FAIL_COND_V(error != OK, error);
	r_result["output"] = output;
	r_result["files"] = hashes;
	r_result["sha256"] = FileAccess::get_sha256(output);
	r_result["signed"] = false;
	r_result["debug_only"] = !debug_files.is_empty();
	return OK;
}
} // namespace CookerFiles
/*>>----- VEYA_COOKER */
