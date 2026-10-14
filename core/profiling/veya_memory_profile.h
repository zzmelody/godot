#pragma once

#include <cstddef>
#include <cstdint>

// Value-only ABI shared with the extension. No Godot objects or allocator state
// cross this boundary. The engine owns the single Tracy client and registry.
enum class VeyaMemoryDomain : uint32_t {
	kGodot,
	kCpp,
	kFlecs,
	kLuauRuntime,
	kLuauDescriptor,
	kLuauConfiguration,
	kLuauAgent,
	kRmlUi,
	kCount,
};

struct VeyaMemoryCounters {
	uint64_t allocations = 0, frees = 0;
	uint64_t live_bytes = 0, peak_bytes = 0, allocated_bytes = 0;
	uint64_t sampled_allocations = 0, sampled_frees = 0;
	uint64_t unmatched_frees = 0;
};

struct VeyaMemorySnapshot {
	uint32_t enabled = 0, sample_every = 64, stack_depth = 12;
	uint32_t domain_count = static_cast<uint32_t>(VeyaMemoryDomain::kCount);
	uint64_t large_threshold = 65536, registry_bytes = 0;
	uint64_t registry_capacity = 0, registry_live = 0, registry_overflows = 0;
	uint64_t failed_reallocations = 0;
	VeyaMemoryCounters domains[static_cast<uint32_t>(VeyaMemoryDomain::kCount)]{};
};

using VeyaReallocate = void *(*)(void *, size_t);

void VeyaProfileMemoryAllocate(const void *p_pointer, size_t p_bytes);
void VeyaProfileMemoryFree(const void *p_pointer);
void *VeyaProfileMemoryReallocate(void *p_pointer, size_t p_bytes, VeyaReallocate p_reallocate);

