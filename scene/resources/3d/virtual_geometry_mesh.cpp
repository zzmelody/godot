#include "virtual_geometry_mesh.h"

#include "core/object/class_db.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/virtual_geometry_streaming.h"

#include <cmath>
#include <limits>
#include <utility>

namespace {

bool valid_container_file(const String &p_file) {
	if (p_file.is_empty() || p_file.length() > 128 || p_file.begins_with(".") || !p_file.ends_with(".vgp")) {
		return false;
	}
	for (int index = 0; index < p_file.length(); ++index) {
		const char32_t value = p_file[index];
		if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.')) {
			return false;
		}
	}
	return true;
}
} // namespace

Error VirtualGeometryMesh::set_geometry_data(const Dictionary &p_data) {
	// Published roots are immutable once they own a rendering identity. World
	// updates prepare a fresh Resource and publish it through the existing host
	// transaction, preserving the old root until the replacement is ready.
	if (mesh.is_valid()) {
		return ERR_ALREADY_IN_USE;
	}
	if (p_data.size() != 4 || p_data.get("root", Variant()).get_type() != Variant::PACKED_BYTE_ARRAY ||
			p_data.get("container", Variant()).get_type() != Variant::STRING ||
			p_data.get("container_bytes", Variant()).get_type() != Variant::INT ||
			p_data.get("materials", Variant()).get_type() != Variant::ARRAY) {
		return ERR_INVALID_DATA;
	}
	const PackedByteArray bytes = p_data["root"];
	const String file = p_data["container"];
	const int64_t length = p_data["container_bytes"];
	const Array slots = p_data["materials"];
	if (bytes.is_empty() || uint64_t(bytes.size()) > UINT32_MAX - 15 || length <= 0 || !valid_container_file(file)) {
		return ERR_INVALID_DATA;
	}
	VirtualGeometry::GeometryModelRoot candidate;
	const auto decoded = VirtualGeometry::decode_geometry_model_root(bytes.ptr(), bytes.size(), uint64_t(length), UINT64_MAX, candidate);
	if (decoded.error != VirtualGeometry::ModelCodecError::OK || uint64_t(slots.size()) != candidate.materials.size()) {
		return ERR_INVALID_DATA;
	}
	const Vector3 lower(candidate.lower[0], candidate.lower[1], candidate.lower[2]);
	Vector3 extent;
	for (int axis = 0; axis < 3; ++axis) {
		// AABB stores position+size. Finite endpoints alone do not guarantee a
		// finite size, and nearest rounding can exclude the upper endpoint.
		const double difference = double(candidate.upper[axis]) - double(candidate.lower[axis]);
		real_t size = real_t(difference);
		if (double(size) < difference) {
			size = std::nextafter(size, std::numeric_limits<real_t>::infinity());
		}
		if (!std::isfinite(size)) {
			return ERR_INVALID_DATA;
		}
		extent[axis] = size;
	}
	Vector<Ref<Material>> candidate_materials;
	candidate_materials.resize(slots.size());
	for (int index = 0; index < slots.size(); ++index) {
		if (slots[index].get_type() != Variant::NIL && slots[index].get_type() != Variant::OBJECT) {
			return ERR_INVALID_DATA;
		}
		const Ref<Material> material = slots[index];
		if (slots[index].get_type() == Variant::OBJECT && slots[index].get_validated_object() && material.is_null()) {
			return ERR_INVALID_DATA;
		}
		candidate_materials.write[index] = material;
	}
	root_data = bytes;
	model = std::move(candidate);
	materials = std::move(candidate_materials);
	container_file = file;
	container_bytes = uint64_t(length);
	bounds = AABB(lower, extent);
	clear_cache();
	emit_changed();
	return OK;
}

Dictionary VirtualGeometryMesh::get_geometry_data() const {
	Dictionary data;
	if (!has_geometry()) {
		return data;
	}
	Array slots;
	slots.resize(materials.size());
	for (int index = 0; index < materials.size(); ++index) {
		slots[index] = materials[index];
	}
	data["root"] = root_data;
	data["container"] = container_file;
	data["container_bytes"] = int64_t(container_bytes);
	data["materials"] = slots;
	return data;
}

bool VirtualGeometryMesh::has_geometry() const {
	return !root_data.is_empty();
}

