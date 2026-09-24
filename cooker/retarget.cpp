/*<<----- VEYA_COOKER: use Godot's skeleton import processors from a declarative Luau batch. */
#include "retarget.h"

#include "cooker/asset_files.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "editor/import/3d/resource_importer_scene.h"
#include "modules/fbx/editor/editor_scene_importer_ufbx.h"
#include "modules/gltf/gltf_state.h"
#include "scene/resources/bone_map.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/packed_scene.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"

namespace CookerRetarget {

static void apply_import_defaults(const Ref<ResourceImporterScene> &p_importer, const String &p_source, const Ref<BoneMap> &p_map, HashMap<StringName, Variant> &r_options) {
	List<ResourceImporter::ImportOption> definitions;
	p_importer->get_import_options(p_source, &definitions);
	for (const ResourceImporter::ImportOption &option : definitions) {
		r_options[option.option.name] = option.default_value;
	}
	r_options["fbx/importer"] = EditorSceneFormatImporterUFBX::FBX_IMPORTER_UFBX;
	r_options["fbx/embedded_image_handling"] = GLTFState::HANDLE_BINARY_IMAGE_MODE_EMBED_AS_UNCOMPRESSED;
	r_options["_cooker_retarget_bone_map"] = p_map;
}

Error make_bone_map(const Dictionary &p_job, Dictionary &r_result) {
	ERR_FAIL_COND_V(p_job.get("output", Variant()).get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_job.get("bones", Variant()).get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
	String output = p_job["output"];
	ERR_FAIL_COND_V(output.get_extension().to_lower() != "res", ERR_INVALID_PARAMETER);
	Error error = CookerFiles::check_path(output, true);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V_MSG(FileAccess::exists(output), ERR_ALREADY_EXISTS, "Bone map output already exists.");
	Ref<SkeletonProfileHumanoid> profile;
	profile.instantiate();
	Ref<BoneMap> map;
	map.instantiate();
	map->set_profile(profile);
	Dictionary bones = p_job["bones"];
	ERR_FAIL_COND_V(bones.is_empty() || bones.size() > profile->get_bone_size(), ERR_INVALID_PARAMETER);
	HashSet<String> used_source_bones;
	for (const Variant *key = bones.next(); key; key = bones.next(key)) {
		ERR_FAIL_COND_V(key->get_type() != Variant::STRING || bones[*key].get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
		String profile_bone = *key;
		String source_bone = bones[*key];
		ERR_FAIL_COND_V(profile->find_bone(profile_bone) < 0 || source_bone.is_empty() || source_bone.length() > 128, ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(used_source_bones.has(source_bone), ERR_INVALID_PARAMETER);
		used_source_bones.insert(source_bone);
		map->set_skeleton_bone_name(profile_bone, source_bone);
	}
	for (int i = 0; i < profile->get_bone_size(); i++) {
		if (profile->is_required(i)) {
			ERR_FAIL_COND_V_MSG(map->get_skeleton_bone_name(profile->get_bone_name(i)).is_empty(), ERR_INVALID_PARAMETER, "Required humanoid bone is unmapped: " + String(profile->get_bone_name(i)));
		}
	}
	error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));
	ERR_FAIL_COND_V(error != OK, error);
	error = ResourceSaver::save(map, output, ResourceSaver::FLAG_COMPRESS);
	ERR_FAIL_COND_V(error != OK, error);
	r_result["output"] = output;
	r_result["profile"] = "SkeletonProfileHumanoid";
	r_result["mapped_bones"] = bones.size();
	return OK;
}

Error retarget_animations(const Dictionary &p_job, Dictionary &r_result) {
	for (const char *name : { "source_dir", "output_dir", "bone_map" }) {
		ERR_FAIL_COND_V(p_job.get(name, Variant()).get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	}
	String source_dir = p_job["source_dir"];
	String output_dir = p_job["output_dir"];
	String map_path = p_job["bone_map"];
	String selected_animation = p_job.get("animation_name", "");
	ERR_FAIL_COND_V(p_job.has("animation_name") && p_job["animation_name"].get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_job.has("in_place") && p_job["in_place"].get_type() != Variant::BOOL, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_job.has("loop") && p_job["loop"].get_type() != Variant::BOOL, ERR_INVALID_PARAMETER);
	bool in_place = p_job.get("in_place", false);
	bool loop = p_job.get("loop", false);
	Error error = CookerFiles::check_path(source_dir, false, false, false);
	ERR_FAIL_COND_V(error != OK, error);
	error = CookerFiles::check_path(output_dir.path_join("probe.res"), true);
	ERR_FAIL_COND_V(error != OK, error);
	error = CookerFiles::check_path(map_path);
	ERR_FAIL_COND_V(error != OK, error);
	Ref<BoneMap> map = ResourceLoader::load(map_path, "BoneMap", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	ERR_FAIL_COND_V(map.is_null() || map->get_profile().is_null(), error == OK ? ERR_INVALID_DATA : error);
	Ref<DirAccess> directory = DirAccess::open(source_dir, &error);
	ERR_FAIL_COND_V(directory.is_null(), error == OK ? ERR_FILE_NOT_FOUND : error);
	Vector<String> names;
	HashSet<String> excluded;
	HashSet<String> included;
	if (p_job.has("include")) {
		ERR_FAIL_COND_V(p_job["include"].get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		Array include = p_job["include"];
		ERR_FAIL_COND_V(include.is_empty() || include.size() > 512, ERR_INVALID_PARAMETER);
		for (const Variant &entry : include) {
			ERR_FAIL_COND_V(entry.get_type() != Variant::STRING || String(entry).get_extension().to_lower() != "fbx", ERR_INVALID_PARAMETER);
			included.insert(String(entry));
		}
	}
	if (p_job.has("exclude")) {
		ERR_FAIL_COND_V(p_job["exclude"].get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
		Array exclude = p_job["exclude"];
		ERR_FAIL_COND_V(exclude.size() > 64, ERR_INVALID_PARAMETER);
		for (const Variant &entry : exclude) {
			ERR_FAIL_COND_V(entry.get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
			excluded.insert(String(entry));
		}
	}
	error = directory->list_dir_begin();
	ERR_FAIL_COND_V(error != OK, error);
	for (String name = directory->get_next(); !name.is_empty(); name = directory->get_next()) {
		if (!directory->current_is_dir() && name.get_extension().to_lower() == "fbx" && !excluded.has(name) && (included.is_empty() || included.has(name))) {
			names.push_back(name);
		}
	}
	directory->list_dir_end();
	names.sort();
	ERR_FAIL_COND_V(names.is_empty() || names.size() > 512, ERR_INVALID_DATA);
	ERR_FAIL_COND_V_MSG(!included.is_empty() && names.size() != included.size(), ERR_FILE_NOT_FOUND, "One or more declared animation sources are missing.");
	error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output_dir));
	ERR_FAIL_COND_V(error != OK, error);
	Array clips;
	for (const String &name : names) {
		String source = source_dir.path_join(name);
		String output = output_dir.path_join(name.get_basename() + ".res");
		error = CookerFiles::check_path(source);
		ERR_FAIL_COND_V(error != OK, error);
		error = CookerFiles::check_path(output, true);
		ERR_FAIL_COND_V(error != OK, error);
		ERR_FAIL_COND_V_MSG(FileAccess::exists(output), ERR_ALREADY_EXISTS, "Retarget output already exists: " + output);
		Ref<ResourceImporterScene> importer;
		importer.instantiate();
		importer->set_scene_import_type("AnimationLibrary");
		HashMap<StringName, Variant> options;
		apply_import_defaults(importer, source, map, options);
		List<String> variants;
		List<String> generated;
		Variant metadata;
		error = importer->import(ResourceUID::INVALID_ID, source, output.get_basename(), options, &variants, &generated, &metadata);
		ERR_FAIL_COND_V_MSG(error != OK, error, "Retarget failed for " + source);
		Ref<AnimationLibrary> library = ResourceLoader::load(output, "AnimationLibrary", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
		ERR_FAIL_COND_V_MSG(library.is_null(), ERR_INVALID_DATA, "Retarget produced no library: " + source);
		LocalVector<StringName> animations;
		library->get_animation_list(&animations);
		ERR_FAIL_COND_V_MSG(animations.is_empty(), ERR_INVALID_DATA, "Retarget produced no animations: " + source);
		bool modified = false;
		if (!selected_animation.is_empty()) {
			ERR_FAIL_COND_V_MSG(!library->has_animation(selected_animation), ERR_INVALID_DATA, "Expected animation missing: " + source);
			for (const StringName &animation_name : animations) {
				if (animation_name != StringName(selected_animation)) {
					library->remove_animation(animation_name);
					modified = true;
				}
			}
			animations.clear();
			library->get_animation_list(&animations);
		}
		for (const StringName &animation_name : animations) {
			Ref<Animation> animation = library->get_animation(animation_name);
			if (loop) {
				animation->set_loop_mode(Animation::LOOP_LINEAR);
				modified = true;
			}
			int rotations = 0;
			bool found_hips = false;
			for (int track = 0; track < animation->get_track_count(); track++) {
				NodePath path = animation->track_get_path(track);
				if (in_place && animation->track_get_type(track) == Animation::TYPE_POSITION_3D && String(path) == "%GeneralSkeleton:Hips") {
					int keys = animation->track_get_key_count(track);
					ERR_FAIL_COND_V(keys == 0, ERR_INVALID_DATA);
					Vector3 origin = animation->track_get_key_value(track, 0);
					for (int key = 0; key < keys; key++) {
						Vector3 position = animation->track_get_key_value(track, key);
						position.x = origin.x;
						position.z = origin.z;
						animation->track_set_key_value(track, key, position);
					}
					found_hips = true;
					modified = true;
				}
				if (animation->track_get_type(track) == Animation::TYPE_ROTATION_3D && String(path).begins_with("%GeneralSkeleton:")) {
					rotations++;
				}
			}
			ERR_FAIL_COND_V_MSG(rotations < 10, ERR_INVALID_DATA, "Retarget lost humanoid rotation tracks: " + source);
			ERR_FAIL_COND_V_MSG(in_place && !found_hips, ERR_INVALID_DATA, "In-place animation has no Hips position track: " + source);
		}
		if (modified) {
			error = ResourceSaver::save(library, output, ResourceSaver::FLAG_COMPRESS);
			ERR_FAIL_COND_V(error != OK, error);
		}
		Dictionary clip;
		clip["source"] = source;
		clip["output"] = output;
		clip["animations"] = animations.size();
		clips.push_back(clip);
	}
	r_result["source_dir"] = source_dir;
	r_result["output_dir"] = output_dir;
	r_result["count"] = clips.size();
	r_result["clips"] = clips;
	return OK;
}

static Skeleton3D *find_skeleton(Node *p_root) {
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_root)) {
		return skeleton;
	}
	for (int i = 0; i < p_root->get_child_count(); i++) {
		if (Skeleton3D *skeleton = find_skeleton(p_root->get_child(i))) {
			return skeleton;
		}
	}
	return nullptr;
}

Error retarget_model(const Dictionary &p_job, Dictionary &r_result) {
	for (const char *name : { "source", "output", "bone_map" }) {
		ERR_FAIL_COND_V(p_job.get(name, Variant()).get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	}
	String source = p_job["source"];
	String output = p_job["output"];
	String map_path = p_job["bone_map"];
	ERR_FAIL_COND_V(source.get_extension().to_lower() != "fbx" || output.get_extension().to_lower() != "scn", ERR_INVALID_PARAMETER);
	Error error = CookerFiles::check_path(source);
	ERR_FAIL_COND_V(error != OK, error);
	error = CookerFiles::check_path(output, true);
	ERR_FAIL_COND_V(error != OK, error);
	error = CookerFiles::check_path(map_path);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V(FileAccess::exists(output), ERR_ALREADY_EXISTS);
	Ref<BoneMap> map = ResourceLoader::load(map_path, "BoneMap", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	ERR_FAIL_COND_V(map.is_null() || map->get_profile().is_null(), ERR_INVALID_DATA);
	Ref<ResourceImporterScene> importer;
	importer.instantiate();
	importer->set_scene_import_type("PackedScene");
	HashMap<StringName, Variant> options;
	apply_import_defaults(importer, source, map, options);
	error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));
	ERR_FAIL_COND_V(error != OK, error);
	List<String> variants;
	List<String> generated;
	Variant metadata;
	error = importer->import(ResourceUID::INVALID_ID, source, output.get_basename(), options, &variants, &generated, &metadata);
	ERR_FAIL_COND_V(error != OK, error);
	Ref<PackedScene> scene = ResourceLoader::load(output, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	ERR_FAIL_COND_V(scene.is_null(), ERR_INVALID_DATA);
	Node *root = scene->instantiate();
	ERR_FAIL_NULL_V(root, ERR_CANT_CREATE);
	Skeleton3D *skeleton = find_skeleton(root);
	bool valid = skeleton && skeleton->find_bone("Hips") >= 0 && skeleton->find_bone("LeftUpperArm") >= 0 && skeleton->find_bone("RightFoot") >= 0;
	int bone_count = skeleton ? skeleton->get_bone_count() : 0;
	memdelete(root);
	ERR_FAIL_COND_V_MSG(!valid, ERR_INVALID_DATA, "Model did not produce a Godot humanoid skeleton.");
	r_result["output"] = output;
	r_result["bone_count"] = bone_count;
	return OK;
}

Error make_animation_preview(const Dictionary &p_job, Dictionary &r_result) {
	for (const char *name : { "model", "animation", "output" }) {
		ERR_FAIL_COND_V(p_job.get(name, Variant()).get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	}
	String model_path = p_job["model"];
	String animation_path = p_job["animation"];
	String output = p_job["output"];
	ERR_FAIL_COND_V(output.get_extension().to_lower() != "scn", ERR_INVALID_PARAMETER);
	Error error = CookerFiles::check_path(model_path);
	ERR_FAIL_COND_V(error != OK, error);
	error = CookerFiles::check_path(animation_path);
	ERR_FAIL_COND_V(error != OK, error);
	error = CookerFiles::check_path(output, true);
	ERR_FAIL_COND_V(error != OK || FileAccess::exists(output), error == OK ? ERR_ALREADY_EXISTS : error);
	Ref<PackedScene> model = ResourceLoader::load(model_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	ERR_FAIL_COND_V(model.is_null(), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(p_job.get("animation_name", Variant()).get_type() != Variant::STRING, ERR_INVALID_PARAMETER);
	String animation_name = p_job["animation_name"];
	Ref<AnimationLibrary> library = ResourceLoader::load(animation_path, "AnimationLibrary", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	ERR_FAIL_COND_V(library.is_null() || !library->has_animation(animation_name), ERR_INVALID_DATA);
	Node *root = model->instantiate();
	ERR_FAIL_NULL_V(root, ERR_CANT_CREATE);
	if (!find_skeleton(root)) {
		memdelete(root);
		ERR_FAIL_V_MSG(ERR_INVALID_DATA, "Preview model has no skeleton.");
	}
	AnimationPlayer *player = memnew(AnimationPlayer);
	player->set_name("RetargetPreviewPlayer");
	root->add_child(player);
	player->set_owner(root);
	player->set_root_node(NodePath(".."));
	error = player->add_animation_library(StringName(), library);
	if (error != OK) {
		memdelete(root);
		return error;
	}
	player->set_autoplay(animation_name);
	Ref<PackedScene> preview;
	preview.instantiate();
	error = preview->pack(root);
	memdelete(root);
	ERR_FAIL_COND_V(error != OK, error);
	error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));
	ERR_FAIL_COND_V(error != OK, error);
	error = ResourceSaver::save(preview, output, ResourceSaver::FLAG_COMPRESS);
	ERR_FAIL_COND_V(error != OK, error);
	r_result["output"] = output;
	r_result["animation"] = animation_path;
	return OK;
}
}
/*>>----- VEYA_COOKER */
