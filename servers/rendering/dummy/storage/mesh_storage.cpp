/**************************************************************************/
/*  mesh_storage.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "mesh_storage.h"

using namespace RendererDummy;

MeshStorage *MeshStorage::singleton = nullptr;

MeshStorage::MeshStorage() {
	singleton = this;
}

MeshStorage::~MeshStorage() {
	singleton = nullptr;
}

RID MeshStorage::mesh_allocate() {
	return mesh_owner.allocate_rid();
}

void MeshStorage::mesh_initialize(RID p_rid) {
	mesh_owner.initialize_rid(p_rid, DummyMesh());
}

void MeshStorage::mesh_free(RID p_rid) {
	DummyMesh *mesh = mesh_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(mesh);
	mesh->dependency.deleted_notify(p_rid);
	mesh_owner.free(p_rid);
}

void MeshStorage::mesh_surface_remove(RID p_mesh, int p_surface) {
	DummyMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);
	m->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
	m->surfaces.remove_at(p_surface);
}

void MeshStorage::mesh_clear(RID p_mesh) {
	DummyMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);

	m->surfaces.clear();
}

RID MeshStorage::_multimesh_allocate() {
	return multimesh_owner.allocate_rid();
}

void MeshStorage::_multimesh_initialize(RID p_rid) {
	multimesh_owner.initialize_rid(p_rid, DummyMultiMesh());
}

void MeshStorage::_multimesh_free(RID p_rid) {
	DummyMultiMesh *multimesh = multimesh_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(multimesh);

	multimesh_owner.free(p_rid);
}

void MeshStorage::_multimesh_set_buffer(RID p_multimesh, const Vector<float> &p_buffer) {
	DummyMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL(multimesh);
	/*<<----- VEYA_COOKER: validate the serialized instance buffer before accessing it. */
#ifdef VEYA_COOKER
	ERR_FAIL_COND(p_buffer.size() != int64_t(multimesh->instance_count) * multimesh->stride);
#endif
	/*>>----- VEYA_COOKER */
	multimesh->buffer.resize(p_buffer.size());
	float *cache_data = multimesh->buffer.ptrw();
	memcpy(cache_data, p_buffer.ptr(), p_buffer.size() * sizeof(float));
}

Vector<float> MeshStorage::_multimesh_get_buffer(RID p_multimesh) const {
	DummyMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL_V(multimesh, Vector<float>());

	return multimesh->buffer;
}

/*<<----- VEYA_COOKER: data-only 3D MultiMesh editing, no GPU or 2D instance support. */
#ifdef VEYA_COOKER
void MeshStorage::_multimesh_allocate_data(RID p_multimesh, int p_instances, RSE::MultimeshTransformFormat p_transform_format, bool p_use_colors, bool p_use_custom_data, bool p_use_indirect) {
	DummyMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL(multimesh);
	ERR_FAIL_COND(p_instances < 0);
	// MultiMesh starts with an empty 2D allocation before deserializing transform_format.
	ERR_FAIL_COND(p_instances > 0 && p_transform_format != RSE::MULTIMESH_TRANSFORM_3D);
	multimesh->instance_count = p_instances;
	multimesh->stride = 12 + (p_use_colors ? 4 : 0) + (p_use_custom_data ? 4 : 0);
	multimesh->buffer.resize(int64_t(p_instances) * multimesh->stride);
	multimesh->buffer.fill(0.0f);
}

void MeshStorage::_multimesh_instance_set_transform(RID p_multimesh, int p_index, const Transform3D &p_transform) {
	DummyMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL(multimesh);
	ERR_FAIL_INDEX(p_index, multimesh->instance_count);
	float *data = multimesh->buffer.ptrw() + int64_t(p_index) * multimesh->stride;
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			data[row * 4 + column] = p_transform.basis[row][column];
		}
		data[row * 4 + 3] = p_transform.origin[row];
	}
}

Transform3D MeshStorage::_multimesh_instance_get_transform(RID p_multimesh, int p_index) const {
	DummyMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL_V(multimesh, Transform3D());
	ERR_FAIL_INDEX_V(p_index, multimesh->instance_count, Transform3D());
	const float *data = multimesh->buffer.ptr() + int64_t(p_index) * multimesh->stride;
	Transform3D transform;
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			transform.basis[row][column] = data[row * 4 + column];
		}
		transform.origin[row] = data[row * 4 + 3];
	}
	return transform;
}
#endif
/*>>----- VEYA_COOKER */
