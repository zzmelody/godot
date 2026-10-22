#include "virtual_geometry_mesh.h"
#include "virtual_geometry/model_decoder.h"
#include "content_hash.h"

#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/packed_scene.h"

#include "asset_files.h"
#include "atomic_save.h"
#include "virtual_geometry/model_builder.h"

#include "core/crypto/crypto_core.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/version.h"
#include "scene/resources/surface_tool.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace CookerVirtualGeometry {
namespace {
namespace vg = VirtualGeometry;

void run_dag_groups(void *p_context, uint32_t p_count, void (*p_work)(void *, uint32_t)) {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (pool == nullptr || pool->get_thread_count() <= 1 || p_count > INT32_MAX) {
		for (uint32_t index = 0; index < p_count; ++index) {
			p_work(p_context, index);
		}
		return;
	}
	const auto task = pool->add_native_group_task(p_work, p_context, int(p_count), MIN(8, pool->get_thread_count()), false, "Build virtual geometry groups");
	pool->wait_for_group_task_completion(task);
}

Error refuse(String &r_message, const String &p_message, Error p_error = ERR_INVALID_DATA) {
	r_message = p_message;
	return p_error;
}

// Feed explicit little-endian scalar bits, never native struct padding. Small
// writes are batched so hashing the input does not call SHA once per scalar.
class BuildHash {
	CryptoCore::SHA256Context context;
	std::array<uint8_t, 4096> bytes;
	size_t used = 0;
	Error error = OK;

