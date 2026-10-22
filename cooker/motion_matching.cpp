#include "motion_matching.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "asset_files.h"
#include "atomic_save.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/packed_scene.h"

namespace CookerMotionMatching {
namespace {
constexpr int kDimensions = 27;
constexpr double kRate = 60;
struct OwnedTree {
  Node* root = nullptr;
  ~OwnedTree() { if (root) memdelete(root); }
};
Skeleton3D* FindSkeleton(Node* node) {
  if (auto* skeleton = Object::cast_to<Skeleton3D>(node)) return skeleton;
  for (int i = 0; i < node->get_child_count(); ++i) {
    if (auto* skeleton = FindSkeleton(node->get_child(i))) return skeleton;
  }
  return nullptr;
}
struct Sample {
  Transform3D root;
  std::array<Vector3, 3> points;
};
struct Sampler {
  Skeleton3D* skeleton;
  Ref<Animation> animation;
  std::vector<int> positions, rotations, scales, parents;
  std::vector<Transform3D> globals;
  int trajectory = -1;
  std::array<int, 3> bones{};
  Basis rest_inverse;
  Error Prepare(Skeleton3D* target, const Ref<Animation>& source, const String& driver) {
    skeleton = target;
    animation = source;
    const int count = skeleton->get_bone_count();
    ERR_FAIL_COND_V(count < 1 || count > 128, ERR_INVALID_DATA);
    trajectory = skeleton->find_bone(driver);
    bones = {skeleton->find_bone("LeftFoot"), skeleton->find_bone("RightFoot"),
             skeleton->find_bone("Hips")};
    ERR_FAIL_COND_V(trajectory < 0 || bones[0] < 0 || bones[1] < 0 || bones[2] < 0,
                    ERR_INVALID_DATA);
    rest_inverse = skeleton->get_bone_global_rest(trajectory).basis.orthonormalized().inverse();
    globals.resize(count);
    for (int bone = 0; bone < count; ++bone) {
      const int parent = skeleton->get_bone_parent(bone);
      ERR_FAIL_COND_V(parent >= bone, ERR_INVALID_DATA);
      parents.push_back(parent);
      const NodePath path("%GeneralSkeleton:" + String(skeleton->get_bone_name(bone)));
      positions.push_back(source->find_track(path, Animation::TYPE_POSITION_3D));
      rotations.push_back(source->find_track(path, Animation::TYPE_ROTATION_3D));
      scales.push_back(source->find_track(path, Animation::TYPE_SCALE_3D));
    }
    return OK;
  }
  Sample At(double time) {
    for (int bone = 0; bone < skeleton->get_bone_count(); ++bone) {
      const Transform3D rest = skeleton->get_bone_rest(bone);
      const Vector3 position = positions[bone] >= 0 ?
          animation->position_track_interpolate(positions[bone], time) : rest.origin;
      const Quaternion rotation = rotations[bone] >= 0 ?
          animation->rotation_track_interpolate(rotations[bone], time) : rest.basis.get_rotation_quaternion();
      const Vector3 scale = scales[bone] >= 0 ?
          animation->scale_track_interpolate(scales[bone], time) : rest.basis.get_scale();
      const Transform3D local(Basis(rotation).scaled_local(scale), position);
      globals[bone] = parents[bone] >= 0 ? globals[parents[bone]] * local : local;
    }
    const Vector3 forward = (globals[trajectory].basis.orthonormalized() * rest_inverse).xform(Vector3(0, 0, 1));
    Sample sample;
    sample.root = Transform3D(Basis(Vector3(0, 1, 0), std::atan2(forward.x, forward.z)),
                              Vector3(globals[trajectory].origin.x, 0, globals[trajectory].origin.z));
    for (int i = 0; i < 3; ++i) sample.points[i] = globals[bones[i]].origin;
    return sample;
  }
};
Transform3D FutureRoot(Sampler& sampler, double time, double duration,
                       bool loop, const Transform3D& cycle) {
  Transform3D accumulated;
  if (loop) {
    while (time > duration) {
      accumulated = accumulated * cycle;
      time -= duration;
    }
  }
  return accumulated * sampler.At(std::min(time, duration)).root;
}
void StoreVector(PackedFloat32Array& values, int offset, Vector3 value,
                 double right_sign) {
  values.set(offset, value.x * right_sign);
  values.set(offset + 1, value.y);
  values.set(offset + 2, value.z);
}
Error BuildClip(Skeleton3D* skeleton, const Dictionary& declaration,
                double right_sign, Ref<Animation>& output, Dictionary& report) {
  const String path = declaration.get("source", "");
  ERR_FAIL_COND_V(CookerFiles::check_path(path) != OK, ERR_INVALID_PARAMETER);
  const Ref<AnimationLibrary> library = ResourceLoader::load(path, "AnimationLibrary");
  ERR_FAIL_COND_V(library.is_null() || library->get_animation_list_size() != 1, ERR_INVALID_DATA);
  LocalVector<StringName> names;
  library->get_animation_list(&names);
  const Ref<Animation> source = library->get_animation(names[0]);
  const double duration = source->get_length();
  ERR_FAIL_COND_V(!std::isfinite(duration) || duration < .1 || duration > 30, ERR_INVALID_DATA);
  const String driver = declaration.get("trajectory_bone", "");
  ERR_FAIL_COND_V(driver != "Root" && driver != "Hips", ERR_INVALID_PARAMETER);
  const bool loop = declaration.get("loop", false);
  const bool terminal_hold = declaration.get("terminal_hold", false);
  ERR_FAIL_COND_V(loop && terminal_hold, ERR_INVALID_PARAMETER);
  const Array contexts = declaration.get("contexts", Array());
  ERR_FAIL_COND_V(contexts.is_empty() || contexts.size() > 16, ERR_INVALID_PARAMETER);
  Sampler sampler;
  Error error = sampler.Prepare(skeleton, source, driver);
  ERR_FAIL_COND_V(error != OK, error);
  const int count = int(std::ceil(duration * kRate - 1e-6)) + 1;
  std::vector<Sample> samples;
  samples.reserve(count);
  for (int i = 0; i < count; ++i) samples.push_back(sampler.At(std::min(i / kRate, duration)));
  for (const auto& sample : samples) {
    ERR_FAIL_COND_V(!sample.root.is_finite(), ERR_INVALID_DATA);
    for (const auto& point : sample.points) ERR_FAIL_COND_V(!point.is_finite(), ERR_INVALID_DATA);
  }
  // Holding is only legal for a source whose endpoint is actually stationary.
  if (terminal_hold) {
    const double window = std::min(.2, duration);
    const auto earlier = sampler.At(duration - window);
    ERR_FAIL_COND_V_MSG((samples.back().root.origin - earlier.root.origin).length() / window > .25,
                        ERR_INVALID_DATA, "MM terminal hold requires a stationary endpoint: " + path);
  }
  const Transform3D cycle = samples.back().root * samples.front().root.affine_inverse();
  output = source->duplicate(true);
  output->set_loop_mode(loop ? Animation::LOOP_LINEAR : Animation::LOOP_NONE);
  // Replace only hierarchy roots. Inverse planar motion then applies exactly
  // once to every descendant, including the retained Hips translation.
  for (int bone = 0; bone < skeleton->get_bone_count(); ++bone) {
    if (skeleton->get_bone_parent(bone) >= 0) continue;
    const NodePath path_name("%GeneralSkeleton:" + String(skeleton->get_bone_name(bone)));
    for (auto type : {Animation::TYPE_POSITION_3D, Animation::TYPE_ROTATION_3D}) {
      const int existing = output->find_track(path_name, type);
      if (existing >= 0) output->remove_track(existing);
    }
    const int position_track = output->add_track(Animation::TYPE_POSITION_3D);
    const int rotation_track = output->add_track(Animation::TYPE_ROTATION_3D);
    output->track_set_path(position_track, path_name);
    output->track_set_path(rotation_track, path_name);
    for (int i = 0; i < count; ++i) {
      const double time = std::min(i / kRate, duration);
      sampler.At(time);
      const Transform3D local = samples[i].root.affine_inverse() * sampler.globals[bone];
      output->position_track_insert_key(position_track, time, local.origin);
      output->rotation_track_insert_key(rotation_track, time, local.basis.get_rotation_quaternion());
    }
  }
  PackedFloat32Array features, contacts, roots;
  PackedByteArray eligible;
  features.resize(count * kDimensions);
  contacts.resize(count * 2);
  roots.resize(count * 3);
  eligible.resize(count);
  double ground[2] = {1e9, 1e9};
  for (const auto& sample : samples) for (int side = 0; side < 2; ++side)
    ground[side] = std::min(ground[side], double(sample.points[side].y));
  int eligible_count = 0;
  double maximum_speed = 0;
  for (int i = 0; i < count; ++i) {
    const auto& sample = samples[i];
    const double time = std::min(i / kRate, duration);
    const int before = std::max(0, i - 1), after = std::min(count - 1, i + 1);
    const double dt = std::min(after / kRate, duration) - before / kRate;
    const Transform3D inverse = sample.root.affine_inverse();
    for (int side = 0; side < 2; ++side) {
      StoreVector(features, i * kDimensions + side * 3, inverse.xform(sample.points[side]), right_sign);
    }
    for (int joint = 0; joint < 3; ++joint) {
      const Vector3 velocity = (samples[after].points[joint] - samples[before].points[joint]) / dt;
      StoreVector(features, i * kDimensions + 6 + joint * 3, inverse.basis.xform(velocity), right_sign);
      if (joint < 2) {
        const double height = sample.points[joint].y - ground[joint];
        const double height_weight = std::clamp((.08 - height) / .055, 0.0, 1.0);
        const double speed_weight = std::clamp((.5 - velocity.length()) / .4, 0.0, 1.0);
        contacts.set(i * 2 + joint, height_weight * speed_weight);
      }
    }
    const Vector3 root_velocity = (samples[after].root.origin - samples[before].root.origin) / dt;
    maximum_speed = std::max(maximum_speed, double(root_velocity.length()));
    roots.set(i * 3, sample.root.origin.x);
    roots.set(i * 3 + 1, sample.root.origin.z);
    roots.set(i * 3 + 2, std::atan2(sample.root.basis.get_column(2).x, sample.root.basis.get_column(2).z));
    constexpr std::array<double, 3> kHorizons{.2, .4, .8};
    for (int h = 0; h < 3; ++h) {
      const Transform3D future = inverse * FutureRoot(sampler, time + kHorizons[h], duration, loop, cycle);
      features.set(i * kDimensions + 15 + h * 2, future.origin.x * right_sign);
      features.set(i * kDimensions + 16 + h * 2, future.origin.z);
      features.set(i * kDimensions + 21 + h * 2, future.basis.get_column(2).x * right_sign);
      features.set(i * kDimensions + 22 + h * 2, future.basis.get_column(2).z);
    }
    const bool valid = (loop || terminal_hold || time + .8 <= duration) && time < duration;
    eligible.set(i, valid ? 1 : 0);
    eligible_count += valid;
  }
  ERR_FAIL_COND_V(eligible_count == 0, ERR_INVALID_DATA);
  for (int i = 0; i < features.size(); ++i) {
    ERR_FAIL_COND_V_MSG(!std::isfinite(features[i]), ERR_INVALID_DATA,
                        "Nonfinite MM feature: " + path + " channel=" + itos(i % 27) + " sample=" + itos(i / 27));
  }
  for (int side = 0; side < 2; ++side) {
    const int track = output->add_track(Animation::TYPE_VALUE);
    output->track_set_path(track, NodePath(side == 0 ? "CharacterPoseSignals:left_contact" : "CharacterPoseSignals:right_contact"));
    output->value_track_set_update_mode(track, Animation::UPDATE_CONTINUOUS);
    for (int i = 0; i < count; ++i)
      output->track_insert_key(track, std::min(i / kRate, duration), double(contacts[i * 2 + side]));
  }
  Dictionary data;
  data["features"] = features;
  data["contacts"] = contacts;
  data["eligible"] = eligible;
  data["contexts"] = contexts;
  data["root_trajectory"] = roots;
  data["sample_rate"] = kRate;
  data["trajectory_bone"] = driver;
  data["source"] = path;
  data["source_sha256"] = FileAccess::get_sha256(path);
  data["terminal_hold"] = terminal_hold;
  output->set_meta("motion_matching", data);
  report["id"] = declaration.get("id", "");
  report["samples"] = count;
  report["searchable_seconds"] = eligible_count / kRate;
  report["duration"] = duration;
  report["maximum_root_speed"] = maximum_speed;
  report["contexts"] = contexts;
  report["trajectory_bone"] = driver;
  report["visual_status"] = "requires_runtime_review";
  return OK;
}
}  // namespace
Error Build(const Dictionary& job, Dictionary& result) {
  for (const char* key : {"base_library", "target_model", "output"}) {
    ERR_FAIL_COND_V(job.get(key, Variant()).get_type() != Variant::STRING ||
                        CookerFiles::check_path(job[key], String(key) == "output") != OK,
                    ERR_INVALID_PARAMETER);
  }
  const String output = job["output"];
  ERR_FAIL_COND_V(CookerAtomicSave::occupied(output), ERR_ALREADY_EXISTS);
  const Ref<AnimationLibrary> base = ResourceLoader::load(job["base_library"], "AnimationLibrary");
  const Ref<PackedScene> model = ResourceLoader::load(job["target_model"], "PackedScene");
  ERR_FAIL_COND_V(base.is_null() || model.is_null(), ERR_INVALID_DATA);
  OwnedTree tree{model->instantiate()};
  auto* skeleton = tree.root ? FindSkeleton(tree.root) : nullptr;
  ERR_FAIL_COND_V(!skeleton, ERR_INVALID_DATA);
  const Array declarations = job.get("clips", Array());
  ERR_FAIL_COND_V(declarations.is_empty() || declarations.size() > 64, ERR_INVALID_PARAMETER);
  const double right_sign = job.get("feature_right_sign", -1.0);
  ERR_FAIL_COND_V(right_sign != -1 && right_sign != 1, ERR_INVALID_PARAMETER);
  Ref<AnimationLibrary> library = base->duplicate(true);
  Array reports;
  for (const Dictionary& declaration : declarations) {
    const String id = declaration.get("id", "");
    ERR_FAIL_COND_V(!id.begins_with("mm.") || id.length() > 96 || id.contains("/") ||
                        id.contains(":") || library->has_animation(id), ERR_INVALID_PARAMETER);
    Ref<Animation> animation;
    Dictionary report;
    const Error error = BuildClip(skeleton, declaration, right_sign, animation, report);
    ERR_FAIL_COND_V(error != OK, error);
    library->add_animation(id, animation);
    reports.push_back(report);
  }
  ERR_FAIL_COND_V(library->get_animation_list_size() > 256, ERR_INVALID_DATA);
  std::array<double, kDimensions> sums{}, squares{};
  int sample_count = 0;
  for (const Dictionary& declaration : declarations) {
    const Ref<Animation> animation = library->get_animation(declaration["id"]);
    const Dictionary record = animation->get_meta("motion_matching");
    const PackedFloat32Array features = record["features"];
    const PackedByteArray eligible = record["eligible"];
    for (int sample = 0; sample < eligible.size(); ++sample) {
      if (!eligible[sample]) continue;
      ++sample_count;
      for (int i = 0; i < kDimensions; ++i) {
        const double value = features[sample * kDimensions + i];
        sums[i] += value;
        squares[i] += value * value;
      }
    }
  }
  ERR_FAIL_COND_V(sample_count == 0, ERR_INVALID_DATA);
  PackedFloat32Array mean, inverse_deviation;
  for (int i = 0; i < kDimensions; ++i) {
    const double average = sums[i] / sample_count;
    const double variance = std::max(0.0, squares[i] / sample_count - average * average);
    mean.push_back(average);
    inverse_deviation.push_back(variance > 1e-8 ? 1 / std::sqrt(variance) : 0);
  }
  for (const Dictionary& declaration : declarations) {
    const Ref<Animation> animation = library->get_animation(declaration["id"]);
    Dictionary record = animation->get_meta("motion_matching");
    record["mean"] = mean;
    record["inverse_deviation"] = inverse_deviation;
    animation->set_meta("motion_matching", record);
  }
  const Error directory = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));
  ERR_FAIL_COND_V(directory != OK, directory);
  const Error saved = CookerAtomicSave::save_resource(library, output,
      ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
  ERR_FAIL_COND_V(saved != OK, saved);
  result["output"] = output;
  result["clips"] = reports;
  result["feature_dimensions"] = kDimensions;
  return OK;
}
}  // namespace CookerMotionMatching
