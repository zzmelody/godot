#ifndef VIRTUAL_GEOMETRY_STORAGE_RD_H
#define VIRTUAL_GEOMETRY_STORAGE_RD_H

#include "core/io/virtual_geometry/runtime_fixups.h"
#include "core/math/aabb.h"
#include "core/os/mutex.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/self_list.h"
#include "core/templates/span.h"
#include "core/variant/typed_array.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_fixup.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_requests.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_diagnostics.glsl.gen.h"
#include "servers/rendering/renderer_rd/virtual_geometry_transcoder.h"
#include "servers/rendering/storage/mesh_storage.h"

#include <atomic>

namespace RendererRD {

// MeshStorage owns this service. All mutations run on the rendering thread.
// Feedback borrows only a mutex-protected value snapshot, never live GPU state.
class VirtualGeometryStorage {
public:
	struct Bindings {
		RID metadata;
		RID geometry;
		uint32_t page_table_word = 0;
		uint64_t generation = 0;
		uint32_t parts_word = 0;
		uint32_t groups_word = 0;
		uint32_t nodes = 0;
		uint32_t hierarchy_depth = 0;
		uint32_t clusters = 0;
		uint32_t root_pages = 0;
		uint32_t root_clusters = 0;
		uint32_t pages = 0;
	};

private:
	struct Asset;
	struct Page {
		enum State { QUEUED,
			TRANSCODING,
			TRANSCODED,
			INSTALL_READY,
			PUBLISHING,
			RESIDENT,
			UNINSTALLING,
			RETIRING } state = QUEUED;
		Asset *asset = nullptr;
		uint32_t index = 0;
		uint64_t lease = 0;
		uint64_t upload = 0;
		uint64_t serial = 0;
		uint64_t work = 0;
		uint32_t slot = UINT32_MAX;
		uint32_t units = 0;
		uint32_t root_clusters = 0;
		RID source;
		Vector<uint8_t> bytes;
		VirtualGeometry::HierarchyPage fixups;
		bool canceled = false;
		bool retire_outcome = false;
		bool install_pending = true;
		bool dependencies_held = false;
		bool waiting_roots = false;
		bool root_counted = false;
		uint32_t unresolved = 0;
		Error install_error = ERR_SKIP;
	};
	struct Asset {
		SelfList<Asset> request_waiter;
		uint64_t request_seen_epoch = 0;
		Asset() :
				request_waiter(this) {}
		RID mesh;
		uint64_t generation = 0;
		uint64_t metadata = 0;
		uint32_t gpu_bytes = 0;
		uint32_t roots_transcoded = 0;
		uint32_t roots_published = 0;
		bool roots_committed = false;
		bool destroyed = false;
		Bindings bindings;
		VirtualGeometry::GeometryModelRoot root;
		Vector<RID> materials;
		HashMap<uint32_t, Page *> pages;
		std::vector<uint8_t> committed;
		std::vector<uint32_t> physical_slots;
		std::vector<uint32_t> dependency_users;
		HashMap<uint32_t, LocalVector<Page *>> waiters;
		LocalVector<Page *> root_waiters;
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
	// The callback holds this value owner alive. It never borrows an Asset or
	// the storage service, so a canceled mesh/view cannot become a dangling call.
	class RequestReadback : public RefCounted {
	public:
		Vector<uint8_t> bytes;
		bool ready = false;
		void complete(const Vector<uint8_t> &p_bytes, const Ref<RefCounted> &p_keep_alive) {
			bytes = p_bytes;
			ready = true;
		}
	};
	struct RequestSlot {
		RID buffer;
		uint64_t charge = 0;
		RID mesh;
		uint64_t generation = 0;
		Ref<RequestReadback> readback;
	};
	struct RequestRecord {
		uint64_t sequence;
		uint64_t mesh;
		uint64_t generation;
		uint32_t page;
		uint32_t priority;
	};
	struct FreedBuffer {
		uint64_t serial;
		uint64_t bytes;
		bool metadata = false;
		bool staging = true;
	};
	struct PatchBatch {
		Page *page = nullptr;
		Vector<uint8_t> commands;
		RID source;
		RID uniforms;
	};
	static constexpr uint32_t UNIT_BYTES = VirtualGeometry::ROOT_PAGE_GPU_CAPACITY;
	static constexpr uint32_t MAX_OUTCOMES = 4096;
	static constexpr uint32_t REQUEST_BYTES = 44;
	static constexpr uint32_t OUTCOME_BYTES = 60;
	static constexpr uint32_t SNAPSHOT_BYTES = 88;
	static constexpr uint32_t MAX_REQUESTS = 1024;
	static constexpr uint32_t REQUEST_SLOTS = 4;
	static constexpr uint32_t REQUEST_CAPACITY = 128;
	static constexpr uint32_t REQUEST_BUFFER_BYTES = 16 + REQUEST_CAPACITY * 8;
	static constexpr uint32_t REQUEST_RECORD_BYTES = 32;
	static constexpr uint32_t REQUEST_HEADER_BYTES = 32;
	enum PageResult { PAGE_RESIDENT,
		PAGE_RETIRED,
		PAGE_FAILED,
		PAGE_RETIRE_FAILED };

