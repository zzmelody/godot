#include "shared_inputs.h"

#include "content_hash.h"

#include "core/io/resource_loader.h"

#include <cstddef>
#include <utility>

namespace CookerSharedInputs {
namespace {
String Key(const String& path) {
  return path.simplify_path().to_lower();
}
}  // namespace
thread_local Batch* Batch::current_ = nullptr;

Batch::Batch(const Array& steps) : previous_(current_) {
  current_ = this;
#ifdef WINDOWS_ENABLED
  std::map<String, size_t> counts;
  for (const Dictionary& step : steps) {
    if (step.get("inputs", Variant()).get_type() == Variant::DICTIONARY) {
      const Dictionary inputs = step["inputs"];
      for (const Variant& key : inputs.keys()) {
        if (inputs[key].get_type() == Variant::STRING) {
          ++counts[Key(inputs[key])];
        }
      }
    }
    for (const auto* field : {"output", "output_dir"}) {
      if (step.get(field, Variant()).get_type() == Variant::STRING) {
        const String output = Key(step[field]);
        const String directory = String(field) == "output"
                                     ? output.get_base_dir()
                                     : output.trim_suffix("/");
        output_directories_.insert(directory + "/");
      }
    }
  }
  for (const auto& [path, count] : counts) {
    if (count > 1) {
      repeated_.insert(path);
    }
  }
#endif
}

Batch::~Batch() {
  current_ = previous_;
}

bool Batch::Eligible(const String& path, const HashSet<String>& files) const {
  if (!repeated_.count(Key(path))) {
    return false;
  }
  for (const auto& file : files) {
    const String key = Key(file);
    for (const auto& directory : output_directories_) {
      if (key.begins_with(directory)) {
        return false;
      }
    }
  }
  return true;
}

Error Batch::Find(const String& path, const Input*& input) {
  input = nullptr;
  if (!current_) {
    return OK;
  }
  const auto found = current_->inputs_.find(Key(path));
  if (found == current_->inputs_.end()) {
    return OK;
  }
  for (const auto& file : found->second.files) {
    String digest;
    uint64_t bytes = 0;
    const Error error = cooker_hash::PinSharedInputFile(file, digest, bytes);
    ERR_FAIL_COND_V(error != OK, error);
    ERR_FAIL_COND_V_MSG(digest != String(found->second.hashes[file]) ||
                            bytes != found->second.bytes.at(file),
                        ERR_BUSY,
                        "Shared recipe input changed during pipeline: " + file);
  }
  input = &found->second;
  ++current_->reuses_;
  return OK;
}

Error Batch::Store(const String& path, const HashSet<String>& files,
                   const Dictionary& hashes, const Input*& input) {
  input = nullptr;
  if (!current_ || !current_->Eligible(path, files)) {
    return OK;
  }
  Input prepared;
  prepared.files = files;
  for (const auto& file : files) {
    String digest;
    uint64_t bytes = 0;
    const Error error = cooker_hash::PinSharedInputFile(file, digest, bytes);
    ERR_FAIL_COND_V(error != OK, error);
    ERR_FAIL_COND_V_MSG(!hashes.has(file) || digest != String(hashes[file]),
                        ERR_BUSY,
                        "Shared recipe input changed before freezing: " + file);
    prepared.hashes[file] = digest;
    prepared.md5[file] = cooker_hash::Md5File(file);
    prepared.types.emplace(file, ResourceLoader::get_resource_type(file));
    prepared.bytes.emplace(file, bytes);
  }
  // Reload fresh objects under the leases. The binary loader preserves path
  // identities in IGNORE mode. REPLACE would reset live MultiMesh properties
  // in serialization order while their instance count is still nonzero.
  Error error = OK;
  prepared.resource = ResourceLoader::load(
      path, "", ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP, &error);
  ERR_FAIL_COND_V(error != OK || prepared.resource.is_null(),
                  error == OK ? ERR_FILE_CANT_READ : error);
  const auto stored = current_->inputs_.emplace(Key(path), std::move(prepared));
  input = &stored.first->second;
  return OK;
}
}  // namespace CookerSharedInputs
