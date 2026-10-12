#pragma once

// Internal host/extension ABI. Copied verbatim into the Godot fork by the
// build helper; neither this header nor libange depends on Tracy headers.
#include <cstdint>

inline constexpr uint32_t ANGE_TRACY_ABI_VERSION = 1;
inline constexpr uint32_t ANGE_TRACY_STORAGE_SIZE = 64;
inline constexpr uint32_t ANGE_TRACY_STORAGE_ALIGNMENT = 16;
inline constexpr uint32_t ANGE_TRACY_ON_DEMAND = 1;

struct alignas(ANGE_TRACY_STORAGE_ALIGNMENT) AngeTracyZoneStorage {
  unsigned char bytes[ANGE_TRACY_STORAGE_SIZE];
};

struct AngeTracyHostApiV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t storage_size;
  uint32_t storage_alignment;
  uint32_t sdk_version; // major << 16 | minor << 8 | patch; diagnostic only
  uint32_t flags;
  uint32_t (*is_connected)();
  // Returns constructed, not active. Every nonzero result needs one zone_end,
  // on the same thread, even if the viewer disconnected during construction.
  uint32_t (*zone_begin)(AngeTracyZoneStorage *, uint32_t line,
                        const char *file, uint64_t file_size,
                        const char *function, uint64_t function_size,
                        const char *name, uint64_t name_size);
  void (*zone_end)(AngeTracyZoneStorage *);
};

using AngeTracyGetApiV1 = const AngeTracyHostApiV1 *(*)();
inline constexpr const char *ANGE_TRACY_GET_API_SYMBOL = "ange_tracy_get_api_v1";

// Tracy's allocated source location has 12 bytes of fixed fields/separators.
// Subtract before adding: invalid huge lengths must not wrap into a small sum.
inline bool ange_tracy_source_fits(uint64_t file, uint64_t function, uint64_t name) {
  uint64_t budget = 65535 - 12;
  const uint64_t lengths[] = {file, function, name};
  for (uint64_t length : lengths) {
    if (length > budget)
      return false;
    budget -= length;
  }
  return true;
}
