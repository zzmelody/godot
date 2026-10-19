// SPDX-License-Identifier: MIT
#pragma once

#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_device.h"

#include <array>

namespace RendererRD {

// One render view owns these bindings. Reordering source batches must not
// recreate descriptor sets. Each recording retains only its current resources;
// RD invalidates a set when any borrowed buffer or program is destroyed.
class VirtualGeometryUniformCache {
	struct Key {
		RID program;
		std::array<RID, 5> resources;
		bool operator==(const Key &p_other) const {
			return program == p_other.program && resources == p_other.resources;
		}
	};
	struct Hasher {
		static uint32_t hash(const Key &p_key) {
			uint32_t value = hash_murmur3_one_64(p_key.program.get_id());
			for (RID resource : p_key.resources) {
				value = hash_murmur3_one_64(resource.get_id(), value);
			}
			return hash_fmix32(value);
		}
	};
	struct Entry {
		RID uniforms;
		uint64_t epoch = 0;
	};
	HashMap<Key, Entry, Hasher> entries;
	std::array<uint32_t, 5> bindings;
	uint32_t uniform_mask = 0;
	uint64_t epoch = 0;

	void clear() {
		for (const KeyValue<Key, Entry> &entry : entries) {
			if (RD::get_singleton()->uniform_set_is_valid(entry.value.uniforms)) {
				RD::get_singleton()->free_rid(entry.value.uniforms);
			}
		}
		entries.clear();
	}

	void prune() {
		for (auto entry = entries.begin(); entry;) {
			auto current = entry;
			++entry;
			if (current->value.epoch != epoch) {
				if (RD::get_singleton()->uniform_set_is_valid(current->value.uniforms)) {
					RD::get_singleton()->free_rid(current->value.uniforms);
				}
				entries.erase(current->key);
			}
		}
	}

public:
	class Frame {
		VirtualGeometryUniformCache &owner;

	public:
		explicit Frame(VirtualGeometryUniformCache &p_owner) : owner(p_owner) {}
		Frame(const Frame &) = delete;
		Frame &operator=(const Frame &) = delete;
		~Frame() { owner.prune(); }
	};

	explicit VirtualGeometryUniformCache(const std::array<uint32_t, 5> &p_bindings, uint32_t p_uniform_mask = 0) :
			bindings(p_bindings), uniform_mask(p_uniform_mask) {}
	VirtualGeometryUniformCache(const VirtualGeometryUniformCache &) = delete;
	VirtualGeometryUniformCache &operator=(const VirtualGeometryUniformCache &) = delete;
	~VirtualGeometryUniformCache() { clear(); }

	Frame begin() {
		if (++epoch == 0) {
			clear();
			epoch = 1;
		}
		return Frame(*this);
	}

	RID get(RID p_program, const std::array<RID, 5> &p_resources) {
		Entry &entry = entries[Key{ p_program, p_resources }];
		entry.epoch = epoch;
		if (!RD::get_singleton()->uniform_set_is_valid(entry.uniforms)) {
			Vector<RD::Uniform> uniforms;
			uniforms.resize(bindings.size());
			for (uint32_t index = 0; index < bindings.size(); ++index) {
				const auto type = (uniform_mask & (1u << index)) != 0 ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
				uniforms.write[index] = RD::Uniform(type, bindings[index], { p_resources[index] });
			}
			entry.uniforms = RD::get_singleton()->uniform_set_create(uniforms, p_program, 0);
		}
		return entry.uniforms;
	}
};

} // namespace RendererRD
