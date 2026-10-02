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
#include <algorithm>

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
NodePath mirrored_bone_path(const NodePath &p_path) {
	const String path = String(p_path);
	const String prefix = "%GeneralSkeleton:";
	const String bone = path.trim_prefix(prefix);
	if (bone.begins_with("Left")) return NodePath(prefix + "Right" + bone.substr(4));
	if (bone.begins_with("Right")) return NodePath(prefix + "Left" + bone.substr(5));
	return p_path;
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
// Offline FK sampling of the actual target body. The authored Luau graph
// selects clips/thresholds; the native cook stores bounded contact curves and
// the measured stance cadence alongside immutable bone tracks.
Error calibrate_gait(const Dictionary &p_config, const Ref<Animation> &p_animation) {
	const String model_path = p_config.get("model", "");
	ERR_FAIL_COND_V(CookerFiles::check_path(model_path) != OK, ERR_INVALID_PARAMETER);
	const Ref<PackedScene> model = ResourceLoader::load(model_path, "PackedScene");
	ERR_FAIL_COND_V(model.is_null(), ERR_INVALID_DATA);
	Tree tree; tree.root = model->instantiate();
	Skeleton3D *skeleton = tree.root ? skeleton_in(tree.root) : nullptr;
	ERR_FAIL_COND_V(!skeleton || skeleton->get_bone_count() > 128, ERR_INVALID_DATA);
	const Array direction_value = p_config.get("direction", Array());
	ERR_FAIL_COND_V(direction_value.size() != 3, ERR_INVALID_PARAMETER);
	Vector3 direction(double(direction_value[0]), 0, double(direction_value[2]));
	ERR_FAIL_COND_V(!direction.is_finite() || direction.length() < 0.9, ERR_INVALID_PARAMETER);
	direction.normalize();
	const double planted_height = p_config.get("planted_height", 0.025);
	const double release_height = p_config.get("release_height", 0.08);
	const double nominal_speed = p_config.get("nominal_speed", 1.5);
	ERR_FAIL_COND_V(!std::isfinite(planted_height) || !std::isfinite(release_height) || planted_height < 0 || release_height <= planted_height || release_height > .3 || !std::isfinite(nominal_speed) || nominal_speed <= 0 || nominal_speed > 10, ERR_INVALID_PARAMETER);
	const int count = skeleton->get_bone_count();
	const int feet[2] = { skeleton->find_bone("LeftFoot"), skeleton->find_bone("RightFoot") };
	ERR_FAIL_COND_V(feet[0] < 0 || feet[1] < 0 || p_animation->get_loop_mode() == Animation::LOOP_NONE, ERR_INVALID_DATA);
	const int samples = int(std::ceil(p_animation->get_length() * 60));
	ERR_FAIL_COND_V(samples < 8 || samples > 1800, ERR_PARAMETER_RANGE_ERROR);
	Vector<int> positions, rotations, parents; positions.resize(count); rotations.resize(count); parents.resize(count);
	Vector<Transform3D> globals; globals.resize(count);
	Vector<Vector3> points[2]; points[0].resize(samples + 1); points[1].resize(samples + 1);
	for (int bone = 0; bone < count; ++bone) {
		const NodePath path("%GeneralSkeleton:" + String(skeleton->get_bone_name(bone)));
		positions.write[bone] = p_animation->find_track(path, Animation::TYPE_POSITION_3D);
		rotations.write[bone] = p_animation->find_track(path, Animation::TYPE_ROTATION_3D);
		parents.write[bone] = skeleton->get_bone_parent(bone);
		ERR_FAIL_COND_V(parents[bone] >= bone, ERR_INVALID_DATA);
	}
	double minimum[2] = { 1e9, 1e9 }, maximum[2] = { -1e9, -1e9 };
	for (int sample = 0; sample <= samples; ++sample) {
		const double time = sample * p_animation->get_length() / samples;
		for (int bone = 0; bone < count; ++bone) {
			const Transform3D rest = skeleton->get_bone_rest(bone);
			const Vector3 position = positions[bone] >= 0 ? p_animation->position_track_interpolate(positions[bone], time) : rest.origin;
			const Quaternion rotation = rotations[bone] >= 0 ? p_animation->rotation_track_interpolate(rotations[bone], time) : rest.basis.get_rotation_quaternion();
			const Transform3D local(Basis(rotation).scaled(rest.basis.get_scale()), position);
			globals.write[bone] = parents[bone] >= 0 ? globals[parents[bone]] * local : local;
		}
		for (int side = 0; side < 2; ++side) {
			const Vector3 point = globals[feet[side]].origin;
			ERR_FAIL_COND_V(!point.is_finite(), ERR_INVALID_DATA);
			points[side].write[sample] = point;
			minimum[side] = MIN(minimum[side], point.y); maximum[side] = MAX(maximum[side], point.y);
		}
	}
	Vector<double> velocities;
	PackedFloat32Array weights[2];
	int origin = 0;
	for (int sample = 0; sample < samples; ++sample) {
		if (points[0][sample].dot(direction) > points[0][origin].dot(direction)) origin = sample;
		for (int side = 0; side < 2; ++side) {
			const double lift = points[side][sample].y - minimum[side];
			const double backward = -(points[side][sample + 1] - points[side][sample]).dot(direction) * samples / p_animation->get_length();
			const double release = CLAMP((lift - planted_height) / (release_height - planted_height), 0.0, 1.0);
			// Contact requires the foot to travel backwards relative to the body.
			// This releases the low swing foot before its forward sweep/landing.
			const double contact = backward > .05 ? 1 - release * release * (3 - 2 * release) : 0;
			weights[side].push_back(contact);
			if (contact > .9 && backward > .1) velocities.push_back(backward);
		}
	}
	ERR_FAIL_COND_V_MSG(velocities.size() < 4, ERR_INVALID_DATA, "Gait has insufficient stance samples; check direction: " + model_path);
	std::sort(velocities.ptrw(), velocities.ptrw() + velocities.size());
	const double reference_speed = velocities[velocities.size() / 2];
	ERR_FAIL_COND_V(reference_speed < .1 || reference_speed > 10, ERR_INVALID_DATA);
	Dictionary gait;
	gait["schema_version"] = 1; gait["nominal_speed"] = nominal_speed;
	gait["reference_speed"] = reference_speed; gait["phase_origin"] = double(origin) / samples;
	gait["left_contact"] = weights[0]; gait["right_contact"] = weights[1];
	gait["left_min_height"] = minimum[0]; gait["right_min_height"] = minimum[1];
	gait["left_lift"] = maximum[0] - minimum[0]; gait["right_lift"] = maximum[1] - minimum[1];
	gait["stance_samples"] = velocities.size(); gait["model"] = model_path;
	p_animation->set_meta("locomotion_gait", gait);
	return OK;
}
Error mirror_bone_rotations(const Array &p_bones, const Ref<Animation> &p_animation) {
	ERR_FAIL_COND_V(p_bones.size() > 16, ERR_PARAMETER_RANGE_ERROR);
	HashSet<String> names;
	for (const Variant &value : p_bones) {
		ERR_FAIL_COND_V(value.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		const String bone = value;
		ERR_FAIL_COND_V(bone.length() > 64 || bone.contains(":") || bone.contains("/") || names.has(bone) ||
				(!bone.begins_with("Left") && !bone.begins_with("Right")), ERR_INVALID_PARAMETER);
		names.insert(bone);
		const NodePath path("%GeneralSkeleton:" + bone), target = mirrored_bone_path(path);
		const int source_track = p_animation->find_track(path, Animation::TYPE_ROTATION_3D);
		ERR_FAIL_COND_V(source_track < 0 || p_animation->find_track(target, Animation::TYPE_ROTATION_3D) < 0 ||
				p_animation->track_get_key_count(source_track) == 0, ERR_INVALID_DATA);
	}
	if (p_bones.is_empty()) return OK;
	// Read every source from one snapshot, so mirrored swaps never depend on
	// declaration order. Rest translations and all other bone tracks stay intact.
	const Ref<Animation> source = p_animation->duplicate(true);
	for (const Variant &value : p_bones) {
		const NodePath path("%GeneralSkeleton:" + String(value)), target = mirrored_bone_path(path);
		const int source_track = source->find_track(path, Animation::TYPE_ROTATION_3D);
		p_animation->remove_track(p_animation->find_track(target, Animation::TYPE_ROTATION_3D));
		source->copy_track(source_track, p_animation);
		const int track = p_animation->get_track_count() - 1;
		p_animation->track_set_path(track, target);
		for (int key = 0; key < p_animation->track_get_key_count(track); ++key) {
			const Quaternion rotation = p_animation->track_get_key_value(track, key);
			p_animation->track_set_key_value(track, key, Quaternion(rotation.x, -rotation.y, -rotation.z, rotation.w));
		}
	}
	return OK;
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
		const String space = offset.get("space", "local");
		ERR_FAIL_COND_V(space != "local" && space != "parent", ERR_INVALID_PARAMETER);
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
			// A prone Hips-local Y axis is horizontal. Parent-space correction
			// changes heading about the skeleton parent's Y without rolling the body.
			p_animation->track_set_key_value(track, sample,
				(space == "parent" ? correction * original : original * correction).normalized());
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

Error prepare_compose_job(Dictionary &r_job) {
	if (!r_job.has("source_remap")) return OK;
	ERR_FAIL_COND_V(r_job.has("clips") || r_job.get("source_remap", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(r_job.get("preserve_base_hips", Variant()).get_type() != Variant::BOOL || !bool(r_job["preserve_base_hips"]), ERR_INVALID_PARAMETER);
	const Array mappings = r_job["source_remap"];
	ERR_FAIL_COND_V(mappings.is_empty() || mappings.size() > 8, ERR_PARAMETER_RANGE_ERROR);
	Vector<String> from, to;
	for (const Variant &value : mappings) {
		ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
		const Dictionary mapping = value;
		ERR_FAIL_COND_V(mapping.get("from", Variant()).get_type() != Variant::STRING || mapping.get("to", Variant()).get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		const String old_root = mapping["from"], new_root = mapping["to"];
		ERR_FAIL_COND_V(!old_root.ends_with("/") || !new_root.ends_with("/") || old_root == new_root, ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(CookerFiles::check_path(old_root.trim_suffix("/"), false, false, false) != OK ||
				CookerFiles::check_path(new_root.trim_suffix("/"), false, false, false) != OK, ERR_INVALID_PARAMETER);
		for (const String &previous : from) ERR_FAIL_COND_V(old_root.begins_with(previous) || previous.begins_with(old_root), ERR_INVALID_PARAMETER);
		from.push_back(old_root); to.push_back(new_root);
	}
	Ref<AnimationLibrary> base;
	Error error = load_library(r_job.get("base_library", ""), base);
	ERR_FAIL_COND_V(error != OK, error);
	LocalVector<StringName> names; base->get_animation_list(&names);
	ERR_FAIL_COND_V(names.size() > 256 || int(names.size()) != int(r_job.get("expected_clips", 0)), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(r_job.get("retained", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
	const Array retained = r_job["retained"];
	HashSet<String> keep;
	for (const Variant &value : retained) {
		ERR_FAIL_COND_V(value.get_type() != Variant::STRING || !identifier(value) || keep.has(value) || !base->has_animation(value), ERR_INVALID_PARAMETER);
		keep.insert(value);
	}
	Array clips;
	for (const StringName &name : names) {
		const Ref<Animation> original = base->get_animation(name);
		ERR_FAIL_COND_V(original->get_meta("source_segments", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_DATA);
		const Array sources = original->get_meta("source_segments");
		ERR_FAIL_COND_V(sources.is_empty() || sources.size() > 8, ERR_INVALID_DATA);
		Array segments;
		int mapped = 0;
		for (const Variant &source_value : sources) {
			ERR_FAIL_COND_V(source_value.get_type() != Variant::DICTIONARY, ERR_INVALID_DATA);
			Dictionary segment = Dictionary(source_value).duplicate(true);
			const String old_path = segment.get("source", "");
			for (int index = 0; index < from.size(); ++index) {
				if (!old_path.begins_with(from[index])) continue;
				const String new_path = to[index] + old_path.substr(from[index].length());
				Ref<AnimationLibrary> old_source, new_source;
				error = load_library(old_path, old_source); ERR_FAIL_COND_V(error != OK, error);
				error = load_library(new_path, new_source); ERR_FAIL_COND_V(error != OK, error);
				const String animation = segment.get("animation", "mixamo_com");
				ERR_FAIL_COND_V(!old_source->has_animation(animation) || !new_source->has_animation(animation), ERR_INVALID_DATA);
				const double old_length = old_source->get_animation(animation)->get_length();
				const double new_length = new_source->get_animation(animation)->get_length();
				ERR_FAIL_COND_V(old_length <= 0 || new_length <= 0, ERR_INVALID_DATA);
				// Preserve the compiled segment duration if FBX sampling endpoints differ.
				segment["speed"] = double(segment.get("speed", 1.0)) * new_length / old_length;
				segment["source"] = new_path;
				segment.erase("sha256");
				++mapped;
				break;
			}
			segments.push_back(segment);
		}
		ERR_FAIL_COND_V_MSG((mapped == 0) != keep.has(name) || (mapped != 0 && mapped != sources.size()), ERR_INVALID_DATA,
				"Source substitution must cover every segment, or explicitly retain the semantic: " + String(name));
		if (mapped == 0) continue;
		Dictionary clip;
		clip["id"] = String(name); clip["replace_existing"] = true;
		clip["segments"] = segments; clip["reference_segments"] = sources;
		clip["loop"] = original->get_loop_mode() != Animation::LOOP_NONE;
		clip["mirror_x"] = original->get_meta("mirror_x", false);
		clip["mirror_bones"] = original->get_meta("mirror_bones", Array());
		clip["rotation_offsets"] = original->get_meta("rotation_offsets", Array());
		clip["hips_position_offsets"] = original->get_meta("hips_position_offsets", Array());
		if (original->has_meta("align_hips_to")) clip["align_hips_to"] = original->get_meta("align_hips_to");
		if (original->has_meta("align_hips_axes")) clip["align_hips_axes"] = original->get_meta("align_hips_axes");
		clip["visual_status"] = "body_baked_requires_runtime_review";
		clips.push_back(clip);
	}
	ERR_FAIL_COND_V(clips.is_empty() || clips.size() != int(r_job.get("expected_replaced", 0)), ERR_INVALID_DATA);
	r_job["clips"] = clips;
	r_job.erase("source_remap"); // Expanded paths and old provenance are now cache dependencies.
	return OK;
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
		const Variant preserve_value = p_job.get("preserve_base_hips", false);
		ERR_FAIL_COND_V(preserve_value.get_type() != Variant::BOOL || (bool(preserve_value) && !replacing), ERR_INVALID_PARAMETER);
		const Ref<Animation> original = replacing ? library->get_animation(id) : Ref<Animation>();
		// Replacing must be explicit and refer to an existing base semantic.
		// A typo must not silently become a new animation in an override job.
		ERR_FAIL_COND_V(replacing != library->has_animation(id), ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(!replacing && library->get_animation_list_size() >= 256, ERR_PARAMETER_RANGE_ERROR);
		ERR_FAIL_COND_V(clip.get("segments", Variant()).get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		const Array segments = clip["segments"];
		ERR_FAIL_COND_V(segments.is_empty() || segments.size() > 8, ERR_PARAMETER_RANGE_ERROR);
		const Variant mirror_value = clip.get("mirror_x", false);
		ERR_FAIL_COND_V(mirror_value.get_type() != Variant::BOOL, ERR_INVALID_PARAMETER);
		const bool mirror_x = mirror_value;
		const String hips_reference_id = clip.get("align_hips_to", "");
		const Variant hips_axes_value = clip.get("align_hips_axes", "xyz");
		ERR_FAIL_COND_V(hips_axes_value.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		const String hips_axes = hips_axes_value;
		ERR_FAIL_COND_V((hips_axes != "xyz" && hips_axes != "xz") ||
				(clip.has("align_hips_axes") && hips_reference_id.is_empty()), ERR_INVALID_PARAMETER);
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
				const NodePath path = mirror_x ? mirrored_bone_path(source->track_get_path(source_track)) : source->track_get_path(source_track);
				int target_track = composed->find_track(path, type);
				if (target_track < 0) { target_track = composed->add_track(type); composed->track_set_path(target_track, path); }
				if (type == Animation::TYPE_ROTATION_3D) ++rotations;
				const int samples = int(std::ceil(duration * 60));
				total_keys += samples + 1;
				ERR_FAIL_COND_V(total_keys > 4000000, ERR_OUT_OF_MEMORY);
				const bool hips = String(path) == "%GeneralSkeleton:Hips";
				Vector3 origin;
				if (type == Animation::TYPE_POSITION_3D) {
					origin = source->position_track_interpolate(source_track, 0);
					if (mirror_x) origin.x = -origin.x;
				}
				const int previous_keys = composed->track_get_key_count(target_track);
				const Variant previous = previous_keys ? composed->track_get_key_value(target_track, previous_keys - 1) : Variant();
				for (int sample = 0; sample <= samples; ++sample) {
					const double phase = double(sample) / samples;
					const double time = reverse ? finish - phase * (finish - start) : start + phase * (finish - start);
					const double at = cursor + phase * duration;
					const double blend_weight = cursor > 0 && previous_keys && blend > 0 ? MIN(1.0, phase * duration / blend) : 1.0;
					if (type == Animation::TYPE_POSITION_3D) {
						Vector3 position = source->position_track_interpolate(source_track, time);
						if (mirror_x) position.x = -position.x;
						if (hips) {
							position.x = origin.x; position.z = origin.z;
							if (flatten_vertical) position.y = origin.y;
							if (!hips_reference_id.is_empty()) {
								Vector3 offset = hips_reference - origin;
								if (hips_axes == "xz") offset.y = 0;
								position += offset;
							}
						}
						if (blend_weight < 1) position = Vector3(previous).lerp(position, blend_weight);
						composed->position_track_insert_key(target_track, at, position);
					} else if (type == Animation::TYPE_ROTATION_3D) {
						Quaternion rotation = source->rotation_track_interpolate(source_track, time);
						if (mirror_x) rotation = Quaternion(rotation.x, -rotation.y, -rotation.z, rotation.w);
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
		const Variant mirror_bones_value = clip.get("mirror_bones", Array());
		ERR_FAIL_COND_V(mirror_bones_value.get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		const Array mirror_bones = mirror_bones_value;
		error = mirror_bone_rotations(mirror_bones, composed);
		ERR_FAIL_COND_V(error != OK, error);
		const Array offsets = clip.get("rotation_offsets", Array());
		error = apply_rotation_offsets(offsets, composed);
		ERR_FAIL_COND_V(error != OK, error);
		const Array position_offsets = clip.get("hips_position_offsets", Array());
		if (!bool(preserve_value)) {
			error = apply_hips_position_offsets(position_offsets, composed);
			ERR_FAIL_COND_V(error != OK, error);
		}
		if (bool(preserve_value)) {
			ERR_FAIL_COND_V_MSG(Math::abs(cursor - original->get_length()) > 0.00001, ERR_INVALID_DATA, "Substitution changed playback duration: " + id);
			const int old_hips = original->find_track(NodePath("%GeneralSkeleton:Hips"), Animation::TYPE_POSITION_3D);
			ERR_FAIL_COND_V(old_hips < 0, ERR_INVALID_DATA);
			const int new_hips = composed->find_track(NodePath("%GeneralSkeleton:Hips"), Animation::TYPE_POSITION_3D);
			if (new_hips >= 0) composed->remove_track(new_hips);
			original->copy_track(old_hips, composed);
			composed->set_length(original->get_length());
			composed->set_step(original->get_step());
			composed->set_meta("preserved_hips_from", base_path);
			composed->set_meta("reference_segments", clip.get("reference_segments", Array()));
		}
		composed->set_loop_mode(bool(clip.get("loop", false)) ? Animation::LOOP_LINEAR : Animation::LOOP_NONE);
		if (clip.has("gait")) {
			ERR_FAIL_COND_V(clip["gait"].get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
			error = calibrate_gait(clip["gait"], composed);
			ERR_FAIL_COND_V(error != OK, error);
		}
		composed->set_meta("character_semantic", id);
		composed->set_meta("source_segments", sources);
		composed->set_meta("mirror_x", mirror_x);
		composed->set_meta("mirror_bones", mirror_bones);
		composed->set_meta("rotation_offsets", offsets);
		composed->set_meta("hips_position_offsets", position_offsets);
		if (!hips_reference_id.is_empty()) composed->set_meta("align_hips_to", hips_reference_id);
		if (clip.has("align_hips_axes")) composed->set_meta("align_hips_axes", hips_axes);
		composed->set_meta("visual_status", clip.get("visual_status", "unverified"));
		if (replacing) library->remove_animation(id);
		error = library->add_animation(id, composed);
		ERR_FAIL_COND_V(error != OK, error);
		Dictionary record;
		record["id"] = id; record["seconds"] = cursor; record["sources"] = sources;
		record["mirror_x"] = mirror_x;
		record["mirror_bones"] = mirror_bones;
		record["rotation_offsets"] = offsets;
		record["hips_position_offsets"] = position_offsets;
		if (!hips_reference_id.is_empty()) record["align_hips_to"] = hips_reference_id;
		if (clip.has("align_hips_axes")) record["align_hips_axes"] = hips_axes;
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
	if (bool(p_job.get("preserve_base_hips", false))) library->set_meta("source_replaced_ids", replaced_ids);
	error = ResourceSaver::save(library, output, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
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
	error = ResourceSaver::save(assembled, output, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
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
		row["mirror_x"] = animation->get_meta("mirror_x", false);
		row["mirror_bones"] = animation->get_meta("mirror_bones", Array());
		row["rotation_offsets"] = animation->get_meta("rotation_offsets", Array());
		row["hips_position_offsets"] = animation->get_meta("hips_position_offsets", Array());
		if (animation->has_meta("locomotion_gait")) row["locomotion_gait"] = animation->get_meta("locomotion_gait");
		if (animation->has_meta("align_hips_axes")) row["align_hips_axes"] = animation->get_meta("align_hips_axes");
		if (bool(p_job.get("include_hips_positions", false))) {
			const int hips = animation->find_track(NodePath("%GeneralSkeleton:Hips"), Animation::TYPE_POSITION_3D);
			ERR_FAIL_COND_V(hips < 0 || animation->track_get_key_count(hips) == 0, ERR_INVALID_DATA);
			const Vector3 first = animation->position_track_interpolate(hips, 0);
			const Vector3 last = animation->position_track_interpolate(hips, animation->get_length());
			ERR_FAIL_COND_V(!first.is_finite() || !last.is_finite(), ERR_INVALID_DATA);
			Array first_position, last_position;
			for (int axis = 0; axis < 3; ++axis) {
				first_position.push_back(first[axis]); last_position.push_back(last[axis]);
			}
			row["hips_first_position_m"] = first_position;
			row["hips_last_position_m"] = last_position;
		}
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
		const Variant from_composition = p_job.get("expected_changed_from_composition", false);
		ERR_FAIL_COND_V(from_composition.get_type() != Variant::BOOL || (bool(from_composition) && p_job.has("expected_changed")), ERR_INVALID_PARAMETER);
		const Variant changed_value = bool(from_composition) ? library->get_meta("source_replaced_ids", Variant()) : p_job.get("expected_changed", Array());
		ERR_FAIL_COND_V(changed_value.get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		const Array expected = changed_value;
		ERR_FAIL_COND_V(expected.size() > 256, ERR_PARAMETER_RANGE_ERROR);
		HashSet<String> expected_ids;
		for (const Variant &value : expected) {
			ERR_FAIL_COND_V(value.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
			const String id = value;
			ERR_FAIL_COND_V(!identifier(id) || expected_ids.has(id) || !base->has_animation(id), ERR_INVALID_PARAMETER);
			expected_ids.insert(id);
		}
		const Variant added_value = p_job.get("expected_added", Array());
		ERR_FAIL_COND_V(added_value.get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		const Array added = added_value;
		ERR_FAIL_COND_V(added.size() > 256, ERR_PARAMETER_RANGE_ERROR);
		HashSet<String> added_ids;
		for (const Variant &value : added) {
			ERR_FAIL_COND_V(value.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
			const String id = value;
			ERR_FAIL_COND_V(!identifier(id) || added_ids.has(id) || base->has_animation(id) || !library->has_animation(id), ERR_INVALID_PARAMETER);
			added_ids.insert(id);
		}
		LocalVector<StringName> names; base->get_animation_list(&names);
		ERR_FAIL_COND_V(names.size() > 256 || library->get_animation_list_size() != int(names.size() + added.size()), ERR_INVALID_DATA);
		Array changed, retained;
		uint64_t compared_keys = 0;
		for (const StringName &name : names) {
			ERR_FAIL_COND_V(!library->has_animation(name), ERR_INVALID_DATA);
			if (bool(from_composition)) {
				const Ref<Animation> before = base->get_animation(name), after = library->get_animation(name);
				ERR_FAIL_COND_V(before->get_length() != after->get_length() || before->get_loop_mode() != after->get_loop_mode(), ERR_INVALID_DATA);
				const int old_hips = before->find_track(NodePath("%GeneralSkeleton:Hips"), Animation::TYPE_POSITION_3D);
				const int new_hips = after->find_track(NodePath("%GeneralSkeleton:Hips"), Animation::TYPE_POSITION_3D);
				ERR_FAIL_COND_V(old_hips < 0 || new_hips < 0, ERR_INVALID_DATA);
				Ref<Animation> old_position, new_position; old_position.instantiate(); new_position.instantiate();
				before->copy_track(old_hips, old_position); after->copy_track(new_hips, new_position);
				ERR_FAIL_COND_V(!same_motion(old_position, new_position, compared_keys), ERR_INVALID_DATA);
			}
			const bool same = same_motion(base->get_animation(name), library->get_animation(name), compared_keys);
			ERR_FAIL_COND_V(compared_keys > 4000000, ERR_OUT_OF_MEMORY);
			ERR_FAIL_COND_V_MSG(same == expected_ids.has(name), ERR_INVALID_DATA, "Animation replacement comparison differs: " + String(name));
			if (same) retained.push_back(String(name)); else changed.push_back(String(name));
		}
		report["compared_base"] = compare_path;
		report["base_sha256"] = FileAccess::get_sha256(compare_path);
		report["changed_ids"] = changed; report["retained_ids"] = retained;
		report["added_ids"] = added;
		report["compared_keys"] = compared_keys;
		if (bool(from_composition)) report["playback_and_hips_preserved"] = true;
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
