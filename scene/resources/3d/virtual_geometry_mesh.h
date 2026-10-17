#ifndef VIRTUAL_GEOMETRY_MESH_H
#define VIRTUAL_GEOMETRY_MESH_H

#include "core/io/virtual_geometry/model_codec.h"
#include "scene/resources/mesh.h"

// A serialized geometry root and material slots, with a separately paged
// container. No detailed geometry is expanded by the ordinary Mesh array API.
// GPU storage/installation is not connected yet; get_rid() refuses explicitly.
class VirtualGeometryMesh : public Mesh {
	GDCLASS(VirtualGeometryMesh, Mesh);

	PackedByteArray root_data;
	VirtualGeometry::GeometryModelRoot model;
	Vector<Ref<Material>> materials;
	String container_file;
	uint64_t container_bytes = 0;
	AABB bounds;

protected:
	static void _bind_methods();
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_value) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

public:
	// Main-thread configuration. Refusal leaves the entire old root/material
	// binding intact. Input contains no physical GPU addresses or detail bytes.
	Error set_geometry_data(const Dictionary &p_data);
	Dictionary get_geometry_data() const;
	bool has_geometry() const;
	const VirtualGeometry::GeometryModelRoot &get_model() const { return model; }
	const String &get_container_file() const { return container_file; }
	uint64_t get_container_bytes() const { return container_bytes; }

	int get_surface_count() const override;
	int surface_get_array_len(int p_idx) const override;
	int surface_get_array_index_len(int p_idx) const override;
	Array surface_get_arrays(int p_surface) const override;
	TypedArray<Array> surface_get_blend_shape_arrays(int p_surface) const override;
	Dictionary surface_get_lods(int p_surface) const override;
	BitField<ArrayFormat> surface_get_format(int p_idx) const override;
	PrimitiveType surface_get_primitive_type(int p_idx) const override;
	void surface_set_material(int p_idx, const Ref<Material> &p_material) override;
	Ref<Material> surface_get_material(int p_idx) const override;
	int get_blend_shape_count() const override;
	StringName get_blend_shape_name(int p_index) const override;
	void set_blend_shape_name(int p_index, const StringName &p_name) override;
	AABB get_aabb() const override;
	RID get_rid() const override;
};

#endif // VIRTUAL_GEOMETRY_MESH_H
