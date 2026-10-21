#ifndef VIRTUAL_GEOMETRY_STREAMING_H
#define VIRTUAL_GEOMETRY_STREAMING_H

#include "core/io/virtual_geometry/model_codec.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "virtual_geometry_demand_queue.h"

#include <list>
#include <vector>

// Main-thread host for publication-admitted roots. Workers borrow only their
// owned file-range job. Rendering storage remains the GPU state authority.
class VirtualGeometryStreaming {
	struct Key {
		RID mesh;
		uint64_t generation = 0;
		uint32_t page = 0;
	};
	using DemandQueue = VirtualGeometry::PageDemandQueue<Key>;
	using Demand = DemandQueue::Demand;
	struct Page {
		enum State { ABSENT,
			QUEUED,
			READING,
			UPLOADING,
			RESIDENT,
			RETIRING,
			FAILED } state = ABSENT;
		uint32_t users = 0;
		uint64_t lease = 0;
		uint64_t upload = 0;
		bool holds_dependencies = false;
		bool slot_reserved = false;
		bool listed = false;
		std::list<Key>::iterator position;
		DemandQueue::Handle queued_demand;
	};
	struct Asset {
		RID mesh;
		uint64_t generation = 0;
		String container;
		VirtualGeometry::PageDirectory directory;
		std::vector<Page> pages;
		uint32_t roots = 0;
		uint32_t roots_ready = 0;
		uint32_t pending = 0;
		uint64_t metadata_lease = 0;
		Error error = OK;
		bool active = true;
	};
	struct Read {
		Key key;
		Asset *asset = nullptr;
		String path;
		uint64_t offset = 0;
		uint64_t container_bytes = 0;
		uint32_t length = 0;
		uint32_t decoded_length = 0;
		uint64_t admission = 0;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		PackedByteArray bytes;
		Error error = OK;
	};
	struct Transfer {
		Asset *asset = nullptr;
		uint32_t page = 0;
		uint64_t upload = 0;
		uint64_t admission = 0;
		bool retiring = false;
	};
	static VirtualGeometryStreaming *singleton;
	static constexpr uint32_t MAX_READS = 16;
	static constexpr uint32_t MAX_SUBMISSIONS = 16;
	HashMap<RID, Asset *> assets;
	HashMap<uint64_t, Transfer> transfers;
	DemandQueue demands;
	std::list<Key> residents;
	std::vector<Read *> reads;
	std::vector<Read *> ready;
	Callable acquire, received, release;
	// Zero selects automatic pool growth and unrestricted aggregate accounting.
	uint64_t pool_bytes = 0;
	uint64_t metadata_bytes = 0;
	uint64_t work_bytes = 0;
	uint64_t generation = 0, transaction = 0;
	uint64_t requests_after = 0, outcomes_after = 0;
	uint64_t pool_admission = 0;
	uint64_t refused_reads = 0, failed_reads = 0, evictions = 0;
	uint64_t busy_submissions = 0, capacity_refusals = 0;
	uint64_t occupied = 0, retiring = 0, reserved_slots = 0;
	uint64_t physical_bytes = 0, metadata_reserved = 0, work_reserved = 0;
	uint64_t pinned_roots = 0;
	bool configured = false;
	bool stopping = false;
	bool waiting_for_read = false;
	Mutex removals_mutex;
	Vector<RID> deferred_removals;

	uint64_t _acquire(uint32_t p_kind, uint64_t p_owner, const String &p_key, uint64_t p_bytes);
	void _received(uint64_t p_token);
	void _release(uint64_t p_token);
	void _withdraw_read_waiter();
	static void _read(void *p_read);
	Asset *_find(const Key &p_key) const;
	void _drop_dependencies(Asset *p_asset, uint32_t p_page);
	bool _demand(Asset *p_asset, uint32_t p_page, float p_priority, uint32_t p_depth = 0);
	void _touch(Asset *p_asset, uint32_t p_page);
	bool _evict();
	void _outcomes();
	void _requests();
	void _receive();
	void _submit();
	void _start_reads();
	void _delete_if_finished(Asset *p_asset);
	void _fail_roots(Asset *p_asset, Error p_error);
	Error _configure();

public:
	VirtualGeometryStreaming();
	~VirtualGeometryStreaming();
	static VirtualGeometryStreaming *get_singleton() { return singleton; }
	// Kind 0 borrows a read slot; kind 1 reserves persistent capacity; kind 2
	// withdraws the process reader's unadmitted FIFO turn and returns zero. A zero
	// result defers admission. Received frees only the read slot. Release follows
	// the GPU outcome, or worker completion when a read is canceled/refused.
	Error set_admission(const Callable &p_acquire, const Callable &p_received, const Callable &p_release);
	Error initialize() { return _configure(); }
	Error install(const PackedByteArray &p_root, const VirtualGeometry::GeometryModelRoot &p_model,
			const TypedArray<RID> &p_materials, const String &p_container, RID &r_mesh);
	void remove(RID p_mesh);
	Error get_status(RID p_mesh, bool &r_ready) const;
	Dictionary get_statistics() const;
	void update();
	// Stop joins owned file jobs and cancels mesh owners. Capacity admission is
	// retained through process shutdown, including device/physical-pool teardown.
	void stop();
};

#endif // VIRTUAL_GEOMETRY_STREAMING_H
