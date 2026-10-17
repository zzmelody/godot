#include "virtual_geometry_minimum.h"

#include "asset_files.h"

#include <array>
#include <map>
#include <new>
#include <utility>

namespace CookerVirtualGeometry {
namespace {
namespace vg = VirtualGeometry;

struct RootSurface {
	PackedVector3Array positions;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedColorArray colors;
	PackedVector2Array uvs[2];
	PackedInt32Array indices;
};

} // namespace

Error minimum_root_mesh(const Ref<VirtualGeometryMesh> &p_source, Ref<ArrayMesh> &r_mesh, std::vector<uint32_t> &r_slots) {
	ERR_FAIL_COND_V(p_source.is_null() || !p_source->has_geometry(), ERR_INVALID_DATA);
	const auto &model = p_source->get_model();
	ERR_FAIL_COND_V(model.attributes.uv_sets > 2, ERR_UNAVAILABLE);
	const String owner = p_source->get_path().get_slice("::", 0);
	ERR_FAIL_COND_V(owner.is_empty(), ERR_FILE_BAD_PATH);
	const String path = owner.get_base_dir().path_join(p_source->get_container_file());
	Error error = CookerFiles::check_path(path);
	ERR_FAIL_COND_V(error != OK, error);
	const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &error);
	ERR_FAIL_COND_V(file.is_null(), error == OK ? ERR_FILE_CANT_READ : error);
	ERR_FAIL_COND_V(file->get_length() != p_source->get_container_bytes(), ERR_INVALID_DATA);
	uint64_t root_bytes = 0;
	for (uint32_t page = 0; page < model.hierarchy.root_pages; ++page) {
		root_bytes += model.directory.pages[page].disk_bytes;
	}
	ERR_FAIL_COND_V(root_bytes > 64 * 1024 * 1024, ERR_OUT_OF_MEMORY);
	try {
		std::map<uint32_t, RootSurface> surfaces;
		uint64_t vertices = 0;
		for (uint32_t page = 0; page < model.hierarchy.root_pages; ++page) {
			const auto &location = model.directory.pages[page];
			file->seek(location.container_offset);
			const Vector<uint8_t> bytes = file->get_buffer(location.disk_bytes);
			ERR_FAIL_COND_V(bytes.size() != location.disk_bytes, ERR_FILE_CANT_READ);
			vg::GeometryPage geometry;
			vg::HierarchyPage fixups;
			ERR_FAIL_COND_V(vg::decode_geometry_model_page(model, page, bytes.ptr(), bytes.size(), geometry, fixups).error != vg::ModelCodecError::OK, ERR_INVALID_DATA);
			for (const auto &cluster : geometry.clusters) {
				if ((cluster.flags & vg::CLUSTER_FLAG_ROOT_GROUP) == 0) {
					continue;
				}
				vg::QuantizedClusterPositions quantized;
				std::vector<std::array<float, 3>> positions;
				std::vector<float> attributes;
				std::vector<uint32_t> indices;
				std::vector<vg::ClusterMaterialRange> ranges;
				ERR_FAIL_COND_V(vg::unpack_cluster_positions(cluster.positions, cluster.position_bytes.data(), cluster.position_bytes.size(), quantized) != vg::PositionCodecError::OK ||
								vg::reconstruct_cluster_positions(quantized, positions) != vg::PositionCodecError::OK ||
								vg::unpack_cluster_attributes(cluster.attributes.layout, cluster.attributes.bytes.data(), cluster.attributes.bytes.size(), attributes).error != vg::AttributeCodecError::OK ||
								vg::unpack_cluster_topology(cluster.topology.layout, cluster.topology.references.data(), cluster.topology.references.size(), indices) != vg::TopologyCodecError::OK ||
								vg::unpack_cluster_materials(cluster.materials.info, cluster.materials.table, cluster.topology.layout.triangles, ranges) != vg::MaterialCodecError::OK,
						ERR_INVALID_DATA);
				const auto &format = cluster.attributes.layout.format;
				for (const auto &range : ranges) {
					const uint32_t slot = cluster.material_palette[range.material];
					ERR_FAIL_COND_V(slot >= model.materials.size(), ERR_INVALID_DATA);
					auto &surface = surfaces[slot];
					for (uint32_t triangle = range.start; triangle < range.start + range.count; ++triangle) {
						const auto &a = positions[indices[triangle * 3]];
						const auto &b = positions[indices[triangle * 3 + 1]];
						const auto &c = positions[indices[triangle * 3 + 2]];
						if (a == b || a == c || b == c) {
							continue;
						}
						ERR_FAIL_COND_V(vertices + 3 > 2000000, ERR_OUT_OF_MEMORY);
						vertices += 3;
						for (uint32_t corner = 0; corner < 3; ++corner) {
							const uint32_t vertex = indices[triangle * 3 + corner];
							const auto &position = positions[vertex];
							surface.indices.push_back(surface.positions.size());
							surface.positions.push_back(Vector3(position[0], position[1], position[2]));
							const float *value = attributes.data() + vertex * format.scalar_count();
							surface.normals.push_back(Vector3(value[0], value[1], value[2]));
							value += 3;
							if (format.has_tangents) {
								for (uint32_t axis = 0; axis < 4; ++axis) {
									surface.tangents.push_back(*value++);
								}
							}
							if (format.has_colors) {
								surface.colors.push_back(Color(value[0], value[1], value[2], value[3]));
								value += 4;
							}
							for (uint32_t uv = 0; uv < format.uv_sets; ++uv) {
								surface.uvs[uv].push_back(Vector2(value[0], value[1]));
								value += 2;
							}
						}
					}
				}
			}
		}
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		std::vector<uint32_t> slots;
		for (const auto &entry : surfaces) {
			const auto &surface = entry.second;
			if (surface.indices.is_empty()) {
				continue;
			}
			Array arrays;
			arrays.resize(Mesh::ARRAY_MAX);
			arrays[Mesh::ARRAY_VERTEX] = surface.positions;
			arrays[Mesh::ARRAY_NORMAL] = surface.normals;
			arrays[Mesh::ARRAY_INDEX] = surface.indices;
			if (model.attributes.has_tangents) {
				arrays[Mesh::ARRAY_TANGENT] = surface.tangents;
			}
			if (model.attributes.has_colors) {
				arrays[Mesh::ARRAY_COLOR] = surface.colors;
			}
			if (model.attributes.uv_sets > 0) {
				arrays[Mesh::ARRAY_TEX_UV] = surface.uvs[0];
			}
			if (model.attributes.uv_sets > 1) {
				arrays[Mesh::ARRAY_TEX_UV2] = surface.uvs[1];
			}
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
			ERR_FAIL_COND_V(mesh->get_surface_count() != int(slots.size() + 1), ERR_INVALID_DATA);
			mesh->surface_set_material(slots.size(), p_source->surface_get_material(entry.first));
			slots.push_back(entry.first);
		}
		ERR_FAIL_COND_V(slots.empty(), ERR_INVALID_DATA);
		r_mesh = mesh;
		r_slots = std::move(slots);
		return OK;
	} catch (const std::bad_alloc &) {
		return ERR_OUT_OF_MEMORY;
	}
}

} // namespace CookerVirtualGeometry