	void flush() {
		if (error == OK && used) {
			error = context.update(bytes.data(), used);
		}
		used = 0;
	}

public:
	BuildHash() { error = context.start(); }
	void word(uint32_t p_value) {
		if (used + 4 > bytes.size()) {
			flush();
		}
		for (int byte = 0; byte < 4; ++byte) {
			bytes[used++] = uint8_t(p_value >> (byte * 8));
		}
	}
	void scalar(float p_value) {
		uint32_t bits;
		std::memcpy(&bits, &p_value, sizeof(bits));
		word(bits);
	}
	void scalar(double p_value) {
		uint64_t bits;
		std::memcpy(&bits, &p_value, sizeof(bits));
		word(uint32_t(bits));
		word(uint32_t(bits >> 32));
	}
	void text(const char *p_value) {
		flush();
		if (error == OK) {
			error = context.update(reinterpret_cast<const uint8_t *>(p_value), std::strlen(p_value) + 1);
		}
	}
	Error finish(std::array<uint8_t, 32> &r_key) {
		flush();
		return error == OK ? context.finish(r_key.data()) : error;
	}
};

String hex(const uint8_t *p_bytes, size_t p_count) {
	static constexpr char DIGITS[] = "0123456789abcdef";
	String result;
	for (size_t index = 0; index < p_count; ++index) {
		result += DIGITS[p_bytes[index] >> 4];
		result += DIGITS[p_bytes[index] & 15];
	}
	return result;
}

Error extract(const Ref<Mesh> &p_source, const Settings &p_settings, vg::GeometryBuildInput &r_input, uint32_t &r_source_vertices, String &r_message) {
	const int surfaces = p_source->get_surface_count();
	if (surfaces <= 0 || surfaces > 4096 || p_source->get_blend_shape_count() != 0) {
		return refuse(r_message, "virtual_geometry requires static triangle surfaces without blend shapes");
	}
	std::vector<Array> surface_arrays;
	surface_arrays.reserve(surfaces);
	vg::AttributeLayout layout;
	layout.interpolate_uvs = p_settings.interpolate_uvs;
	uint64_t vertices = 0;
	uint64_t triangles = 0;
	for (int surface = 0; surface < surfaces; ++surface) {
		if (p_source->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			return refuse(r_message, vformat("virtual_geometry surface %d is not triangles", surface));
		}
		Array arrays = p_source->surface_get_arrays(surface);
		if (arrays.size() != Mesh::ARRAY_MAX || arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY) {
			return refuse(r_message, vformat("virtual_geometry surface %d has no vertex array", surface));
		}
		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		vertices += positions.size();
		if (positions.size() < 3 || vertices > p_settings.max_source_vertices) {
			return refuse(r_message, vformat("virtual_geometry source vertex budget exceeded: %d > %d", vertices, p_settings.max_source_vertices), ERR_OUT_OF_MEMORY);
		}
		for (int slot : { Mesh::ARRAY_BONES, Mesh::ARRAY_WEIGHTS, Mesh::ARRAY_CUSTOM0, Mesh::ARRAY_CUSTOM1, Mesh::ARRAY_CUSTOM2, Mesh::ARRAY_CUSTOM3 }) {
			if (arrays[slot].get_type() != Variant::NIL && !arrays[slot].is_zero()) {
				return refuse(r_message, vformat("virtual_geometry surface %d has unsupported skin or custom attributes", surface));
			}
		}
		const auto valid = [&arrays, &positions](int p_slot, Variant::Type p_type, int p_components = 1) {
			if (arrays[p_slot].get_type() == Variant::NIL) {
				return true;
			}
			if (arrays[p_slot].get_type() != p_type) {
				return false;
			}
			int64_t count = 0;
			switch (p_type) {
				case Variant::PACKED_VECTOR3_ARRAY:
					count = PackedVector3Array(arrays[p_slot]).size();
					break;
				case Variant::PACKED_VECTOR2_ARRAY:
					count = PackedVector2Array(arrays[p_slot]).size();
					break;
				case Variant::PACKED_COLOR_ARRAY:
					count = PackedColorArray(arrays[p_slot]).size();
					break;
				case Variant::PACKED_FLOAT32_ARRAY:
					count = PackedFloat32Array(arrays[p_slot]).size();
					break;
				default:
					return false;
			}
			return count == 0 || count == int64_t(positions.size()) * p_components;
		};
		if (!valid(Mesh::ARRAY_NORMAL, Variant::PACKED_VECTOR3_ARRAY) ||
				!valid(Mesh::ARRAY_TANGENT, Variant::PACKED_FLOAT32_ARRAY, 4) ||
				!valid(Mesh::ARRAY_COLOR, Variant::PACKED_COLOR_ARRAY) ||
				!valid(Mesh::ARRAY_TEX_UV, Variant::PACKED_VECTOR2_ARRAY) ||
				!valid(Mesh::ARRAY_TEX_UV2, Variant::PACKED_VECTOR2_ARRAY) ||
				(arrays[Mesh::ARRAY_INDEX].get_type() != Variant::NIL && arrays[Mesh::ARRAY_INDEX].get_type() != Variant::PACKED_INT32_ARRAY)) {
			return refuse(r_message, vformat("virtual_geometry surface %d has invalid attribute lengths or types", surface));
		}
		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const int64_t corners = indices.is_empty() ? positions.size() : indices.size();
		triangles += corners / 3;
		if (corners < 3 || corners % 3 || triangles > uint64_t(p_settings.max_source_vertices) * 4) {
			return refuse(r_message, "virtual_geometry source triangle count is invalid or exceeds its budget");
		}
		for (int32_t index : indices) {
			if (index < 0 || index >= positions.size()) {
				return refuse(r_message, vformat("virtual_geometry surface %d has an invalid index", surface));
			}
		}
		layout.has_tangents |= !PackedFloat32Array(arrays[Mesh::ARRAY_TANGENT]).is_empty();
		layout.has_colors |= !PackedColorArray(arrays[Mesh::ARRAY_COLOR]).is_empty();
		if (!PackedVector2Array(arrays[Mesh::ARRAY_TEX_UV2]).is_empty()) {
			layout.uv_sets = 2;
		} else if (!PackedVector2Array(arrays[Mesh::ARRAY_TEX_UV]).is_empty()) {
			layout.uv_sets = MAX(layout.uv_sets, 1u);
		}
		surface_arrays.push_back(arrays);
	}
	r_input.layout = layout;
	for (int surface = 0; surface < surfaces; ++surface) {
		Array arrays = surface_arrays[surface];
		const bool needs_normals = PackedVector3Array(arrays[Mesh::ARRAY_NORMAL]).is_empty();
		const bool needs_tangents = layout.has_tangents && PackedFloat32Array(arrays[Mesh::ARRAY_TANGENT]).is_empty() &&
				!PackedVector2Array(arrays[Mesh::ARRAY_TEX_UV]).is_empty();
		if (needs_normals || needs_tangents) {
			Ref<SurfaceTool> tool;
			tool.instantiate();
			tool->create_from(p_source, surface);
			if (needs_normals) {
				tool->generate_normals();
			}
			if (needs_tangents) {
				tool->generate_tangents();
			}
			arrays = tool->commit_to_arrays();
		}
		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
		const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
		const PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
		const PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
		const PackedVector2Array uv2s = arrays[Mesh::ARRAY_TEX_UV2];
		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		if (positions.size() != normals.size()) {
			return refuse(r_message, "virtual_geometry normal preparation failed");
		}
		std::vector<uint32_t> remap(positions.size(), UINT32_MAX);
		const int64_t corners = indices.is_empty() ? positions.size() : indices.size();
		for (int64_t corner = 0; corner < corners; ++corner) {
			const uint32_t source_vertex = indices.is_empty() ? uint32_t(corner) : uint32_t(indices[corner]);
			if (remap[source_vertex] == UINT32_MAX) {
				if (r_input.mesh.positions.size() >= p_settings.max_source_vertices) {
					return refuse(r_message, "virtual_geometry prepared vertex budget exceeded", ERR_OUT_OF_MEMORY);
				}
				const Vector3 position = positions[source_vertex];
				const Vector3 normal = normals[source_vertex];
				if (!position.is_finite() || !normal.is_finite() || normal.length_squared() <= 0) {
					return refuse(r_message, "virtual_geometry positions or normals are non-finite or degenerate");
				}
				remap[source_vertex] = uint32_t(r_input.mesh.positions.size());
				r_input.mesh.positions.push_back({ float(position.x), float(position.y), float(position.z) });
				auto &attributes = r_input.attributes;
				for (int axis = 0; axis < 3; ++axis) {
					attributes.push_back(float(normal[axis]));
				}
				if (layout.has_tangents) {
					if (tangents.is_empty()) {
						// No authored UV/tangent frame on this surface. Supply an
						// orthogonal frame for the common model attribute layout.
						const Vector3 axis = std::abs(normal.x) < 0.8 ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
						const Vector3 tangent = normal.cross(axis).normalized();
						for (int component = 0; component < 3; ++component) {
							attributes.push_back(float(tangent[component]));
						}
						attributes.push_back(1);
					} else {
						for (int component = 0; component < 4; ++component) {
							attributes.push_back(tangents[source_vertex * 4 + component]);
						}
					}
				}
				if (layout.has_colors) {
					const Color color = colors.is_empty() ? Color(1, 1, 1, 1) : colors[source_vertex];
					for (int component = 0; component < 4; ++component) {
						attributes.push_back(color[component]);
					}
				}
				for (uint32_t set = 0; set < layout.uv_sets; ++set) {
					const PackedVector2Array &values = set == 0 ? uvs : uv2s;
					const Vector2 uv = values.is_empty() ? Vector2() : values[source_vertex];
					attributes.push_back(float(uv.x));
					attributes.push_back(float(uv.y));
				}
			}
			r_input.mesh.indices.push_back(remap[source_vertex]);
			if (corner % 3 == 0) {
				r_input.mesh.materials.push_back(uint32_t(surface));
			}
		}
	}
	r_source_vertices = uint32_t(vertices);
	return OK;
}
} // namespace

