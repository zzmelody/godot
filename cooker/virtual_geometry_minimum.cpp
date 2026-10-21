#include "virtual_geometry_minimum.h"

#include "asset_files.h"
#include "virtual_geometry/model_decoder.h"
#include "core/object/script_language.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"

#include <array>
#include <functional>
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
	try {
		std::map<uint32_t, RootSurface> surfaces;
		vg::GeometryModelPageDecoder decoder;
		ERR_FAIL_COND_V(decoder.initialize(model, model.hierarchy.root_pages).error != vg::ModelCodecError::OK, ERR_INVALID_DATA);
		for (uint32_t page : decoder.get_order()) {
			const auto &location = model.directory.pages[page];
			file->seek(location.container_offset);
			const Vector<uint8_t> bytes = file->get_buffer(location.disk_bytes);
			ERR_FAIL_COND_V(bytes.size() != location.disk_bytes, ERR_FILE_CANT_READ);
			const vg::GeometryPage *geometry = nullptr;
			vg::HierarchyPage fixups;
			ERR_FAIL_COND_V(decoder.decode(page, bytes.ptr(), bytes.size(), geometry, fixups).error != vg::ModelCodecError::OK, ERR_INVALID_DATA);
			for (const auto &cluster : geometry->clusters) {
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
						ERR_FAIL_COND_V(uint64_t(surface.positions.size()) + 3 > INT32_MAX, ERR_OUT_OF_MEMORY);
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

Error BakeMeshCache::get(const Ref<VirtualGeometryMesh> &p_source, BakeMesh &r_mesh) {
	try {
		ERR_FAIL_COND_V(p_source.is_null(), ERR_INVALID_PARAMETER);
		const auto found = meshes.find(p_source->get_instance_id());
		if (found != meshes.end()) {
			r_mesh = found->second;
			return OK;
		}
		BakeMesh candidate;
		const Error error = minimum_root_mesh(p_source, candidate.mesh, candidate.source_slots);
		ERR_FAIL_COND_V(error != OK, error);
		uint64_t added = 0;
		for (int surface = 0; surface < candidate.mesh->get_surface_count(); ++surface) {
			added += candidate.mesh->surface_get_array_len(surface);
		}
		ERR_FAIL_COND_V(added > UINT64_MAX - vertices, ERR_OUT_OF_MEMORY);
		meshes.emplace(p_source->get_instance_id(), candidate);
		vertices += added;
		r_mesh = std::move(candidate);
		return OK;
	} catch (const std::bad_alloc &) {
		return ERR_OUT_OF_MEMORY;
	}
}

Error minimum_asset_roots(const Ref<PackedScene> &p_source, Ref<PackedScene> &r_scene) {
	ERR_FAIL_COND_V(p_source.is_null(), ERR_INVALID_PARAMETER);
	Node *source = p_source->instantiate();
	ERR_FAIL_NULL_V(source, ERR_INVALID_DATA);
	struct SourceOwner {
		Node *node;
		~SourceOwner() { memdelete(node); }
	} owner{ source };
	try {
		struct AssetSurface {
			RootSurface data;
			Ref<Material> material;
		};
		std::map<std::pair<ObjectID, uint64_t>, size_t> surface_slots;
		std::vector<AssetSurface> surfaces;
		BakeMeshCache cache;
		uint64_t instances = 0, triangles = 0;
		real_t margin = 0;
		std::function<Error(Node *, const Transform3D &)> visit = [&](Node *p_node, const Transform3D &p_parent) -> Error {
			const Node3D *spatial = Object::cast_to<Node3D>(p_node);
			const Ref<Script> script = p_node->get_script();
			ERR_FAIL_COND_V(spatial == nullptr || script.is_valid() || p_node->has_meta("veya_resources"), ERR_INVALID_DATA);
			const Transform3D transform = p_parent * spatial->get_transform();
			ERR_FAIL_COND_V(!transform.is_finite(), ERR_INVALID_DATA);
			if (const auto *node = Object::cast_to<MultiMeshInstance3D>(p_node)) {
				const Ref<MultiMesh> multi = node->get_multimesh();
				ERR_FAIL_COND_V(multi.is_null() || multi->get_transform_format() != MultiMesh::TRANSFORM_3D || !multi->is_using_colors() || !multi->is_using_custom_data() ||
						(multi->get_visible_instance_count() >= 0 && multi->get_visible_instance_count() != multi->get_instance_count()), ERR_INVALID_DATA);
				const Ref<VirtualGeometryMesh> geometry = multi->get_mesh();
				ERR_FAIL_COND_V(geometry.is_null(), ERR_INVALID_DATA);
				BakeMesh baked;
				const Error error = cache.get(geometry, baked);
				ERR_FAIL_COND_V(error != OK, error);
				// Offline storage preserves the packed buffer. Read all fields from
				// it; individual color/custom getters on a dummy renderer are empty.
				const Vector<float> buffer = RenderingServer::get_singleton()->multimesh_get_buffer(multi->get_rid());
				ERR_FAIL_COND_V(buffer.size() != int64_t(multi->get_instance_count()) * 20, ERR_INVALID_DATA);
				for (int instance = 0; instance < multi->get_instance_count(); ++instance) {
					const float *values = buffer.ptr() + int64_t(instance) * 20;
					Transform3D local;
					for (int row = 0; row < 3; ++row) {
						for (int column = 0; column < 3; ++column) {
							local.basis.rows[row][column] = values[row * 4 + column];
						}
						local.origin[row] = values[row * 4 + 3];
					}
					const Transform3D placement = transform * local;
					const real_t determinant = placement.basis.determinant();
					ERR_FAIL_COND_V(!placement.is_finite() || !Math::is_finite(determinant) || determinant == 0 || !placement.affine_inverse().is_finite(), ERR_INVALID_DATA);
					// This opt-in path changes the instance coordinate system. Verify
					// the USD asset-height/origin contract before replacing its data.
					const Color height(values[12], values[13], values[14], values[15]);
					const Color custom(values[16], values[17], values[18], values[19]);
					const Vector3 origin = placement.affine_inverse().origin;
					const bool unused_data = height == Color(0, 0, 0, 0) && custom == Color(0, 0, 0, 0);
					ERR_FAIL_COND_V_MSG(!unused_data && (!Vector3(height.r, height.g, height.b).is_equal_approx(Vector3(placement.basis.get_column(0).y, placement.basis.get_column(1).y, placement.basis.get_column(2).y)) ||
							!Math::is_equal_approx(height.a, placement.origin.y) || !Vector3(custom.r, custom.g, custom.b).is_equal_approx(origin) || !Math::is_equal_approx(custom.a, real_t(1))), ERR_INVALID_DATA,
							vformat("Asset coordinates differ at %s[%d]: height=%s expected=%s offset=%s expected_offset=%s origin=%s expected_origin=%s marker=%s", node->get_name(), instance, Vector3(height.r, height.g, height.b), Vector3(placement.basis.get_column(0).y, placement.basis.get_column(1).y, placement.basis.get_column(2).y), height.a, placement.origin.y, Vector3(custom.r, custom.g, custom.b), origin, custom.a));
					const Basis normal = placement.basis.inverse().transposed();
					const bool mirrored = determinant < 0;
					for (int slot = 0; slot < baked.mesh->get_surface_count(); ++slot) {
						Ref<Material> material = node->get_material_override();
						if (material.is_null()) {
							material = baked.mesh->surface_get_material(slot);
						}
						ERR_FAIL_COND_V(material.is_null(), ERR_INVALID_DATA);
						const uint64_t format = baked.mesh->surface_get_format(slot) & (Mesh::ARRAY_FORMAT_NORMAL | Mesh::ARRAY_FORMAT_TANGENT | Mesh::ARRAY_FORMAT_COLOR | Mesh::ARRAY_FORMAT_TEX_UV | Mesh::ARRAY_FORMAT_TEX_UV2);
						// Object IDs identify sharing only. The source traversal defines
						// output order, independently of process-local ID allocation.
						const auto [found, added] = surface_slots.emplace(std::make_pair(material->get_instance_id(), format), surfaces.size());
						if (added) {
							surfaces.push_back({ {}, material });
						}
						auto &surface = surfaces[found->second];
						auto &out = surface.data;
						const Array arrays = baked.mesh->surface_get_arrays(slot);
						const PackedVector3Array points = arrays[Mesh::ARRAY_VERTEX], normals = arrays[Mesh::ARRAY_NORMAL];
						const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
						const PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
						const PackedVector2Array uv = arrays[Mesh::ARRAY_TEX_UV], uv2 = arrays[Mesh::ARRAY_TEX_UV2];
						const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
						ERR_FAIL_COND_V(uint64_t(out.positions.size()) + points.size() > INT32_MAX || uint64_t(out.indices.size()) + indices.size() > INT32_MAX, ERR_OUT_OF_MEMORY);
						const int offset = out.positions.size();
						for (int vertex = 0; vertex < points.size(); ++vertex) {
							out.positions.push_back(placement.xform(points[vertex]));
							const Vector3 n = normal.xform(normals[vertex]).normalized();
							out.normals.push_back(n);
							if (!tangents.is_empty()) {
								Vector3 tangent = placement.basis.xform(Vector3(tangents[vertex * 4], tangents[vertex * 4 + 1], tangents[vertex * 4 + 2]));
								tangent = (tangent - n * n.dot(tangent)).normalized();
								out.tangents.push_back(tangent.x);
								out.tangents.push_back(tangent.y);
								out.tangents.push_back(tangent.z);
								out.tangents.push_back(tangents[vertex * 4 + 3] * (mirrored ? -1 : 1));
							}
							if (!colors.is_empty()) {
								out.colors.push_back(colors[vertex]);
							}
							if (!uv.is_empty()) {
								out.uvs[0].push_back(uv[vertex]);
							}
							if (!uv2.is_empty()) {
								out.uvs[1].push_back(uv2[vertex]);
							}
						}
						for (int triangle = 0; triangle < indices.size(); triangle += 3) {
							out.indices.push_back(offset + indices[triangle]);
							out.indices.push_back(offset + indices[triangle + (mirrored ? 2 : 1)]);
							out.indices.push_back(offset + indices[triangle + (mirrored ? 1 : 2)]);
						}
						triangles += indices.size() / 3;
					}
					++instances;
				}
				const real_t scale = transform.basis.get_column(0).length() + transform.basis.get_column(1).length() + transform.basis.get_column(2).length();
				margin = MAX(margin, node->get_extra_cull_margin() * scale);
			} else {
				ERR_FAIL_COND_V(p_node->get_class() != StringName("Node3D"), ERR_INVALID_DATA);
			}
			for (int child = 0; child < p_node->get_child_count(); ++child) {
				const Error error = visit(p_node->get_child(child), transform);
				ERR_FAIL_COND_V(error != OK, error);
			}
			return OK;
		};
		const Error error = visit(source, Transform3D());
		ERR_FAIL_COND_V(error != OK, error);
		ERR_FAIL_COND_V(surfaces.empty(), ERR_INVALID_DATA);
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		for (const auto &surface : surfaces) {
			const auto &data = surface.data;
			Array arrays;
			arrays.resize(Mesh::ARRAY_MAX);
			arrays[Mesh::ARRAY_VERTEX] = data.positions;
			arrays[Mesh::ARRAY_NORMAL] = data.normals;
			arrays[Mesh::ARRAY_INDEX] = data.indices;
			if (!data.tangents.is_empty()) {
				arrays[Mesh::ARRAY_TANGENT] = data.tangents;
			}
			if (!data.colors.is_empty()) {
				arrays[Mesh::ARRAY_COLOR] = data.colors;
			}
			for (int uv = 0; uv < 2; ++uv) {
				if (!data.uvs[uv].is_empty()) {
					arrays[uv == 0 ? Mesh::ARRAY_TEX_UV : Mesh::ARRAY_TEX_UV2] = data.uvs[uv];
				}
			}
			const int slot = mesh->get_surface_count();
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
			ERR_FAIL_COND_V(mesh->get_surface_count() != slot + 1, ERR_INVALID_DATA);
			mesh->surface_set_material(slot, surface.material);
		}
		Ref<MultiMesh> multi;
		multi.instantiate();
		multi->set_transform_format(MultiMesh::TRANSFORM_3D);
		multi->set_use_colors(true);
		multi->set_use_custom_data(true);
		multi->set_mesh(mesh);
		multi->set_instance_count(1);
		PackedFloat32Array buffer;
		buffer.resize(20);
		buffer.fill(0);
		buffer.set(0, 1);
		buffer.set(5, 1);
		buffer.set(10, 1);
		buffer.set(13, 1);
		buffer.set(19, 1);
		RenderingServer::get_singleton()->multimesh_set_buffer(multi->get_rid(), buffer);
		auto *root = memnew(MultiMeshInstance3D);
		SourceOwner result_owner{ root };
		root->set_name("MinimumAssetRoots");
		root->set_multimesh(multi);
		root->set_extra_cull_margin(margin);
		Ref<PackedScene> result;
		result.instantiate();
		const Error packed = result->pack(root);
		ERR_FAIL_COND_V(packed != OK, packed);
		print_line("MINIMUM_ASSET_ROOTS instances=" + uitos(instances) + " triangles=" + uitos(triangles));
		r_scene = result;
		return OK;
	} catch (const std::bad_alloc &) {
		return ERR_OUT_OF_MEMORY;
	}
}

} // namespace CookerVirtualGeometry
