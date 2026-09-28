/*<<----- VEYA_COOKER: bounded shared humanoid composition, assembly and source coverage. */
#include "character_actions.h"
#include "asset_files.h"
#include "core/io/json.h"
#include "core/io/resource_saver.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/packed_scene.h"
#include <cmath>

namespace CookerCharacter {
namespace {
bool identifier(const String &p_id) {
	if (p_id.is_empty() || p_id.length() > 96) return false;
	for (int i = 0; i < p_id.length(); ++i) {
		const char32_t c = p_id[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
	}
	return true;
}
Error output_path(const String &p_path, const String &p_extension) {
	Error error = CookerFiles::check_path(p_path, true);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V(p_path.get_extension() != p_extension, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(FileAccess::exists(p_path), ERR_ALREADY_EXISTS);
	return DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(p_path.get_base_dir()));
}
bool bone_track(const Ref<Animation> &p_animation, int p_track) {
	const auto type = p_animation->track_get_type(p_track);
	return (type == Animation::TYPE_POSITION_3D || type == Animation::TYPE_ROTATION_3D || type == Animation::TYPE_SCALE_3D) &&
		String(p_animation->track_get_path(p_track)).begins_with("%GeneralSkeleton:");
}
bool same_motion(const Ref<Animation> &p_a, const Ref<Animation> &p_b, uint64_t &r_keys) {
	if (p_a->get_length() != p_b->get_length() || p_a->get_loop_mode() != p_b->get_loop_mode() ||
		p_a->get_step() != p_b->get_step() || p_a->get_track_count() != p_b->get_track_count()) return false;
	for (int track = 0; track < p_a->get_track_count(); ++track) {
		if (p_a->track_get_type(track) != p_b->track_get_type(track) || p_a->track_get_path(track) != p_b->track_get_path(track) ||
			p_a->track_is_enabled(track) != p_b->track_is_enabled(track) || p_a->track_is_imported(track) != p_b->track_is_imported(track) ||
			p_a->track_get_interpolation_type(track) != p_b->track_get_interpolation_type(track) ||
			p_a->track_get_interpolation_loop_wrap(track) != p_b->track_get_interpolation_loop_wrap(track) ||
			p_a->track_get_key_count(track) != p_b->track_get_key_count(track)) return false;
		for (int key = 0; key < p_a->track_get_key_count(track); ++key) {
			if (++r_keys > 4000000 || p_a->track_get_key_time(track, key) != p_b->track_get_key_time(track, key) ||
				p_a->track_get_key_transition(track, key) != p_b->track_get_key_transition(track, key) ||
				p_a->track_get_key_value(track, key) != p_b->track_get_key_value(track, key)) return false;
		}
	}
	return true;
}
Skeleton3D *skeleton_in(Node *p_node) {
	if (auto *skeleton = Object::cast_to<Skeleton3D>(p_node)) return skeleton;
	for (int i = 0; i < p_node->get_child_count(); ++i) if (auto *found = skeleton_in(p_node->get_child(i))) return found;
	return nullptr;
}
// RAII is important here: validation errors must not leave detached mesh trees alive.
struct Tree {
	Node *root = nullptr;
	~Tree() { if (root) memdelete(root); }
};
void remove_players(Node *p_node) {
	for (int i = p_node->get_child_count() - 1; i >= 0; --i) {
		Node *child = p_node->get_child(i);
		if (Object::cast_to<AnimationPlayer>(child)) { p_node->remove_child(child); memdelete(child); }
		else remove_players(child);
	}
}
Error load_library(const String &p_path, Ref<AnimationLibrary> &r_library) {
	Error error = CookerFiles::check_path(p_path);
	ERR_FAIL_COND_V(error != OK, error);
	r_library = ResourceLoader::load(p_path, "AnimationLibrary", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
	return r_library.is_valid() ? OK : (error == OK ? ERR_INVALID_DATA : error);
}
Error apply_rotation_offsets(const Array &p_offsets, const Ref<Animation> &p_animation) {
	ERR_FAIL_COND_V(p_offsets.size() > 16, ERR_PARAMETER_RANGE_ERROR);
	HashSet<String> bones;
	for (const Variant &value : p_offsets) {
		ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
		const Dictionary offset = value;
		const String bone = offset.get("bone", "");
		ERR_FAIL_COND_V(bone.is_empty() || bone.length() > 64 || bone.contains(":") || bone.contains("/") || bones.has(bone), ERR_INVALID_PARAMETER);
		bones.insert(bone);
		const int track = p_animation->find_track(NodePath("%GeneralSkeleton:" + bone), Animation::TYPE_ROTATION_3D);
		ERR_FAIL_COND_V(track < 0 || offset.get("keys", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_DATA);
		const Array keys = offset["keys"];
		ERR_FAIL_COND_V(keys.size() < 2 || keys.size() > 8, ERR_PARAMETER_RANGE_ERROR);
		Vector<double> phases;
		Vector<Quaternion> rotations;
		for (const Variant &key_value : keys) {
			ERR_FAIL_COND_V(key_value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
			const Dictionary key = key_value;
			const double phase = key.get("phase", -1.0);
			ERR_FAIL_COND_V(!std::isfinite(phase) || phase < 0 || phase > 1 || (!phases.is_empty() && phase <= phases[phases.size() - 1]), ERR_INVALID_PARAMETER);
			ERR_FAIL_COND_V(key.get("degrees", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
			const Array degrees = key["degrees"];
			ERR_FAIL_COND_V(degrees.size() != 3, ERR_INVALID_PARAMETER);
			Vector3 euler;
			for (int axis = 0; axis < 3; ++axis) {
				const double angle = degrees[axis];
				// A source facing opposite the controller may need a full authored
				// heading turn; limb and non-yaw corrections keep the tighter bound.
				const double limit = bone == "Hips" && axis == 1 ? 180 : 90;
				ERR_FAIL_COND_V(!std::isfinite(angle) || std::abs(angle) > limit, ERR_PARAMETER_RANGE_ERROR);
				euler[axis] = Math::deg_to_rad(angle);
			}
			phases.push_back(phase); rotations.push_back(Quaternion::from_euler(euler));
		}
		ERR_FAIL_COND_V(phases[0] != 0.0 || phases[phases.size() - 1] != 1.0, ERR_INVALID_PARAMETER);
		int interval = 0;
		for (int sample = 0; sample < p_animation->track_get_key_count(track); ++sample) {
			const double time = p_animation->track_get_key_time(track, sample);
			const double phase = time / p_animation->get_length();
			while (interval + 2 < phases.size() && phase > phases[interval + 1]) ++interval;
			const double weight = CLAMP((phase - phases[interval]) / (phases[interval + 1] - phases[interval]), 0.0, 1.0);
			const Quaternion correction = rotations[interval].slerp(rotations[interval + 1], weight);
			const Quaternion original = p_animation->track_get_key_value(track, sample);
			p_animation->track_set_key_value(track, sample, (original * correction).normalized());
		}
	}
	return OK;
}
Error apply_hips_position_offsets(const Array &p_offsets, const Ref<Animation> &p_animation) {
	if (p_offsets.is_empty()) return OK;
	ERR_FAIL_COND_V(p_offsets.size() < 2 || p_offsets.size() > 8, ERR_PARAMETER_RANGE_ERROR);
	const int track = p_animation->find_track(NodePath("%GeneralSkeleton:Hips"), Animation::TYPE_POSITION_3D);
	ERR_FAIL_COND_V(track < 0 || p_animation->track_get_key_count(track) == 0, ERR_INVALID_DATA);
	Vector<double> phases;
	Vector<Vector3> offsets;
	for (const Variant &value : p_offsets) {
		ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
		const Dictionary key = value;
		const double phase = key.get("phase", -1.0);
		const Variant position_value = key.get("meters", Variant());
		ERR_FAIL_COND_V(position_value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
		const Dictionary meters = position_value;
		const double x = meters.get("x", 0.0), y = meters.get("y", 0.0), z = meters.get("z", 0.0);
		ERR_FAIL_COND_V(!std::isfinite(phase) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
				|| phase < 0 || phase > 1 || std::abs(x) > 1.0 || std::abs(y) > 1.0 || std::abs(z) > 1.0
				|| (!phases.is_empty() && phase <= phases[phases.size() - 1]), ERR_INVALID_PARAMETER);
		phases.push_back(phase);
		offsets.push_back(Vector3(x, y, z));
	}
	ERR_FAIL_COND_V(phases[0] != 0.0 || phases[phases.size() - 1] != 1.0, ERR_INVALID_PARAMETER);
	int interval = 0;
	for (int sample = 0; sample < p_animation->track_get_key_count(track); ++sample) {
		const double phase = p_animation->track_get_key_time(track, sample) / p_animation->get_length();
		while (interval + 2 < phases.size() && phase > phases[interval + 1]) ++interval;
		const double weight = CLAMP((phase - phases[interval]) / (phases[interval + 1] - phases[interval]), 0.0, 1.0);
		Vector3 position = p_animation->track_get_key_value(track, sample);
		position += offsets[interval].lerp(offsets[interval + 1], weight);
		p_animation->track_set_key_value(track, sample, position);
	}
	return OK;
}
}

Error compose_animation(const Dictionary &p_job, Dictionary &r_result) {
	ERR_FAIL_COND_V(p_job.get("clips", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
	const Array clips = p_job["clips"];
	ERR_FAIL_COND_V(clips.is_empty() || clips.size() > 256, ERR_PARAMETER_RANGE_ERROR);
	const String output = p_job.get("output", "");
	Error error = output_path(output, "res");
	ERR_FAIL_COND_V(error != OK, error);
	Ref<AnimationLibrary> library;
	library.instantiate();
	Array provenance;
	const String base_path = p_job.get("base_library", "");
	if (!base_path.is_empty()) {
		Ref<AnimationLibrary> base;
		error = load_library(base_path, base);
		ERR_FAIL_COND_V(error != OK, error);
		LocalVector<StringName> names; base->get_animation_list(&names);
		ERR_FAIL_COND_V(names.size() > 256, ERR_PARAMETER_RANGE_ERROR);
		for (const StringName &name : names) {
			ERR_FAIL_COND_V(!identifier(name), ERR_INVALID_DATA);
			const Ref<Animation> animation = base->get_animation(name);
			for (int track = 0; track < animation->get_track_count(); ++track) ERR_FAIL_COND_V(!bone_track(animation, track), ERR_INVALID_DATA);
			library->add_animation(name, animation); // Immutable base clips are shared, not resampled.
		}
		// Array is reference-counted, so detach the catalog container before
		// appending/replacing entries in this new immutable library revision.
		provenance = Array(base->get_meta("character_catalog", Array())).duplicate();
		library->set_meta("base_library", base_path);
		library->set_meta("base_sha256", FileAccess::get_sha256(base_path));
	}
	uint64_t total_keys = 0;
	HashSet<String> requested_ids;
	Array replaced_ids;
	for (const Variant &value : clips) {
		ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
		const Dictionary clip = value;
		const String id = clip.get("id", "");
		ERR_FAIL_COND_V(!identifier(id) || requested_ids.has(id), ERR_INVALID_PARAMETER);
		requested_ids.insert(id);
		const Variant replace_value = clip.get("replace_existing", false);
		ERR_FAIL_COND_V(replace_value.get_type() != Variant::BOOL, ERR_INVALID_PARAMETER);
		const bool replacing = replace_value;
		// Replacing must be explicit and refer to an existing base semantic.
		// A typo must not silently become a new animation in an override job.
		ERR_FAIL_COND_V(replacing != library->has_animation(id), ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(!replacing && library->get_animation_list_size() >= 256, ERR_PARAMETER_RANGE_ERROR);
		ERR_FAIL_COND_V(clip.get("segments", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		const Array segments = clip["segments"];
		ERR_FAIL_COND_V(segments.is_empty() || segments.size() > 8, ERR_PARAMETER_RANGE_ERROR);
		const String hips_reference_id = clip.get("align_hips_to", "");
		Vector3 hips_reference;
		if (!hips_reference_id.is_empty()) {
			ERR_FAIL_COND_V(!identifier(hips_reference_id) || segments.size() != 1 || !library->has_animation(hips_reference_id), ERR_INVALID_PARAMETER);
			const Ref<Animation> reference = library->get_animation(hips_reference_id);
			const int track = reference->find_track(NodePath("%GeneralSkeleton:Hips"), Animation::TYPE_POSITION_3D);
			ERR_FAIL_COND_V(track < 0 || reference->track_get_key_count(track) == 0, ERR_INVALID_DATA);
			hips_reference = reference->position_track_interpolate(track, 0);
		}
		Ref<Animation> composed;
		composed.instantiate();
		double cursor = 0;
		Array sources;
		for (const Variant &segment_value : segments) {
			ERR_FAIL_COND_V(segment_value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
			const Dictionary segment = segment_value;
			const String source_path = segment.get("source", "");
			Ref<AnimationLibrary> source_library;
			error = load_library(source_path, source_library);
			ERR_FAIL_COND_V(error != OK, error);
			const String source_name = segment.get("animation", "mixamo_com");
			ERR_FAIL_COND_V(!source_library->has_animation(source_name), ERR_INVALID_DATA);
			const Ref<Animation> source = source_library->get_animation(source_name);
			const double begin = segment.get("begin", 0.0), end = segment.get("end", 1.0), speed = segment.get("speed", 1.0);
			const bool reverse = segment.get("reverse", false), flatten_vertical = clip.get("flatten_vertical", false);
			ERR_FAIL_COND_V(!std::isfinite(begin) || !std::isfinite(end) || !std::isfinite(speed) || begin < 0 || end > 1 || begin >= end || speed < 0.1 || speed > 4, ERR_PARAMETER_RANGE_ERROR);
			const double start = begin * source->get_length(), finish = end * source->get_length();
			const double duration = (finish - start) / speed;
			const double blend = segment.get("blend", 0.08);
			ERR_FAIL_COND_V(!std::isfinite(blend) || blend < 0 || blend > 0.25, ERR_PARAMETER_RANGE_ERROR);
			ERR_FAIL_COND_V(duration < 0.01 || duration > 60 || cursor + duration > 120, ERR_PARAMETER_RANGE_ERROR);
			int rotations = 0;
			for (int source_track = 0; source_track < source->get_track_count(); ++source_track) {
				// Method, audio, property and non-skeleton tracks are never executable content.
				ERR_FAIL_COND_V(!bone_track(source, source_track), ERR_INVALID_DATA);
				const auto type = source->track_get_type(source_track);
				const NodePath path = source->track_get_path(source_track);
				int target_track = composed->find_track(path, type);
				if (target_track < 0) { target_track = composed->add_track(type); composed->track_set_path(target_track, path); }
				if (type == Animation::TYPE_ROTATION_3D) ++rotations;
				const int samples = int(std::ceil(duration * 60));
				total_keys += samples + 1;
				ERR_FAIL_COND_V(total_keys > 4000000, ERR_OUT_OF_MEMORY);
				const bool hips = String(path) == "%GeneralSkeleton:Hips";
				Vector3 origin;
				if (type == Animation::TYPE_POSITION_3D) origin = source->position_track_interpolate(source_track, 0);
				const int previous_keys = composed->track_get_key_count(target_track);
				const Variant previous = previous_keys ? composed->track_get_key_value(target_track, previous_keys - 1) : Variant();
				for (int sample = 0; sample <= samples; ++sample) {
					const double phase = double(sample) / samples;
					const double time = reverse ? finish - phase * (finish - start) : start + phase * (finish - start);
					const double at = cursor + phase * duration;
					const double blend_weight = cursor > 0 && previous_keys && blend > 0 ? MIN(1.0, phase * duration / blend) : 1.0;
					if (type == Animation::TYPE_POSITION_3D) {
						Vector3 position = source->position_track_interpolate(source_track, time);
						if (hips) {
							position.x = origin.x; position.z = origin.z;
							if (flatten_vertical) position.y = origin.y;
							if (!hips_reference_id.is_empty()) position += hips_reference - origin;
						}
						if (blend_weight < 1) position = Vector3(previous).lerp(position, blend_weight);
						composed->position_track_insert_key(target_track, at, position);
					} else if (type == Animation::TYPE_ROTATION_3D) {
						Quaternion rotation = source->rotation_track_interpolate(source_track, time);
						if (blend_weight < 1) rotation = Quaternion(previous).slerp(rotation, blend_weight);
						composed->rotation_track_insert_key(target_track, at, rotation);
					} else {
						Vector3 scale = source->scale_track_interpolate(source_track, time);
						if (blend_weight < 1) scale = Vector3(previous).lerp(scale, blend_weight);
						composed->scale_track_insert_key(target_track, at, scale);
					}
				}
			}
			ERR_FAIL_COND_V(rotations < 10, ERR_INVALID_DATA);
			cursor += duration;
			Dictionary evidence = segment.duplicate(true);
			evidence["sha256"] = FileAccess::get_sha256(source_path);
			sources.push_back(evidence);
		}
		composed->set_length(cursor);
		const Array offsets = clip.get("rotation_offsets", Array());
		error = apply_rotation_offsets(offsets, composed);
		ERR_FAIL_COND_V(error != OK, error);
		const Array position_offsets = clip.get("hips_position_offsets", Array());
		error = apply_hips_position_offsets(position_offsets, composed);
		ERR_FAIL_COND_V(error != OK, error);
		composed->set_loop_mode(bool(clip.get("loop", false)) ? Animation::LOOP_LINEAR : Animation::LOOP_NONE);
		composed->set_meta("character_semantic", id);
		composed->set_meta("source_segments", sources);
		composed->set_meta("rotation_offsets", offsets);
		composed->set_meta("hips_position_offsets", position_offsets);
		if (!hips_reference_id.is_empty()) composed->set_meta("align_hips_to", hips_reference_id);
		composed->set_meta("visual_status", clip.get("visual_status", "unverified"));
		if (replacing) library->remove_animation(id);
		error = library->add_animation(id, composed);
		ERR_FAIL_COND_V(error != OK, error);
		Dictionary record;
		record["id"] = id; record["seconds"] = cursor; record["sources"] = sources;
		record["rotation_offsets"] = offsets;
		record["hips_position_offsets"] = position_offsets;
		if (!hips_reference_id.is_empty()) record["align_hips_to"] = hips_reference_id;
		record["visual_status"] = clip.get("visual_status", "unverified");
		if (replacing) {
			bool updated = false;
			for (int index = 0; index < provenance.size(); ++index) {
				const Dictionary previous = provenance[index];
				if (String(previous.get("id", "")) == id) { provenance[index] = record; updated = true; break; }
			}
			if (!updated) provenance.push_back(record);
			replaced_ids.push_back(id);
		} else provenance.push_back(record);
	}
	library->set_meta("character_catalog_revision", p_job.get("revision", 1));
	library->set_meta("character_catalog", provenance);
	error = ResourceSaver::save(library, output, ResourceSaver::FLAG_COMPRESS);
	ERR_FAIL_COND_V(error != OK, error);
	LocalVector<StringName> composed_names; library->get_animation_list(&composed_names);
	r_result["output"] = output; r_result["clips"] = composed_names.size(); r_result["keys"] = total_keys;
	r_result["replaced_ids"] = replaced_ids;
	return OK;
}

Error assemble(const Dictionary &p_job, Dictionary &r_result) {
	const String model_path = p_job.get("model", ""), library_path = p_job.get("library", ""), output = p_job.get("output", "");
	Error error = CookerFiles::check_path(model_path);
	ERR_FAIL_COND_V(error != OK, error);
	error = output_path(output, "scn");
	ERR_FAIL_COND_V(error != OK, error);
	Ref<PackedScene> model = ResourceLoader::load(model_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
	ERR_FAIL_COND_V(model.is_null(), ERR_INVALID_DATA);
	Ref<AnimationLibrary> library;
	error = load_library(library_path, library);
	ERR_FAIL_COND_V(error != OK, error);
	Tree tree{ model->instantiate() };
	ERR_FAIL_NULL_V(tree.root, ERR_CANT_CREATE);
	Skeleton3D *skeleton = skeleton_in(tree.root);
	ERR_FAIL_NULL_V(skeleton, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(skeleton->find_bone("Hips") < 0 || skeleton->find_bone("LeftHand") < 0 || skeleton->find_bone("RightFoot") < 0, ERR_INVALID_DATA);
	remove_players(tree.root);
	auto *player = memnew(AnimationPlayer);
	player->set_name("CharacterAnimationPlayer"); tree.root->add_child(player); player->set_owner(tree.root);
	player->set_root_node(NodePath(".."));
	error = player->add_animation_library("character", library);
	ERR_FAIL_COND_V(error != OK, error);
	const Array sockets = p_job.get("sockets", Array());
	ERR_FAIL_COND_V(sockets.size() > 16, ERR_PARAMETER_RANGE_ERROR);
	HashSet<String> names;
	for (const Variant &value : sockets) {
		ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
		const Dictionary socket = value;
		const String id = socket.get("id", ""), bone = socket.get("bone", "");
		ERR_FAIL_COND_V(!identifier(id) || names.has(id) || skeleton->find_bone(bone) < 0, ERR_INVALID_DATA);
		names.insert(id);
		auto *attachment = memnew(BoneAttachment3D);
		attachment->set_name(id); skeleton->add_child(attachment); attachment->set_owner(tree.root);
		attachment->set_bone_name(bone);
	}
	tree.root->set_meta("character_rig_profile", p_job.get("rig_profile", Dictionary()));
	tree.root->set_meta("character_library", library_path);
	Ref<PackedScene> assembled; assembled.instantiate();
	error = assembled->pack(tree.root);
	ERR_FAIL_COND_V(error != OK, error);
	error = ResourceSaver::save(assembled, output, ResourceSaver::FLAG_COMPRESS);
	ERR_FAIL_COND_V(error != OK, error);
	r_result["output"] = output; r_result["sockets"] = sockets.size(); r_result["bone_count"] = skeleton->get_bone_count();
	return OK;
}

Error validate(const Dictionary &p_job, Dictionary &r_result) {
	const String library_path = p_job.get("library", ""), output = p_job.get("output", "");
	Ref<AnimationLibrary> library;
	Error error = load_library(library_path, library);
	ERR_FAIL_COND_V(error != OK, error);
	error = output_path(output, "json");
	ERR_FAIL_COND_V(error != OK, error);
	Array required = Array(p_job.get("required", Array())).duplicate();
	if (bool(p_job.get("include_library", false))) {
		LocalVector<StringName> names; library->get_animation_list(&names);
		for (const StringName &name : names) if (!required.has(String(name))) required.push_back(String(name));
	}
	ERR_FAIL_COND_V(required.is_empty() || required.size() > 256, ERR_PARAMETER_RANGE_ERROR);
	Array missing, unverified, rows;
	HashSet<String> seen;
	for (const Variant &value : required) {
		ERR_FAIL_COND_V(value.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		const String id = value;
		ERR_FAIL_COND_V(!identifier(id) || seen.has(id), ERR_INVALID_PARAMETER);
		seen.insert(id);
		Dictionary row; row["id"] = id;
		if (!library->has_animation(id)) { missing.push_back(id); row["status"] = "missing_source"; rows.push_back(row); continue; }
		const Ref<Animation> animation = library->get_animation(id);
		ERR_FAIL_COND_V(animation->get_length() <= 0 || animation->get_track_count() < 10, ERR_INVALID_DATA);
		for (int i = 0; i < animation->get_track_count(); ++i) ERR_FAIL_COND_V(!bone_track(animation, i), ERR_INVALID_DATA);
		row["status"] = animation->get_meta("visual_status", "unverified");
		row["seconds"] = animation->get_length(); row["loop"] = animation->get_loop_mode() != Animation::LOOP_NONE;
		row["sources"] = animation->get_meta("source_segments", Array());
		row["rotation_offsets"] = animation->get_meta("rotation_offsets", Array());
		row["hips_position_offsets"] = animation->get_meta("hips_position_offsets", Array());
		if (String(row["status"]) != "verified") unverified.push_back(id);
		rows.push_back(row);
	}
	Dictionary report;
	report["schema_version"] = 1; report["library"] = library_path; report["sha256"] = FileAccess::get_sha256(library_path);
	report["required"] = required.size(); report["missing"] = missing; report["unverified"] = unverified; report["entries"] = rows;
	report["source_complete"] = missing.is_empty(); report["visual_complete"] = missing.is_empty() && unverified.is_empty();
	const String compare_path = p_job.get("compare_base", "");
	if (!compare_path.is_empty()) {
		Ref<AnimationLibrary> base;
		error = load_library(compare_path, base);
		ERR_FAIL_COND_V(error != OK, error);
		ERR_FAIL_COND_V(p_job.get("expected_changed", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		const Array expected = p_job["expected_changed"];
		ERR_FAIL_COND_V(expected.size() > 256, ERR_PARAMETER_RANGE_ERROR);
		HashSet<String> expected_ids;
		for (const Variant &value : expected) {
			ERR_FAIL_COND_V(value.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
			const String id = value;
			ERR_FAIL_COND_V(!identifier(id) || expected_ids.has(id) || !base->has_animation(id), ERR_INVALID_PARAMETER);
			expected_ids.insert(id);
		}
		LocalVector<StringName> names; base->get_animation_list(&names);
		ERR_FAIL_COND_V(names.size() > 256 || library->get_animation_list_size() != int(names.size()), ERR_INVALID_DATA);
		Array changed, retained;
		uint64_t compared_keys = 0;
		for (const StringName &name : names) {
			ERR_FAIL_COND_V(!library->has_animation(name), ERR_INVALID_DATA);
			const bool same = same_motion(base->get_animation(name), library->get_animation(name), compared_keys);
			ERR_FAIL_COND_V(compared_keys > 4000000, ERR_OUT_OF_MEMORY);
			ERR_FAIL_COND_V_MSG(same == expected_ids.has(name), ERR_INVALID_DATA, "Animation replacement comparison differs: " + String(name));
			if (same) retained.push_back(String(name)); else changed.push_back(String(name));
		}
		report["compared_base"] = compare_path;
		report["base_sha256"] = FileAccess::get_sha256(compare_path);
		report["changed_ids"] = changed; report["retained_ids"] = retained;
		report["compared_keys"] = compared_keys;
	}
	Ref<FileAccess> file = FileAccess::open(output, FileAccess::WRITE, &error);
	ERR_FAIL_COND_V(file.is_null(), error);
	file->store_string(JSON::stringify(report, "\t", true));
	r_result = report; r_result["output"] = output;
	// A report can be deliberately produced for an incomplete catalog; release jobs opt into strict gating.
	return bool(p_job.get("require_complete", false)) && !missing.is_empty() ? ERR_DOES_NOT_EXIST : OK;
}
}
/*>>----- VEYA_COOKER */