bool VirtualGeometryMesh::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name != "_geometry") {
		return false;
	}
	ERR_FAIL_COND_V_MSG(p_value.get_type() != Variant::DICTIONARY, false, "Virtual geometry data must be a dictionary.");
	const Error error = set_geometry_data(p_value);
	ERR_FAIL_COND_V_MSG(error != OK, false, "Invalid virtual geometry root or material binding.");
	return true;
}

bool VirtualGeometryMesh::_get(const StringName &p_name, Variant &r_value) const {
	if (p_name != "_geometry") {
		return false;
	}
	r_value = get_geometry_data();
	return true;
}

void VirtualGeometryMesh::_get_property_list(List<PropertyInfo> *p_list) const {
	p_list->push_back(PropertyInfo(Variant::DICTIONARY, "_geometry", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL));
}

int VirtualGeometryMesh::get_surface_count() const {
	return materials.size();
}

int VirtualGeometryMesh::surface_get_array_len(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, materials.size(), 0);
	ERR_FAIL_V_MSG(0, "Virtual geometry has no ordinary Mesh vertex array. Use published resource costs.");
}

int VirtualGeometryMesh::surface_get_array_index_len(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, materials.size(), 0);
	ERR_FAIL_V_MSG(0, "Virtual geometry has no ordinary Mesh index array. Use published resource costs.");
}

Array VirtualGeometryMesh::surface_get_arrays(int p_surface) const {
	ERR_FAIL_INDEX_V(p_surface, materials.size(), Array());
	ERR_FAIL_V_MSG(Array(), "Virtual geometry cannot expand detailed geometry through the Mesh array API.");
}

TypedArray<Array> VirtualGeometryMesh::surface_get_blend_shape_arrays(int p_surface) const {
	ERR_FAIL_INDEX_V(p_surface, materials.size(), TypedArray<Array>());
	return TypedArray<Array>();
}

Dictionary VirtualGeometryMesh::surface_get_lods(int p_surface) const {
	ERR_FAIL_INDEX_V(p_surface, materials.size(), Dictionary());
	return Dictionary();
}

BitField<Mesh::ArrayFormat> VirtualGeometryMesh::surface_get_format(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, materials.size(), 0);
	return 0;
}

Mesh::PrimitiveType VirtualGeometryMesh::surface_get_primitive_type(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, materials.size(), PRIMITIVE_TRIANGLES);
	return PRIMITIVE_TRIANGLES;
}

void VirtualGeometryMesh::surface_set_material(int p_idx, const Ref<Material> &p_material) {
	ERR_FAIL_INDEX(p_idx, materials.size());
	materials.write[p_idx] = p_material;
	if (mesh.is_valid()) {
		RS::get_singleton()->mesh_surface_set_material(mesh, p_idx, p_material.is_valid() ? p_material->get_rid() : RID());
	}
	emit_changed();
}

Ref<Material> VirtualGeometryMesh::surface_get_material(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, materials.size(), Ref<Material>());
	return materials[p_idx];
}

int VirtualGeometryMesh::get_blend_shape_count() const {
	return 0;
}

StringName VirtualGeometryMesh::get_blend_shape_name(int p_index) const {
	ERR_FAIL_V_MSG(StringName(), "Virtual geometry does not support blend shapes.");
}

void VirtualGeometryMesh::set_blend_shape_name(int p_index, const StringName &p_name) {
	ERR_FAIL_MSG("Virtual geometry does not support blend shapes.");
}

AABB VirtualGeometryMesh::get_aabb() const {
	return bounds;
}

RID VirtualGeometryMesh::get_rid() const {
	if (mesh.is_null() && has_geometry()) {
		const Error error = _install(root_data, model, materials, container_file, mesh);
		ERR_FAIL_COND_V_MSG(error != OK, RID(), vformat("Virtual geometry installation failed for %s (error %s).", get_path(), error));
		const_cast<VirtualGeometryMesh *>(this)->emit_changed();
	}
	return mesh;
}