Error parse_import_settings(const Dictionary &p_options, bool p_runtime_scene,
		Settings &r_settings, bool &r_explicit_geometry) {
	for (const String &key : { String("virtual_geometry"), String("preserve_area") }) {
		ERR_FAIL_COND_V_MSG(p_options.has(key) && (!p_runtime_scene || p_options[key].get_type() != Variant::BOOL),
				ERR_INVALID_PARAMETER, "Virtual geometry import options require a PackedScene and boolean values: " + key);
	}
	const bool explicit_geometry = p_options.get("virtual_geometry", false);
	ERR_FAIL_COND_V_MSG(p_options.has("preserve_area") && !explicit_geometry, ERR_INVALID_PARAMETER,
			"preserve_area requires virtual_geometry=true.");
	r_settings.dag.preserve_area = p_options.get("preserve_area", false);
	r_explicit_geometry = explicit_geometry;
	return OK;
}

Error build(const Ref<Mesh> &p_source, const Settings &p_settings, BuildOutput &r_output, String &r_message) {
	if (p_source.is_null() || Ref<VirtualGeometryMesh>(p_source).is_valid()) {
		return refuse(r_message, "virtual_geometry requires an ordinary source Mesh");
	}
	try {
		vg::GeometryBuildInput input;
		uint32_t source_vertices = 0;
		Error error = extract(p_source, p_settings, input, source_vertices, r_message);
		if (error != OK) {
			return error;
		}
		BuildHash hash;
		hash.text("Veya virtual geometry mesh builder 1");
		hash.text(GODOT_VERSION_HASH);
		hash.word(vg::MODEL_FORMAT_VERSION);
		hash.word(vg::GEOMETRY_PAGE_VERSION);
		hash.word(uint32_t(input.mesh.positions.size()));
		hash.word(uint32_t(input.mesh.indices.size()));
		hash.word(input.layout.has_tangents);
		hash.word(input.layout.has_colors);
		hash.word(input.layout.uv_sets);
		hash.word(input.layout.interpolate_uvs);
		hash.word(p_settings.dag.preserve_area);
		hash.scalar(p_settings.dag.max_edge_length_factor);
		hash.word(uint32_t(p_settings.encoding.position_precision));
		hash.word(p_settings.encoding.normal_bits);
		hash.word(p_settings.encoding.tangent_bits);
		for (const auto &position : input.mesh.positions) {
			for (float value : position) {
				hash.scalar(value);
			}
		}
		for (uint32_t index : input.mesh.indices) {
			hash.word(index);
		}
		for (uint32_t material : input.mesh.materials) {
			hash.word(material);
		}
		for (float value : input.attributes) {
			hash.scalar(value);
		}
		std::array<uint8_t, 32> build_key;
		if (hash.finish(build_key) != OK) {
			return refuse(r_message, "virtual_geometry could not hash canonical build inputs", FAILED);
		}
		vg::GeometryDag dag;
		const uint64_t dag_started = OS::get_singleton()->get_ticks_usec();
		const auto dag_result = vg::build_cluster_dag(input, p_settings.dag, dag, &run_dag_groups);
		const uint64_t dag_build_usec = OS::get_singleton()->get_ticks_usec() - dag_started;
		if (dag_result.error != vg::DagBuildError::OK) {
			return refuse(r_message, vformat("virtual_geometry DAG refused: error=%d group=%d leaf=%d topology=%d partition=%d simplification=%d source_triangles=%d", int(dag_result.error), dag_result.group, int(dag_result.leaf_result.error), int(dag_result.leaf_result.topology_error), int(dag_result.leaf_result.partition_result.error), int(dag_result.simplification_result.error), input.mesh.indices.size() / 3));
		}
		vg::EncodableGeometry geometry;
		const auto encoded = vg::prepare_cluster_encoding(dag, p_settings.encoding, geometry);
		if (encoded.error != vg::ClusterEncodingError::OK) {
			return refuse(r_message, vformat("virtual_geometry encoding refused: error=%d cluster=%d", int(encoded.error), encoded.cluster));
		}
		vg::GeometryPageSet pages;
		const auto paged = vg::build_geometry_pages(geometry, p_settings.max_output_bytes, pages);
		if (paged.error != vg::PageBuildError::OK) {
			return refuse(r_message, vformat("virtual_geometry pages refused: error=%d cluster=%d output_limit=%d source_triangles=%d leaf_clusters=%d encoded_clusters=%d root_clusters=%d", int(paged.error), paged.cluster, p_settings.max_output_bytes, input.mesh.indices.size() / 3, dag.leaf_count, geometry.clusters.size(), geometry.root_clusters.size()));
		}
		vg::GeometryHierarchy hierarchy;
		const auto hierarchical = vg::build_geometry_hierarchy(geometry, pages, hierarchy);
		if (hierarchical.error != vg::HierarchyBuildError::OK) {
			return refuse(r_message, vformat("virtual_geometry hierarchy refused: error=%d part=%d", int(hierarchical.error), hierarchical.part));
		}
		vg::GeometryModelBundle bundle;
		const auto packed = vg::pack_geometry_model(geometry, pages, hierarchy, build_key, p_settings.max_output_bytes, bundle);
		if (packed.error != vg::ModelBuildError::OK) {
			return refuse(r_message, vformat("virtual_geometry model refused: error=%d page=%d codec=%d page_codec=%d cluster=%d", int(packed.error), packed.page, int(packed.codec_result.error), int(packed.page_result.error), packed.page_result.cluster));
		}
		CryptoCore::SHA256Context container_hash;
		std::array<uint8_t, 32> digest;
		if (container_hash.start() != OK || container_hash.update(bundle.container.data(), bundle.container.size()) != OK || container_hash.finish(digest.data()) != OK) {
			return refuse(r_message, "virtual_geometry could not hash the encoded container", FAILED);
		}
		PackedByteArray root;
		if (root.resize(bundle.root.size()) != OK) {
			return refuse(r_message, "virtual_geometry root allocation failed", ERR_OUT_OF_MEMORY);
		}
		std::memcpy(root.ptrw(), bundle.root.data(), bundle.root.size());
		Array materials;
		vg::GeometryModelRoot metadata;
		if (vg::decode_geometry_model_root(bundle.root.data(), bundle.root.size(), bundle.container.size(), 256 * 1024 * 1024, metadata).error != vg::ModelCodecError::OK) {
			return refuse(r_message, "virtual_geometry generated an invalid model root");
		}
		uint64_t gpu_payload_bytes = 0;
		uint64_t gpu_root_payload_bytes = 0;
		vg::GeometryModelPageDecoder decoder;
		if (decoder.initialize(metadata, metadata.directory.pages.size()).error != vg::ModelCodecError::OK) {
			return refuse(r_message, "virtual_geometry generated an invalid page dependency graph");
		}
		for (uint32_t page : decoder.get_order()) {
			const auto &location = metadata.directory.pages[page];
			const uint8_t *bytes = bundle.container.data() + location.container_offset;
			const vg::GeometryPage *decoded_page = nullptr;
			vg::HierarchyPage decoded_fixups;
			std::vector<uint8_t> gpu;
			const auto checked = decoder.decode(page, bytes, location.disk_bytes, decoded_page, decoded_fixups, &gpu);
			if (checked.error != vg::ModelCodecError::OK) {
				return refuse(r_message, vformat("virtual_geometry generated page %d refused: error=%d index=%d", page, int(checked.error), checked.index));
			}
			// The CPU oracle belongs only to Cooker. Runtime uses GPU transcode.
			gpu_payload_bytes += gpu.size();
			if (page < metadata.hierarchy.root_pages) {
				gpu_root_payload_bytes += gpu.size();
			}
		}
		for (uint32_t material : metadata.materials) {
			materials.push_back(p_source->surface_get_material(material));
		}
		// Built-in material/shader/texture paths can point into the source Mesh
		// file. Detach them in one duplicate session, retaining sharing within
		// the material set and keeping standalone external resources shared.
		materials = materials.duplicate_deep(RESOURCE_DEEP_DUPLICATE_INTERNAL);
		Dictionary data;
		data["root"] = root;
		data["container"] = "geometry--" + hex(digest.data(), digest.size()) + ".vgp";
		data["container_bytes"] = int64_t(bundle.container.size());
		data["materials"] = materials;
		BuildOutput candidate;
		candidate.mesh.instantiate();
		if (candidate.mesh->set_geometry_data(data) != OK) {
			return refuse(r_message, "virtual_geometry generated invalid native resource metadata");
		}
		candidate.source_vertices = source_vertices;
		candidate.report["dag_build_usec"] = int64_t(dag_build_usec);
		const WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
		candidate.report["dag_group_worker_limit"] = pool != nullptr ? MAX(1, MIN(8, pool->get_thread_count())) : 1;
		candidate.report["source_vertices"] = candidate.source_vertices;
		candidate.report["prepared_vertices"] = int64_t(input.mesh.positions.size());
		candidate.report["source_triangles"] = int64_t(input.mesh.indices.size() / 3);
		candidate.report["source_surfaces"] = p_source->get_surface_count();
		candidate.report["leaf_clusters"] = dag.leaf_count;
		candidate.report["clusters"] = int64_t(geometry.clusters.size());
		candidate.report["groups"] = int64_t(geometry.groups.size());
		candidate.report["retained_boundary_groups"] = dag.retained_boundary_groups;
		candidate.report["pages"] = int64_t(pages.directory.pages.size());
		candidate.report["root_pages"] = hierarchy.root_pages;
		candidate.report["verified_pages"] = int64_t(metadata.directory.pages.size());
		candidate.report["gpu_payload_bytes"] = int64_t(gpu_payload_bytes);
		candidate.report["gpu_root_payload_bytes"] = int64_t(gpu_root_payload_bytes);
		candidate.report["gpu_page_capacity_bytes"] = int64_t(uint64_t(hierarchy.root_pages) * vg::ROOT_PAGE_GPU_CAPACITY +
				(metadata.directory.pages.size() - hierarchy.root_pages) * vg::STREAMING_PAGE_GPU_CAPACITY);
		candidate.report["root_bytes"] = int64_t(bundle.root.size());
		candidate.report["container_bytes"] = int64_t(bundle.container.size());
		candidate.report["build_key"] = hex(build_key.data(), build_key.size());
		candidate.report["builder_revision"] = GODOT_VERSION_HASH;
		candidate.report["position_precision"] = geometry.position_precision;
		candidate.report["max_position_error"] = geometry.max_position_error;
		candidate.report["quantized_degenerate_triangles"] = int64_t(geometry.quantized_degenerate_triangles);
		candidate.report["uv_sets"] = input.layout.uv_sets;
		candidate.report["has_tangents"] = input.layout.has_tangents;
		candidate.report["has_colors"] = input.layout.has_colors;
		candidate.report["render_ready"] = false;
		candidate.container = std::move(bundle.container);
		r_output = std::move(candidate);
		return OK;
	} catch (const std::bad_alloc &) {
		return refuse(r_message, "virtual_geometry build memory allocation failed", ERR_OUT_OF_MEMORY);
	}
}

