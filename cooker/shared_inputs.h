#pragma once

#include "core/io/resource.h"
#include "core/templates/hash_set.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

#include <cstdint>
#include <map>
#include <set>

namespace CookerSharedInputs {
struct Input {
  Ref<Resource> resource;
  HashSet<String> files;
  Dictionary hashes;
  Dictionary md5;
  std::map<String, String> types;
  std::map<String, uint64_t> bytes;
};

// Retain repeated inputs only when their closure cannot be produced by this
// graph. The content-hash batch outlives this owner and freezes every file.
// No input cache survives a CLI pipeline.
class Batch {
 public:
  explicit Batch(const Array& steps);
  ~Batch();
  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;
  static Error Find(const String& path, const Input*& input);
  static Error Store(const String& path, const HashSet<String>& files,
                     const Dictionary& hashes, const Input*& input);
  uint64_t reuses() const {
    return reuses_;
  }

 private:
  bool Eligible(const String& path, const HashSet<String>& files) const;
  std::set<String> repeated_, output_directories_;
  std::map<String, Input> inputs_;
  uint64_t reuses_ = 0;
  Batch* previous_ = nullptr;
  static thread_local Batch* current_;
};
}  // namespace CookerSharedInputs
