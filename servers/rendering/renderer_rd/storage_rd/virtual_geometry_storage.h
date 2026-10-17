#ifndef VIRTUAL_GEOMETRY_STORAGE_RD_H
#define VIRTUAL_GEOMETRY_STORAGE_RD_H

#include "core/io/virtual_geometry/model_codec.h"
#include "core/os/mutex.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/typed_array.h"
#include "servers/rendering/renderer_rd/virtual_geometry_transcoder.h"

#include <atomic>

namespace RendererRD {

// MeshStorage owns this service. All mutations run on the rendering thread.
// Feedback borrows only a mutex-protected value snapshot, never live GPU state.
class VirtualGeometryStorage {
	struct Asset;
	struct Page {
		enum State { QUEUED,
			TRANSCODING,
			TRANSCODED,
			RETIRING } state = QUEUED;
		Asset *asset = nullptr;
		uint32_t index = 0;
		uint64_t lease = 0;
		uint64_t upload = 0;
		uint64_t serial = 0;
		uint64_t work = 0;
		uint32_t slot = UINT32_MAX;
		uint32_t units = 0;
		RID source;
		Vector<uint8_t> bytes;
		VirtualGeometry::HierarchyPage fixups;
		bool canceled = false;
		bool retire_outcome = false;
	};
	struct Asset {
		RID mesh;
		uint64_t generation = 0;
		uint64_t metadata = 0;
		bool destroyed = false;
		VirtualGeometry::GeometryModelRoot root;
		TypedArray<RID> materials;
		HashMap<uint32_t, Page *> pages;
	};
	struct Outcome {
		uint64_t sequence;
		uint64_t mesh;
		uint64_t generation;
		uint64_t lease;
		uint64_t upload;
		uint32_t page;
		uint32_t status;
		Error error;
		uint64_t completed;
	};
	struct FreedBuffer {
		uint64_t serial;
		uint64_t bytes;
	};
	static constexpr uint32_t UNIT_BYTES = VirtualGeometry::ROOT_PAGE_GPU_CAPACITY;
	static constexpr uint32_t MAX_OUTCOMES = 4096;
	static constexpr uint32_t REQUEST_BYTES = 44;
	static constexpr uint32_t OUTCOME_BYTES = 60;
	static constexpr uint32_t SNAPSHOT_BYTES = 80;

	HashMap<RID, Asset *> assets;
	HashSet<Asset *> detached;
	LocalVector<Page *> queued;
	LocalVector<Page *> inflight;
	LocalVector<Page *> retiring;
	LocalVector<FreedBuffer> freed_buffers;
	LocalVector<Outcome> outcomes;
	LocalVector<uint8_t> slots;
	VirtualGeometryTranscoder transcoder;
	RID pool;
	uint64_t pool_bytes = 0;
	uint64_t metadata_budget = 0;
	uint64_t work_budget = 0;
	uint64_t metadata_reserved = 0;
	uint64_t work_reserved = 0;
	uint64_t occupied_bytes = 0;
	uint64_t retiring_bytes = 0;
	uint64_t staging_bytes = 0;
	uint64_t sequence = 0;
	uint64_t updated_serial = 0;
	uint32_t outcome_reservations = 0;
	mutable Mutex feedback_mutex;
	Vector<uint8_t> snapshot;
	Vector<uint8_t> feedback_spare;
	mutable std::atomic<uint64_t> acknowledged{ 0 };

	void _outcome(Page *p_page, uint32_t p_status, Error p_error);
	void _publish_feedback();
	void _prune_outcomes();
	void _retire(Page *p_page);
	void _delete_page(Page *p_page);
	uint32_t _allocate(uint32_t p_units);
	void _free_source(Page *p_page);

public:
	~VirtualGeometryStorage();
	bool has(RID p_mesh) const { return assets.has(p_mesh); }
	Error set_budget(uint64_t p_pool_bytes, uint64_t p_metadata_bytes, uint64_t p_work_bytes);
	Error register_mesh(RID p_mesh, uint64_t p_generation, const Vector<uint8_t> &p_root, const TypedArray<RID> &p_materials);
	void release_mesh(RID p_mesh);
	// Whole-batch refusal retains caller ownership. Accepted input uses COW owned
	// bytes; transcode outcomes are keyed by lease/upload generation. They do
	// not acknowledge hierarchy publication or a host's complete install lease.
	Error submit_pages(const Vector<uint8_t> &p_records, const TypedArray<PackedByteArray> &p_payloads);
	Vector<uint8_t> get_feedback(uint64_t p_after_sequence) const;
	void update();
};

} // namespace RendererRD

#endif // VIRTUAL_GEOMETRY_STORAGE_RD_H