Error publish_container(const BuildOutput &p_output, const String &p_resource_path,
		uint64_t p_serialized_resource_bytes, uint64_t p_max_output_bytes, Array &r_files) {
	if (p_output.mesh.is_null() || p_serialized_resource_bytes > p_max_output_bytes ||
			p_output.container.size() > p_max_output_bytes - p_serialized_resource_bytes) {
		return ERR_OUT_OF_MEMORY;
	}
	const String path = p_resource_path.get_base_dir().path_join(p_output.mesh->get_container_file());
	Error error = CookerFiles::check_path(path, true);
	if (error != OK || DirAccess::dir_exists_absolute(path)) {
		return error == OK ? ERR_ALREADY_EXISTS : error;
	}
	unsigned char hash[32];
	cooker_hash::Sha256 container_hash;
	if (container_hash.Start() != OK || container_hash.Update(p_output.container.data(), p_output.container.size()) != OK || container_hash.Finish(hash) != OK) {
		return ERR_CANT_CREATE;
	}
	const String expected_hash = String::hex_encode_buffer(hash, sizeof(hash));
	// Page containers are immutable content, even when a stale scene receipt
	// marks its auxiliary files replaceable. Reuse identical bytes rather than
	// replacing a container held by this or another reader's lifetime lease.
	if (FileAccess::exists(path)) {
		if (cooker_hash::HashFile(path) != expected_hash) {
			return ERR_ALREADY_EXISTS;
		}
	} else {
		const String staging = p_resource_path.get_basename() + ".geometry-" + itos(OS::get_singleton()->get_process_id()) + ".partial.vgp";
		CookerAtomicSave::discard_stale(staging);
		Ref<FileAccess> file = FileAccess::open(staging, FileAccess::WRITE, &error);
		if (file.is_null() || error != OK) {
			return error == OK ? ERR_FILE_CANT_WRITE : error;
		}
		file->store_buffer(p_output.container.data(), p_output.container.size());
		file->flush();
		error = file->get_error();
		file.unref();
		if (error == OK) {
			error = CookerAtomicSave::publish(staging, path);
		} else {
			CookerAtomicSave::discard_stale(staging);
		}
		if (error != OK) {
			return error;
		}
	}
	Dictionary entry;
	entry["path"] = path;
	entry["bytes"] = int64_t(p_output.container.size());
	entry["sha256"] = expected_hash;
	r_files.push_back(entry);
	return OK;
}