	HashMap<RID, Asset *> assets;
	HashSet<Asset *> detached;
	LocalVector<Page *> queued;
	LocalVector<Page *> inflight;
	LocalVector<Page *> ready;
	LocalVector<Page *> publishing;
	LocalVector<Page *> uninstalls;
	LocalVector<Page *> retiring;
	LocalVector<FreedBuffer> freed_buffers;
	LocalVector<Outcome> outcomes;
	LocalVector<uint8_t> slots;
	VirtualGeometryTranscoder transcoder;
	VirtualGeometryFixupShaderRD fixup_shader;
	RID fixup_version;
	PipelineDeferredRD fixup_pipeline;
	RID pool;
	VirtualGeometryRequestsShaderRD request_shader;
	RID request_version;
	PipelineDeferredRD request_pipeline;
	RequestSlot request_slots[REQUEST_SLOTS];
	VirtualGeometryDiagnosticsShaderRD diagnostics_shader;
	RID diagnostics_version;
	PipelineDeferredRD diagnostics_pipeline;
	RID diagnostics_buffer;
	Ref<RequestReadback> diagnostics_readback;
	uint32_t reported_diagnostics[2] = {};
	uint64_t diagnostics_serial = 0;
	SelfList<Asset>::List request_waiters;
	uint64_t request_epoch = 0;
	LocalVector<RequestRecord> requests;
	Vector<uint8_t> request_snapshot, request_spare;
	mutable std::atomic<uint64_t> requests_acknowledged{ 0 };
	uint64_t request_sequence = 0, request_overflow = 0, request_invalid = 0;
	uint64_t pool_bytes = 0;
	uint64_t metadata_budget = 0;
	uint64_t work_budget = 0;
	uint64_t metadata_reserved = 0;
	uint64_t work_reserved = 0;
	uint64_t occupied_bytes = 0;
	uint64_t retiring_bytes = 0;
	uint64_t staging_bytes = 0;
	uint64_t gpu_metadata_bytes = 0;
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
	static Span<uint32_t> _dependencies(const Page *p_page);
	static void _remove_page_reference(LocalVector<Page *> &p_list, Page *p_page);
	void _begin_retirement(Page *p_page);
	void _drop_dependencies(Page *p_page);
	void _remove_waiting(Page *p_page);
	void _queue_ready(Page *p_page);
	void _notify_committed(Page *p_page);
	void _process_installs();
	void _process_uninstalls();
	Error _make_patch_batch(Page *p_page, bool p_uninstall, PatchBatch &r_batch);
	Error _record_fixup_batch(LocalVector<PatchBatch> &p_batches);
	void _free_metadata(Asset *p_asset);
	void _delete_page(Page *p_page);
	uint32_t _allocate(uint32_t p_units);
	void _free_source(Page *p_page);
	void _consume_requests();
	void _publish_requests();

public:
	~VirtualGeometryStorage();
	bool has(RID p_mesh) const { return assets.has(p_mesh); }
	// Render-thread borrow, valid only while the Mesh RID remains owned. A null
	// result means the complete root set has not finished GPU publication.
	const Bindings *get_bindings(RID p_mesh) const;
	RendererMeshStorage::VirtualGeometryMaterials get_materials(RID p_mesh) const;
	Error set_material(RID p_mesh, uint32_t p_slot, RID p_material);
	Error set_budget(uint64_t p_pool_bytes, uint64_t p_metadata_bytes, uint64_t p_work_bytes);
	bool reserve_view_work(uint64_t p_bytes);
	void retire_view_work(uint64_t p_bytes);
	Error register_mesh(RID p_mesh, uint64_t p_generation, const Vector<uint8_t> &p_root, const TypedArray<RID> &p_materials, AABB &r_bounds);
	void release_mesh(RID p_mesh);
	// Whole-batch refusal retains caller ownership. Accepted input uses COW owned
	// bytes. Resident outcomes follow GPU fixup publication; slots cannot retire
	// while an uncanceled finer page still owns a dependency on them.
	Error submit_pages(const Vector<uint8_t> &p_records, const TypedArray<PackedByteArray> &p_payloads);
	Vector<uint8_t> get_feedback(uint64_t p_after_sequence) const;
	// Bounded asynchronous feedback, separate from install/retirement outcomes.
	// A saturated feedback queue delays detail; it never removes root coverage.
	Error record_requests(RID p_mesh, uint64_t p_generation, RID p_work, uint32_t p_word, uint32_t p_capacity);
	// Aggregate every view before asynchronous readback. No view owner or scene
	// object is retained, and saturation cannot erase another view's error bits.
	Error record_diagnostics(RID p_view_status, RID p_material_status);
	Vector<uint8_t> get_requests(uint64_t p_after_sequence) const;
	void update();
};

} // namespace RendererRD

#endif // VIRTUAL_GEOMETRY_STORAGE_RD_H
