#include "veya_memory_profile.h"

#include "profiling.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#if defined(GODOT_USE_TRACY)
#include "veya_memory_registry.h"
#include <tracy/TracyC.h>

namespace {
constexpr size_t kRegistryCapacity = 1u << 24;
const char *const kNames[] = { "Godot", "Veya C++", "Flecs", "Luau runtime", "Luau descriptor", "Luau configuration", "Luau agent", "RmlUi C++" };
std::mutex memory_mutex;
VeyaMemoryRegistry registry;
VeyaMemorySnapshot counters;
thread_local bool recording = false;

uint32_t EnvironmentNumber(const char *p_name, uint32_t p_default, uint32_t p_maximum) {
	const char *text = std::getenv(p_name);
	if (!text || !*text) {
		return p_default;
	}
	char *end = nullptr;
	const unsigned long value = std::strtoul(text, &end, 10);
	return end != text && !*end && value <= p_maximum ? static_cast<uint32_t>(value) : p_default;
}

bool Enabled() {
	static const bool enabled = [] {
		const char *mode = std::getenv("VEYA_TRACY_MEMORY");
		return mode && std::strcmp(mode, "on") == 0;
	}();
	return enabled;
}

void Initialize() {
	static const bool initialized = [] {
		counters.enabled = Enabled();
		counters.sample_every = EnvironmentNumber("VEYA_TRACY_MEMORY_SAMPLE_EVERY", 64, 4096);
		if (!counters.sample_every || (counters.sample_every & (counters.sample_every - 1))) {
			counters.sample_every = 64;
		}
		counters.stack_depth = EnvironmentNumber("VEYA_TRACY_MEMORY_STACK_DEPTH", 12, 64);
		if (counters.enabled) {
			auto *entries = static_cast<VeyaMemoryEntry *>(std::calloc(kRegistryCapacity, sizeof(VeyaMemoryEntry)));
			if (entries) {
				for (size_t index = 0; index < kRegistryCapacity; ++index) {
					new (&entries[index]) VeyaMemoryEntry;
				}
			}
			registry.Initialize(entries, entries ? kRegistryCapacity : 0);
			counters.registry_capacity = entries ? kRegistryCapacity : 0;
			counters.registry_bytes = entries ? kRegistryCapacity * sizeof(VeyaMemoryEntry) : 0;
		}
		return true;
	}();
	(void)initialized;
}

uint64_t Connection() {
	return tracy::ProfilerAvailable() && tracy::GetProfiler().IsConnected() ? tracy::GetProfiler().ConnectionId() : 0;
}

void AllocateLocked(const void *p_pointer, size_t p_bytes, uint32_t p_domain) {
	if (!p_pointer || p_domain >= counters.domain_count) {
		return;
	}
	auto &pool = counters.domains[p_domain];
	++pool.allocations;
	pool.allocated_bytes += p_bytes;
	const uint64_t connection = Connection();
	const bool sample = connection && registry.Sample(reinterpret_cast<uintptr_t>(p_pointer), p_bytes, counters.sample_every, counters.large_threshold);
	if (!registry.Insert({ reinterpret_cast<uintptr_t>(p_pointer), p_bytes, sample ? connection : 0, p_domain })) {
		return;
	}
	pool.live_bytes += p_bytes;
	pool.peak_bytes = std::max(pool.peak_bytes, pool.live_bytes);
	if (sample) {
		___tracy_emit_memory_alloc_callstack_named(p_pointer, p_bytes, counters.stack_depth, 1, kNames[p_domain]);
		++pool.sampled_allocations;
	}
}

void FreeLocked(const void *p_pointer, uint32_t p_domain) {
	if (!p_pointer || p_domain >= counters.domain_count) {
		return;
	}
	VeyaMemoryEntry entry;
	if (!registry.Remove(reinterpret_cast<uintptr_t>(p_pointer), entry)) {
		++counters.domains[p_domain].unmatched_frees;
		return;
	}
	auto &pool = counters.domains[entry.domain];
	++pool.frees;
	pool.live_bytes -= entry.bytes;
	if (entry.connection && entry.connection == Connection()) {
		___tracy_emit_memory_free_named(p_pointer, 1, kNames[entry.domain]);
		++pool.sampled_frees;
	}
}
} // namespace

#if defined(_WIN32)
#define VEYA_MEMORY_EXPORT extern "C" __declspec(dllexport)
#else
#define VEYA_MEMORY_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VEYA_MEMORY_EXPORT void veya_tracy_v1_memory_allocate(const void *p_pointer, size_t p_bytes, uint32_t p_domain) {
	if (!Enabled() || recording) {
		return;
	}
	recording = true;
	Initialize();
	{
		std::lock_guard<std::mutex> lock(memory_mutex);
		AllocateLocked(p_pointer, p_bytes, p_domain);
	}
	recording = false;
}

VEYA_MEMORY_EXPORT void veya_tracy_v1_memory_free(const void *p_pointer, uint32_t p_domain) {
	if (!Enabled() || recording) {
		return;
	}
	recording = true;
	Initialize();
	{
		std::lock_guard<std::mutex> lock(memory_mutex);
		FreeLocked(p_pointer, p_domain);
	}
	recording = false;
}

VEYA_MEMORY_EXPORT void *veya_tracy_v1_memory_reallocate(void *p_pointer, size_t p_bytes, uint32_t p_domain, VeyaReallocate p_reallocate) {
	if (!Enabled() || recording) {
		return p_reallocate(p_pointer, p_bytes);
	}
	recording = true;
	Initialize();
	void *result = nullptr;
	{
		// Serialize the real realloc and its events. A different thread cannot
		// report a reused address between freeing the old block and recording it.
		std::lock_guard<std::mutex> lock(memory_mutex);
		result = p_reallocate(p_pointer, p_bytes);
		if (result || p_bytes == 0) {
			FreeLocked(p_pointer, p_domain);
			AllocateLocked(result, p_bytes, p_domain);
		} else {
			++counters.failed_reallocations;
		}
	}
	recording = false;
	return result;
}

VEYA_MEMORY_EXPORT void veya_tracy_v1_memory_snapshot(VeyaMemorySnapshot *r_snapshot) {
	Initialize();
	std::lock_guard<std::mutex> lock(memory_mutex);
	counters.registry_live = registry.Live();
	counters.registry_overflows = registry.Overflows();
	*r_snapshot = counters;
}

void VeyaProfileMemoryAllocate(const void *p_pointer, size_t p_bytes) {
	veya_tracy_v1_memory_allocate(p_pointer, p_bytes, 0);
}
void VeyaProfileMemoryFree(const void *p_pointer) {
	veya_tracy_v1_memory_free(p_pointer, 0);
}
void *VeyaProfileMemoryReallocate(void *p_pointer, size_t p_bytes, VeyaReallocate p_reallocate) {
	return veya_tracy_v1_memory_reallocate(p_pointer, p_bytes, 0, p_reallocate);
}
#else
void VeyaProfileMemoryAllocate(const void *, size_t) {}
void VeyaProfileMemoryFree(const void *) {}
void *VeyaProfileMemoryReallocate(void *p_pointer, size_t p_bytes, VeyaReallocate p_reallocate) {
	return p_reallocate(p_pointer, p_bytes);
}
#endif