Error publish_resource_containers(const Ref<Resource> &p_resource, const std::map<ObjectID, BuildOutput> &p_builds,
		const String &p_resource_path, uint64_t p_max_output_bytes, Array &r_files) {
	Vector<Ref<VirtualGeometryMesh>> meshes;
	HashSet<ObjectID> visited;
	auto add_mesh = [&](const Ref<Mesh> &p_mesh) {
		const Ref<VirtualGeometryMesh> mesh = p_mesh;
		if (mesh.is_valid() && !visited.has(mesh->get_instance_id())) {
			visited.insert(mesh->get_instance_id());
			meshes.push_back(mesh);
		}
	};
	add_mesh(p_resource);
	const Ref<MultiMesh> multi = p_resource;
	if (multi.is_valid()) {
		add_mesh(multi->get_mesh());
	}
	const Ref<PackedScene> scene = p_resource;
	if (scene.is_valid()) {
		Node *root = scene->instantiate();
		if (root == nullptr) {
			return ERR_INVALID_DATA;
		}
		Vector<Node *> pending;
		pending.push_back(root);
		while (!pending.is_empty()) {
			Node *node = pending[pending.size() - 1];
			pending.resize(pending.size() - 1);
			for (int child = 0; child < node->get_child_count(); ++child) {
				pending.push_back(node->get_child(child));
			}
			if (const auto *instance = Object::cast_to<MeshInstance3D>(node)) {
				add_mesh(instance->get_mesh());
			}
			if (const auto *instance = Object::cast_to<MultiMeshInstance3D>(node); instance != nullptr && instance->get_multimesh().is_valid()) {
				add_mesh(instance->get_multimesh()->get_mesh());
			}
		}
		memdelete(root);
	}
	HashSet<String> published;
	uint64_t total = 0;
	for (const Ref<VirtualGeometryMesh> &mesh : meshes) {
		if (p_resource.ptr() != mesh.ptr() && !mesh->is_built_in() && !mesh->get_path().is_empty() &&
				CookerFiles::is_generated(mesh->get_path()) && FileAccess::exists(mesh->get_path())) {
			// ResourceSaver keeps this standalone root as an external dependency.
			// Its relative page file belongs to that root, not to this scene.
			continue;
		}
		const String destination = p_resource_path.get_base_dir().path_join(mesh->get_container_file());
		if (published.has(destination)) {
			continue;
		}
		published.insert(destination);
		if (mesh->get_container_bytes() > p_max_output_bytes - total) {
			return ERR_OUT_OF_MEMORY;
		}
		total += mesh->get_container_bytes();
		const auto found = p_builds.find(mesh->get_instance_id());
		if (found != p_builds.end()) {
			const Error error = publish_container(found->second, p_resource_path, 0, p_max_output_bytes, r_files);
			if (error != OK) {
				return error;
			}
			continue;
		}
		const String owner = mesh->get_path().get_slice("::", 0);
		if (owner.is_empty()) {
			return ERR_FILE_BAD_PATH;
		}
		const String source = owner.get_base_dir().path_join(mesh->get_container_file());
		Error error = OK;
		Ref<FileAccess> file = FileAccess::open(source, FileAccess::READ, &error);
		if (file.is_null() || error != OK || file->get_length() != mesh->get_container_bytes()) {
			return error != OK ? error : ERR_FILE_CORRUPT;
		}
		BuildOutput copy;
		copy.mesh = mesh;
		try {
			copy.container.resize(mesh->get_container_bytes());
		} catch (const std::bad_alloc &) {
			return ERR_OUT_OF_MEMORY;
		}
		if (file->get_buffer(copy.container.data(), copy.container.size()) != copy.container.size()) {
			return ERR_FILE_CANT_READ;
		}
		error = publish_container(copy, p_resource_path, 0, p_max_output_bytes, r_files);
		if (error != OK) {
			return error;
		}
	}
	return OK;
}

