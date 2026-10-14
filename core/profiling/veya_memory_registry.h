#pragma once

#include "veya_memory_profile.h"

struct VeyaMemoryEntry {
	uintptr_t pointer = 0;
	uint64_t bytes = 0, connection = 0;
	uint32_t domain = 0;
};

// Caller supplies fixed storage and serializes access. No allocations, engine
// calls or resource ownership occur in this index. Back-shift deletion avoids
// accumulating tombstones during long captures with many short-lived blocks.
class VeyaMemoryRegistry {
	VeyaMemoryEntry *entries = nullptr;
	size_t capacity = 0, live = 0;
	uint64_t overflows = 0;

	static uint64_t Hash(uintptr_t p_pointer) {
		uint64_t value = static_cast<uint64_t>(p_pointer) >> 4;
		value ^= value >> 30;
		value *= UINT64_C(0xbf58476d1ce4e5b9);
		value ^= value >> 27;
		value *= UINT64_C(0x94d049bb133111eb);
		return value ^ (value >> 31);
	}

public:
	void Initialize(VeyaMemoryEntry *p_entries, size_t p_capacity) {
		entries = p_entries;
		capacity = p_capacity;
		live = 0;
		overflows = 0;
	}
	bool Insert(VeyaMemoryEntry p_entry) {
		if (!entries || capacity == 0 || (capacity & (capacity - 1)) || p_entry.pointer <= 1) {
			++overflows;
			return false;
		}
		size_t slot = Hash(p_entry.pointer) & (capacity - 1);
		for (size_t visited = 0; visited < capacity; ++visited) {
			auto &entry = entries[slot];
			if (entry.pointer == p_entry.pointer) {
				++overflows; // A duplicate live address indicates an incomplete hook.
				return false;
			}
			if (entry.pointer == 0) {
				entry = p_entry;
				++live;
				return true;
			}
			slot = (slot + 1) & (capacity - 1);
		}
		++overflows;
		return false;
	}
	bool Remove(uintptr_t p_pointer, VeyaMemoryEntry &r_entry) {
		if (!entries || !capacity || p_pointer <= 1) {
			return false;
		}
		size_t slot = Hash(p_pointer) & (capacity - 1);
		for (size_t visited = 0; visited < capacity; ++visited) {
			auto &entry = entries[slot];
			if (entry.pointer == 0) {
				return false;
			}
			if (entry.pointer == p_pointer) {
				r_entry = entry;
				size_t hole = slot;
				size_t next = (slot + 1) & (capacity - 1);
				for (size_t shifted = 0; shifted < capacity - 1 && entries[next].pointer; ++shifted) {
					const size_t ideal = Hash(entries[next].pointer) & (capacity - 1);
					if (((hole - ideal) & (capacity - 1)) < ((next - ideal) & (capacity - 1))) {
						entries[hole] = entries[next];
						hole = next;
					}
					next = (next + 1) & (capacity - 1);
				}
				entries[hole] = {};
				--live;
				return true;
			}
			slot = (slot + 1) & (capacity - 1);
		}
		return false;
	}
	bool Sample(uintptr_t p_pointer, uint64_t p_bytes, uint32_t p_every, uint64_t p_large) const {
		return p_bytes >= p_large || (Hash(p_pointer) & (p_every - 1)) == 0;
	}
	size_t Live() const { return live; }
	uint64_t Overflows() const { return overflows; }
};
