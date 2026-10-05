/*<<----- VEYA_COOKER: immutable source payloads published by Luau pipelines. */
#pragma once
#include "asset_files.h"
#include "atomic_save.h"

namespace CookerSource {
// Bootstrap PNGs, native fonts and shader source are already runtime formats.
// Transport their original bytes; do not turn staging into another importer.
inline bool is_payload(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	return extension == "png" || extension == "otf" || extension == "ttf" || extension == "woff2" ||
			extension == "gdshader" || extension == "gdshaderinc" || extension == "uid";
}

inline Error stage(const Dictionary &p_job, Dictionary &r_result) {
	for (const Variant *key = p_job.next(); key; key = p_job.next(key)) {
		const String name = *key;
		ERR_FAIL_COND_V(name != "operation" && name != "source" && name != "output", ERR_INVALID_PARAMETER);
	}
	ERR_FAIL_COND_V(p_job.get("source", Variant()).get_type() != Variant::STRING ||
			p_job.get("output", Variant()).get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	const String source = p_job["source"], output = p_job["output"];
	Error error = CookerFiles::check_path(source);
	ERR_FAIL_COND_V(error != OK, error);
	error = CookerFiles::check_path(output, true);
	ERR_FAIL_COND_V(error != OK, error);
	const int marker = source.find("/source/");
	ERR_FAIL_COND_V(marker < 0, ERR_UNAUTHORIZED);
	const String package = source.left(marker);
	const Vector<String> parts = package.trim_prefix("res://").split("/");
	const bool shared = package == "res://content/shared";
	const bool world = parts.size() == 3 && parts[0] == "content" &&
			(parts[1] == "worlds" || parts[1] == "mod-worlds");
	ERR_FAIL_COND_V(!shared && !world, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!output.begins_with(package + "/generated/"), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!is_payload(source) || source.get_extension() != output.get_extension(), ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(CookerAtomicSave::occupied(output), ERR_ALREADY_EXISTS);
	Ref<FileAccess> input = FileAccess::open(source, FileAccess::READ, &error);
	ERR_FAIL_COND_V(input.is_null() || error != OK, ERR_FILE_CANT_READ);
	const uint64_t bytes = input->get_length();
	ERR_FAIL_COND_V(bytes == 0 || bytes > 64 * 1024 * 1024, ERR_PARAMETER_RANGE_ERROR);
	const PackedByteArray data = input->get_buffer(bytes);
	ERR_FAIL_COND_V(uint64_t(data.size()) != bytes, ERR_FILE_CANT_READ);
	error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));
	ERR_FAIL_COND_V(error != OK, error);
	const String temporary = output + ".partial";
	CookerAtomicSave::discard_stale(temporary);
	Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE, &error);
	ERR_FAIL_COND_V(file.is_null() || error != OK, ERR_FILE_CANT_WRITE);
	file->store_buffer(data);
	file->flush();
	error = file->get_error();
	file.unref();
	if (error == OK) error = CookerAtomicSave::publish(temporary, output);
	if (error != OK) CookerAtomicSave::discard_stale(temporary);
	ERR_FAIL_COND_V(error != OK, error);
	r_result["output"] = output;
	r_result["bytes"] = int64_t(bytes);
	r_result["sha256"] = FileAccess::get_sha256(output);
	return OK;
}
}
/*>>----- VEYA_COOKER */