namespace {
bool automatic_material(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return true;
	}
	const Ref<BaseMaterial3D> material = p_material;
	if (material.is_null()) {
		return false; // Authored shaders use the explicit material/bound contract.
	}
	const auto transparency = material->get_transparency();
	return material->get_next_pass().is_null() &&
			(transparency == BaseMaterial3D::TRANSPARENCY_DISABLED || transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR || transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_HASH) &&
			material->get_alpha_antialiasing() == BaseMaterial3D::ALPHA_ANTIALIASING_OFF &&
			material->get_shading_mode() != BaseMaterial3D::SHADING_MODE_PER_VERTEX && material->get_blend_mode() == BaseMaterial3D::BLEND_MODE_MIX &&
			material->get_depth_draw_mode() != BaseMaterial3D::DEPTH_DRAW_DISABLED && material->get_depth_test() == BaseMaterial3D::DEPTH_TEST_DEFAULT &&
			material->get_distance_fade() == BaseMaterial3D::DISTANCE_FADE_DISABLED && !material->is_proximity_fade_enabled() &&
			material->get_billboard_mode() == BaseMaterial3D::BILLBOARD_DISABLED && !material->is_grow_enabled() &&
			material->get_stencil_mode() == BaseMaterial3D::STENCIL_MODE_DISABLED &&
			!material->get_feature(BaseMaterial3D::FEATURE_REFRACTION) &&
			!material->get_flag(BaseMaterial3D::FLAG_USE_POINT_SIZE) && !material->get_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST) &&
			!material->get_flag(BaseMaterial3D::FLAG_FIXED_SIZE) && !material->get_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE) &&
			!material->get_flag(BaseMaterial3D::FLAG_USE_Z_CLIP_SCALE) && !material->get_flag(BaseMaterial3D::FLAG_USE_FOV_OVERRIDE);
}
} // namespace

