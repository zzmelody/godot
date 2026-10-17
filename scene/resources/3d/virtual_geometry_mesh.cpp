#include "virtual_geometry_mesh.h"

#include "core/object/class_db.h"
#include "scene/resources/material.h"

#include <cmath>
#include <limits>
#include <utility>

namespace {
// Bound root bytes and decoded metadata independently. These are admission
// limits, not a claim about live renderer memory or the page-pool budget.
constexpr uint64_t MAX_ROOT_BYTES = 64 * 1024 * 1024;
constexpr uint64_t MAX_METADATA_BYTES = 256 * 1024 * 1024;

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
	if (bytes.is_empty() || uint64_t(bytes.size()) > MAX_ROOT_BYTES || length <= 0 || !valid_container_file(file)) {
		return ERR_INVALID_DATA;
	}
	VirtualGeometry::GeometryModelRoot candidate;
	const auto decoded = VirtualGeometry::decode_geometry_model_root(bytes.ptr(), bytes.size(), uint64_t(length), MAX_METADATA_BYTES, candidate);
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
	// Never return an ordinary mesh RID with absent vertex buffers. The renderer
	// will own a typed virtual geometry RID when GPU installation is connected.
	ERR_FAIL_V_MSG(RID(), "Virtual geometry GPU storage and rendering are not connected in this build.");
}

void VirtualGeometryMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_geometry_data", "data"), &VirtualGeometryMesh::set_geometry_data);
	ClassDB::bind_method(D_METHOD("get_geometry_data"), &VirtualGeometryMesh::get_geometry_data);
	ClassDB::bind_method(D_METHOD("has_geometry"), &VirtualGeometryMesh::has_geometry);
}