Error VirtualGeometryMesh::_install(const PackedByteArray &p_root, const VirtualGeometry::GeometryModelRoot &p_model,
		const Vector<Ref<Material>> &p_materials, const String &p_file, RID &r_mesh) const {
	TypedArray<RID> slots;
	for (const Ref<Material> &material : p_materials) {
		slots.push_back(material.is_valid() ? material->get_rid() : RID());
	}
	RS *server = RS::get_singleton();
	if (server->get_rendering_device() == nullptr) {
		// The Cooker/headless Dummy owns typed bounds and materials only. Other
		// unsupported renderers refuse mesh_set_virtual_geometry explicitly.
		RID candidate = server->mesh_create();
		const Error error = server->mesh_set_virtual_geometry(candidate, 1, p_root, slots);
		if (error != OK) {
			server->free_rid(candidate);
			return error;
		}
		r_mesh = candidate;
		return OK;
	}
	const String owner_file = get_path().get_slice("::", 0);
	if (owner_file.is_empty() || !owner_file.is_absolute_path()) {
		return ERR_FILE_BAD_PATH;
	}
	VirtualGeometryStreaming *streaming = VirtualGeometryStreaming::get_singleton();
	if (streaming == nullptr) {
		return ERR_UNCONFIGURED;
	}
	const Error error = streaming->install(p_root, p_model, slots, owner_file.get_base_dir().path_join(p_file), r_mesh);
	if (error == OK) {
		streamed = true;
	}
	return error;
}

void VirtualGeometryMesh::_release_mesh() {
	if (mesh.is_null() || RS::get_singleton() == nullptr) {
		return;
	}
	VirtualGeometryStreaming *streaming = VirtualGeometryStreaming::get_singleton();
	if (streamed) {
		if (streaming != nullptr) {
			streaming->remove(mesh);
		}
	} else {
		RS::get_singleton()->free_rid(mesh);
	}
	mesh = RID();
}

bool VirtualGeometryMesh::is_render_ready() const {
	if (!streamed || mesh.is_null() || VirtualGeometryStreaming::get_singleton() == nullptr) {
		return false;
	}
	bool ready = false;
	return VirtualGeometryStreaming::get_singleton()->get_status(mesh, ready) == OK && ready;
}

Error VirtualGeometryMesh::get_streaming_error() const {
	if (!streamed || mesh.is_null() || VirtualGeometryStreaming::get_singleton() == nullptr) {
		return ERR_UNCONFIGURED;
	}
	bool ready = false;
	return VirtualGeometryStreaming::get_singleton()->get_status(mesh, ready);
}

Error VirtualGeometryMesh::prepare_virtual_geometry() const {
	if (!has_geometry()) {
		return ERR_UNCONFIGURED;
	}
	if (mesh.is_null()) {
		const Error error = _install(root_data, model, materials, container_file, mesh);
		if (error != OK) {
			return error;
		}
		const_cast<VirtualGeometryMesh *>(this)->emit_changed();
	}
	if (!streamed) {
		return OK; // Dummy data preparation has no GPU readiness claim.
	}
	TypedArray<RID> material_rids;
	for (const Ref<Material> &material : materials) {
		material_rids.push_back(material.is_valid() ? material->get_rid() : RID());
	}
	const Error material_error = RS::get_singleton()->virtual_geometry_prepare_materials(material_rids);
	if (material_error != OK) {
		return material_error;
	}
	bool ready = false;
	const Error error = VirtualGeometryStreaming::get_singleton()->get_status(mesh, ready);
	return error != OK ? error : ready ? OK
									   : ERR_BUSY;
}

VirtualGeometryMesh::~VirtualGeometryMesh() {
	_release_mesh();
}

void VirtualGeometryMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_geometry_data", "data"), &VirtualGeometryMesh::set_geometry_data);
	ClassDB::bind_method(D_METHOD("get_geometry_data"), &VirtualGeometryMesh::get_geometry_data);
	ClassDB::bind_method(D_METHOD("has_geometry"), &VirtualGeometryMesh::has_geometry);
	ClassDB::bind_method(D_METHOD("is_render_ready"), &VirtualGeometryMesh::is_render_ready);
	ClassDB::bind_method(D_METHOD("get_streaming_error"), &VirtualGeometryMesh::get_streaming_error);
	ClassDB::bind_method(D_METHOD("prepare_virtual_geometry"), &VirtualGeometryMesh::prepare_virtual_geometry);
}