Error convert_imported_scene(const Ref<Resource> &p_source, Ref<Resource> &r_resource,
		std::map<ObjectID, BuildOutput> &r_builds, Dictionary &r_report, String &r_message,
		const Settings *p_explicit_settings) {
	r_resource = p_source;
	const Ref<PackedScene> scene = p_source;
	if (scene.is_null()) {
		return OK; // Explicit source ArrayMesh/animation imports stay source data.
	}
	Node *root = scene->instantiate();
	if (root == nullptr) {
		return refuse(r_message, "virtual geometry import scene could not instantiate");
	}
	std::map<std::pair<ObjectID, bool>, Ref<Mesh>> converted;
	Vector<Node *> pending;
	pending.push_back(root);
	Array policy;
	uint64_t output_bytes = 0;
	const uint64_t output_limit = p_explicit_settings != nullptr ? p_explicit_settings->max_output_bytes : UINT64_MAX;
	Error error = OK;
	while (!pending.is_empty() && error == OK) {
		Node *node = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		for (int child = 0; child < node->get_child_count(); ++child) {
			pending.push_back(node->get_child(child));
		}
		auto *instance = Object::cast_to<MeshInstance3D>(node);
		auto *batch = Object::cast_to<MultiMeshInstance3D>(node);
		Ref<Mesh> source;
		if (instance != nullptr) {
			source = instance->get_mesh();
		} else if (batch != nullptr && batch->get_multimesh().is_valid()) {
			source = batch->get_multimesh()->get_mesh();
		}
		if (source.is_null() || Ref<VirtualGeometryMesh>(source).is_valid()) {
			continue;
		}
		String reason;
		bool preserve_area = false;
		uint64_t triangles = 0;
		if (source->get_blend_shape_count() != 0 || (instance != nullptr && instance->get_skin().is_valid())) {
			reason = "deforming_mesh";
		}
		for (int surface = 0; surface < source->get_surface_count(); ++surface) {
			const uint64_t format = source->surface_get_format(surface);
			if (source->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES || (format & (Mesh::ARRAY_FORMAT_BONES | Mesh::ARRAY_FORMAT_WEIGHTS)) != 0) {
				reason = "non_static_triangles";
			}
			const uint32_t indices = source->surface_get_array_index_len(surface);
			triangles += (indices != 0 ? indices : source->surface_get_array_len(surface)) / 3;
			if (p_explicit_settings == nullptr && (!automatic_material(source->surface_get_material(surface)) || (instance != nullptr && !automatic_material(instance->get_surface_override_material(surface))))) {
				reason = "material_requires_explicit_policy";
			}
			for (const Ref<Material> &candidate : { source->surface_get_material(surface), instance != nullptr ? instance->get_surface_override_material(surface) : Ref<Material>() }) {
				const Ref<BaseMaterial3D> material = candidate;
				preserve_area |= material.is_valid() && material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED;
			}
		}
		auto *geometry = Object::cast_to<GeometryInstance3D>(node);
		if ((p_explicit_settings == nullptr && !automatic_material(geometry->get_material_override())) || geometry->get_material_overlay().is_valid()) {
			reason = "instance_material_requires_explicit_policy";
		}
		const Ref<BaseMaterial3D> override_material = geometry->get_material_override();
		preserve_area |= override_material.is_valid() && override_material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED;
		if (p_explicit_settings != nullptr && !reason.is_empty()) {
			error = refuse(r_message, "explicit scene virtual geometry refused: " + reason);
			break;
		}
		if (p_explicit_settings != nullptr) {
			preserve_area = p_explicit_settings->dag.preserve_area;
		}
		if (p_explicit_settings == nullptr && triangles < 8192 && reason.is_empty()) {
			reason = "below_triangle_threshold";
		}
		Dictionary record;
		record["node"] = String(root->get_path_to(node));
		record["source_triangles"] = int64_t(triangles);
		if (reason.is_empty()) {
			record["preserve_area"] = preserve_area;
			const auto key = std::make_pair(source->get_instance_id(), preserve_area);
			const auto previous = converted.find(key);
			Ref<Mesh> replacement;
			if (previous != converted.end()) {
				replacement = previous->second;
			} else {
				Settings settings = p_explicit_settings != nullptr ? *p_explicit_settings : Settings();
				settings.dag.preserve_area = preserve_area;
				if (p_explicit_settings == nullptr) {
				}
				settings.max_output_bytes = output_limit - output_bytes;
				BuildOutput built;
				error = build(source, settings, built, r_message);
				if (error != OK) {
					r_message = "scene node " + String(root->get_path_to(node)) + " mesh " + source->get_name() + vformat(" (completed_meshes=%d completed_bytes=%d scene_limit=%d): ", converted.size(), output_bytes, output_limit) + r_message;
					break;
				}
				record["build"] = built.report;
				output_bytes += built.container.size() + uint64_t(int64_t(built.report["root_bytes"]));
				if (output_bytes > output_limit) {
					error = refuse(r_message, "scene virtual geometry exceeds its complete output budget", ERR_OUT_OF_MEMORY);
					break;
				}
				replacement = built.mesh;
				r_builds.emplace(built.mesh->get_instance_id(), std::move(built));
				converted.emplace(key, replacement);
			}
			if (instance != nullptr) {
				instance->set_mesh(replacement);
			} else {
				// Duplicate only the MultiMesh container. Its source instance data
				// remains unchanged and the replacement Mesh is shared per model.
				Ref<MultiMesh> multi = batch->get_multimesh()->duplicate();
				multi->set_mesh(replacement);
				batch->set_multimesh(multi);
			}
			record["representation"] = "virtual_geometry";
		} else {
			record["representation"] = "ordinary";
			record["reason"] = reason;
		}
		policy.push_back(record);
	}
	if (error == OK && !converted.empty()) {
		Ref<PackedScene> output;
		output.instantiate();
		error = output->pack(root);
		if (error == OK) {
			r_resource = output;
		}
	}
	memdelete(root);
	r_report["policy"] = p_explicit_settings != nullptr ? "explicit_static_scene_v1" : "dense_static_surface_v1";
	r_report["triangle_threshold"] = p_explicit_settings != nullptr ? 0 : 8192;
	r_report["converted_meshes"] = int64_t(converted.size());
	r_report["encoded_bytes"] = int64_t(output_bytes);
	r_report["nodes"] = policy;
	return error;
}

} // namespace CookerVirtualGeometry
