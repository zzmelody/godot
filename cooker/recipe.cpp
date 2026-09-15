/*<<----- VEYA_COOKER: bounded Luau asset recipes with native builders and one atomic output. */
#include "recipe.h"
#include "asset_files.h"

#include "core/io/json.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/packed_scene.h"

#include "Luau/Compiler.h"
#include "lua.h"
#include "lualib.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace CookerRecipe {
namespace {
constexpr const char *LUAU_VERSION = "0.738";
constexpr const char *LUAU_COMMIT = "c54f558b4d5748ab0658610b8ce0c432053e41eb";
constexpr int HANDLE_TAG = 17;
constexpr int SOURCE_BYTES = 64 * 1024;

struct Limits {
	int memory_mb = 32;
	int time_ms = 2000;
	int max_vertices = 500000;
	int max_instances = 10000;
	int max_resources = 256;
	int input_mb = 256;
	int output_mb = 64;
};

struct NodeDeleter {
	void operator()(Node *p_node) const { memdelete(p_node); }
};
using OwnedNode = std::unique_ptr<Node, NodeDeleter>;

struct Recipe {
	Limits limits;
	lua_State *L = nullptr;
	const char *fault = nullptr;
	bool memory_failed = false;
	size_t allocated = 0, peak_memory = 0;
	int vertices = 0, instances = 0;
	uint64_t interrupts = 0;
	uint32_t random_state = 1;
	std::chrono::steady_clock::time_point deadline;
	std::vector<Ref<Resource>> resources;
	Dictionary parameters;
	Dictionary inputs;
	Dictionary input_hashes;
	Ref<Resource> result;
	std::string bytecode;

	~Recipe() { if (L) lua_close(L); }
	static Recipe &self(lua_State *p_state) { return *static_cast<Recipe *>(lua_callbacks(p_state)->userdata); }

	static void *allocate(void *p_ud, void *p_ptr, size_t p_old, size_t p_new) {
		auto &s = *static_cast<Recipe *>(p_ud);
		if (!p_ptr) p_old = 0;
		if (!p_new) { std::free(p_ptr); s.allocated -= p_old; return nullptr; }
		if (p_new > p_old && p_new - p_old > size_t(s.limits.memory_mb) * 1024 * 1024 - s.allocated) {
			s.memory_failed = true;
			return nullptr;
		}
		void *next = std::realloc(p_ptr, p_new);
		if (!next) { s.memory_failed = true; return nullptr; }
		s.allocated = s.allocated - p_old + p_new;
		s.peak_memory = MAX(s.peak_memory, s.allocated);
		return next;
	}
	void require(bool p_ok, const char *p_message) {
		if (!p_ok) { fault = p_message; luaL_error(L, "%s", p_message); }
	}
	void check_budget() {
		require(!memory_failed, "recipe memory limit exceeded");
		require(!fault, "recipe has a previous host API failure");
		require(std::chrono::steady_clock::now() < deadline, "recipe execution budget exceeded");
	}
	static void interrupt(lua_State *p_state, int p_gc) {
		if (p_gc >= 0) return;
		auto &s = self(p_state);
		if (++s.interrupts > 2000000 || std::chrono::steady_clock::now() >= s.deadline || s.memory_failed || s.fault) {
			if (!s.fault) s.fault = s.memory_failed ? "recipe memory limit exceeded" : "recipe execution budget exceeded";
			// Never resume a broken VM. pcall/xpcall cannot swallow this budget fault.
			if (lua_isyieldable(p_state)) lua_break(p_state);
			else luaL_error(p_state, "%s", s.fault);
		}
	}
	void arity(int p_count) { check_budget(); require(lua_gettop(L) == p_count, "incorrect cooker API argument count"); }
	void field(int p_index, const char *p_name) {
		p_index = lua_absindex(L, p_index);
		lua_pushstring(L, p_name); lua_rawget(L, p_index);
	}
	void fields(int p_index, std::initializer_list<const char *> p_names) {
		require(lua_istable(L, p_index), "expected a configuration table");
		p_index = lua_absindex(L, p_index);
		lua_pushnil(L);
		while (lua_next(L, p_index)) {
			require(lua_type(L, -2) == LUA_TSTRING, "configuration keys must be strings");
			(void)string(-2); // Reject NUL-suffixed aliases as well as unknown keys.
			bool known = false;
			for (const char *name : p_names) known |= std::strcmp(name, lua_tostring(L, -2)) == 0;
			require(known, "unknown configuration field");
			lua_pop(L, 1);
		}
	}
	int array(int p_index, int p_max) {
		require(lua_istable(L, p_index), "expected a dense array");
		p_index = lua_absindex(L, p_index);
		int length = lua_objlen(L, p_index);
		require(length <= p_max, "array exceeds element limit");
		int count = 0;
		lua_pushnil(L);
		while (lua_next(L, p_index)) {
			require(++count <= length && lua_type(L, -2) == LUA_TNUMBER, "expected a dense array without named fields");
			double key = lua_tonumber(L, -2);
			require(std::isfinite(key) && key >= 1 && key <= length && key == std::floor(key), "invalid array index");
			lua_pop(L, 1);
		}
		require(count == length, "sparse arrays are not accepted");
		return length;
	}
	double number(int p_index, double p_min = -100000, double p_max = 100000) {
		require(lua_type(L, p_index) == LUA_TNUMBER, "expected a number, not a coerced string");
		double value = lua_tonumber(L, p_index);
		require(std::isfinite(value) && value >= p_min && value <= p_max, "number is non-finite or out of range");
		return value;
	}
	double number_field(int p_index, const char *p_name, double p_default, double p_min, double p_max) {
		field(p_index, p_name);
		double value = lua_isnil(L, -1) ? p_default : number(-1, p_min, p_max);
		lua_pop(L, 1); return value;
	}
	Vector3 vector(int p_index) {
		require(array(p_index, 3) == 3, "expected three vector components");
		p_index = lua_absindex(L, p_index);
		Vector3 value;
		for (int i = 0; i < 3; ++i) { lua_rawgeti(L, p_index, i + 1); value[i] = number(-1); lua_pop(L, 1); }
		return value;
	}
	Vector3 vector_field(int p_index, const char *p_name, Vector3 p_default) {
		field(p_index, p_name);
		Vector3 value = lua_isnil(L, -1) ? p_default : vector(-1);
		lua_pop(L, 1); return value;
	}
	String string(int p_index) {
		require(lua_type(L, p_index) == LUA_TSTRING, "expected a string");
		size_t length = 0;
		const char *text = lua_tolstring(L, p_index, &length);
		require(length <= 256 && std::memchr(text, 0, length) == nullptr, "string exceeds limit or contains NUL");
		String value;
		require(value.append_utf8(text, length) == OK, "string must be UTF-8");
		return value;
	}
	Ref<Resource> handle(int p_index) {
		auto *id = static_cast<size_t *>(lua_touserdatatagged(L, p_index, HANDLE_TAG));
		require(id && *id < resources.size(), "expected a Cooker asset handle");
		return resources[*id];
	}
	int push_resource(const Ref<Resource> &p_resource) {
		check_budget();
		require(p_resource.is_valid() && resources.size() < size_t(limits.max_resources), "asset resource limit exceeded");
		Ref<Mesh> mesh = p_resource;
		if (mesh.is_valid()) {
			for (int surface = 0; surface < mesh->get_surface_count(); ++surface) {
				vertices += mesh->surface_get_array_len(surface);
				require(vertices <= limits.max_vertices, "mesh vertex budget exceeded");
			}
		}
		Ref<PackedScene> packed = p_resource;
		if (packed.is_valid()) {
			OwnedNode root(packed->instantiate());
			require(root != nullptr, "could not instantiate PackedScene input");
			vertices += count_vertices(root.get());
			require(vertices <= limits.max_vertices, "scene vertex budget exceeded");
		}
		size_t id = resources.size();
		resources.push_back(p_resource);
		*static_cast<size_t *>(lua_newuserdatatagged(L, sizeof(size_t), HANDLE_TAG)) = id;
		return 1;
	}
	static int count_vertices(Node *p_node) {
		int count = 0;
		MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
		if (mesh_instance && mesh_instance->get_mesh().is_valid()) {
			Ref<Mesh> mesh = mesh_instance->get_mesh();
			for (int surface = 0; surface < mesh->get_surface_count(); ++surface) count += mesh->surface_get_array_len(surface);
		}
		MultiMeshInstance3D *multi_instance = Object::cast_to<MultiMeshInstance3D>(p_node);
		if (multi_instance && multi_instance->get_multimesh().is_valid() && multi_instance->get_multimesh()->get_mesh().is_valid()) {
			Ref<Mesh> mesh = multi_instance->get_multimesh()->get_mesh();
			for (int surface = 0; surface < mesh->get_surface_count(); ++surface) count += mesh->surface_get_array_len(surface);
		}
		for (int i = 0; i < p_node->get_child_count(); ++i) count += count_vertices(p_node->get_child(i));
		return count;
	}
	static void collect_bounds(Node *p_node, const Transform3D &p_parent, AABB &r_bounds, bool &r_has_bounds) {
		Transform3D transform = p_parent;
		Node3D *node_3d = Object::cast_to<Node3D>(p_node);
		if (node_3d) transform = p_parent * node_3d->get_transform();
		MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
		if (mesh_instance && mesh_instance->get_mesh().is_valid()) {
			AABB bounds = transform.xform(mesh_instance->get_mesh()->get_aabb());
			if (r_has_bounds) r_bounds.merge_with(bounds);
			else { r_bounds = bounds; r_has_bounds = true; }
		}
		MultiMeshInstance3D *multi_instance = Object::cast_to<MultiMeshInstance3D>(p_node);
		if (multi_instance && multi_instance->get_multimesh().is_valid()) {
			Ref<MultiMesh> multi = multi_instance->get_multimesh();
			if (multi->get_mesh().is_valid()) {
				AABB mesh_bounds = multi->get_mesh()->get_aabb();
				for (int i = 0; i < multi->get_instance_count(); ++i) {
					AABB bounds = (transform * multi->get_instance_transform(i)).xform(mesh_bounds);
					if (r_has_bounds) r_bounds.merge_with(bounds);
					else { r_bounds = bounds; r_has_bounds = true; }
				}
			}
		}
		for (int i = 0; i < p_node->get_child_count(); ++i) collect_bounds(p_node->get_child(i), transform, r_bounds, r_has_bounds);
	}
	static void make_scene_local(Node *p_node, Node *p_owner) {
		p_node->set_owner(p_owner);
		for (int i = 0; i < p_node->get_child_count(); ++i) {
			Node *child = p_node->get_child(i);
			if (child->get_owner()) make_scene_local(child, p_owner);
		}
	}
	static int random(lua_State *p_state) {
		auto &s = self(p_state); s.arity(0);
		uint32_t x = s.random_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s.random_state = x;
		lua_pushnumber(p_state, double(x) / 4294967296.0); return 1;
	}
	static int material(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"color", "roughness", "metallic"});
		Ref<StandardMaterial3D> material; material.instantiate();
		s.field(1, "color");
		if (!lua_isnil(p_state, -1)) {
			int count = s.array(-1, 4); s.require(count == 3 || count == 4, "color needs three or four components");
			Color color(1, 1, 1, 1);
			for (int i = 0; i < count; ++i) { lua_rawgeti(p_state, -1, i + 1); color[i] = s.number(-1, 0, 1); lua_pop(p_state, 1); }
			material->set_albedo(color);
		}
		lua_pop(p_state, 1);
		material->set_roughness(s.number_field(1, "roughness", 0.7, 0, 1));
		material->set_metallic(s.number_field(1, "metallic", 0, 0, 1));
		return s.push_resource(material);
	}
	static int primitive(lua_State *p_state) {
		auto &s = self(p_state); s.arity(2);
		String kind = s.string(1);
		Ref<Mesh> mesh;
		if (kind == "box") {
			s.fields(2, {"size"});
			Vector3 size = s.vector_field(2, "size", Vector3(1, 1, 1));
			s.require(size.x > 0 && size.y > 0 && size.z > 0, "box dimensions must be positive");
			Ref<BoxMesh> box; box.instantiate(); box->set_size(size); mesh = box;
		} else if (kind == "cylinder") {
			s.fields(2, {"height", "radius", "top_radius", "segments"});
			Ref<CylinderMesh> cylinder; cylinder.instantiate();
			double radius = s.number_field(2, "radius", 0.5, 0.001, 100000);
			cylinder->set_height(s.number_field(2, "height", 1, 0.001, 100000));
			cylinder->set_bottom_radius(radius);
			cylinder->set_top_radius(s.number_field(2, "top_radius", radius, 0, 100000));
			double segments = s.number_field(2, "segments", 24, 3, 128);
			s.require(segments == std::floor(segments), "segments must be an integer");
			cylinder->set_radial_segments(segments); mesh = cylinder;
		} else if (kind == "sphere") {
			s.fields(2, {"radius", "segments", "rings"});
			Ref<SphereMesh> sphere; sphere.instantiate();
			double radius = s.number_field(2, "radius", 0.5, 0.001, 100000);
			sphere->set_radius(radius); sphere->set_height(radius * 2);
			double segments = s.number_field(2, "segments", 24, 4, 128);
			double rings = s.number_field(2, "rings", 12, 2, 64);
			s.require(segments == std::floor(segments) && rings == std::floor(rings), "segments and rings must be integers");
			sphere->set_radial_segments(segments); sphere->set_rings(rings); mesh = sphere;
		} else s.require(false, "unknown primitive; expected box, cylinder or sphere");
		return s.push_resource(mesh);
	}
	static int mesh(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"vertices", "indices", "normals", "uvs"});
		s.field(1, "vertices");
		int count = s.array(-1, s.limits.max_vertices - s.vertices);
		s.require(count >= 3, "a mesh requires at least three vertices");
		PackedVector3Array vertices; vertices.resize(count);
		for (int i = 0; i < count; ++i) { lua_rawgeti(p_state, -1, i + 1); vertices.set(i, s.vector(-1)); lua_pop(p_state, 1); }
		lua_pop(p_state, 1);
		s.field(1, "indices");
		int index_count = s.array(-1, 6 * s.limits.max_vertices);
		s.require(index_count >= 3 && index_count % 3 == 0, "indices must contain complete triangles");
		PackedInt32Array indices; indices.resize(index_count);
		for (int i = 0; i < index_count; ++i) {
			lua_rawgeti(p_state, -1, i + 1); double index = s.number(-1, 1, count);
			s.require(index == std::floor(index), "mesh indices must be 1-based integers");
			indices.set(i, int(index) - 1); lua_pop(p_state, 1);
		}
		lua_pop(p_state, 1);
		PackedVector3Array normals; normals.resize(count);
		s.field(1, "normals");
		if (lua_isnil(p_state, -1)) {
			for (int i = 0; i < index_count; i += 3) {
				int a = indices[i], b = indices[i + 1], c = indices[i + 2];
				// Godot front faces are clockwise.
				Vector3 normal = (vertices[c] - vertices[a]).cross(vertices[b] - vertices[a]);
				s.require(normal.length_squared() > 1e-16, "degenerate triangle");
				for (int j : {a, b, c}) normals.set(j, normals[j] + normal);
			}
		} else {
			s.require(s.array(-1, count) == count, "normals must match vertex count");
			for (int i = 0; i < count; ++i) { lua_rawgeti(p_state, -1, i + 1); normals.set(i, s.vector(-1)); lua_pop(p_state, 1); }
		}
		lua_pop(p_state, 1);
		for (int i = 0; i < count; ++i) {
			s.require(normals[i].is_finite() && normals[i].length_squared() > 1e-16, "normal is zero or invalid (including unused vertices)");
			normals.set(i, normals[i].normalized());
		}
		Array arrays; arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices; arrays[Mesh::ARRAY_NORMAL] = normals; arrays[Mesh::ARRAY_INDEX] = indices;
		s.field(1, "uvs");
		if (!lua_isnil(p_state, -1)) {
			s.require(s.array(-1, count) == count, "UVs must match vertex count");
			PackedVector2Array uvs; uvs.resize(count);
			for (int i = 0; i < count; ++i) {
				lua_rawgeti(p_state, -1, i + 1); s.require(s.array(-1, 2) == 2, "UV requires two components");
				Vector2 uv;
				for (int j = 0; j < 2; ++j) { lua_rawgeti(p_state, -1, j + 1); uv[j] = s.number(-1); lua_pop(p_state, 1); }
				uvs.set(i, uv); lua_pop(p_state, 1);
			}
			arrays[Mesh::ARRAY_TEX_UV] = uvs;
		}
		lua_pop(p_state, 1);
		Ref<ArrayMesh> mesh; mesh.instantiate(); mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		return s.push_resource(mesh);
	}
	Transform3D transform(int p_index) {
		Vector3 position = vector_field(p_index, "position", Vector3());
		field(p_index, "basis");
		Basis basis;
		if (!lua_isnil(L, -1)) {
			require(array(-1, 3) == 3, "basis requires three columns");
			for (int i = 0; i < 3; ++i) { lua_rawgeti(L, -1, i + 1); basis.set_column(i, vector(-1)); lua_pop(L, 1); }
			for (const char *name : {"rotation", "scale"}) { field(p_index, name); require(lua_isnil(L, -1), "basis cannot be combined with rotation or scale"); lua_pop(L, 1); }
		} else {
			basis = Basis::from_euler(vector_field(p_index, "rotation", Vector3()));
			Vector3 scale = vector_field(p_index, "scale", Vector3(1, 1, 1));
			for (int i = 0; i < 3; ++i) basis.set_column(i, basis.get_column(i) * scale[i]);
		}
		lua_pop(L, 1);
		require(basis.is_finite() && std::abs(basis.determinant()) > 1e-12, "instance basis must be finite and invertible");
		return Transform3D(basis, position);
	}
	static int scene(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		int count = s.array(1, s.limits.max_instances - s.instances);
		s.require(count > 0, "scene must contain at least one instance");
		s.instances += count;
		struct Batch { Ref<Mesh> mesh; Ref<Material> material; Vector<Transform3D> transforms; };
		std::vector<Batch> batches;
		OwnedNode root(memnew(Node3D)); root->set_name("RecipeAsset");
		for (int i = 0; i < count; ++i) {
			lua_rawgeti(p_state, 1, i + 1); int item = lua_absindex(p_state, -1);
			s.fields(item, {"asset", "material", "position", "rotation", "scale", "basis", "name", "remove"});
			s.field(item, "asset"); Ref<Resource> asset = s.handle(-1); lua_pop(p_state, 1);
			s.field(item, "material"); Ref<Material> material;
			if (!lua_isnil(p_state, -1)) { material = s.handle(-1); s.require(material.is_valid(), "material requires a Material handle"); }
			lua_pop(p_state, 1);
			Transform3D transform = s.transform(item);
			Ref<Mesh> mesh = asset;
			if (mesh.is_valid()) {
				s.field(item, "name"); s.require(lua_isnil(p_state, -1), "name is only valid for PackedScene instances"); lua_pop(p_state, 1);
				s.field(item, "remove"); s.require(lua_isnil(p_state, -1), "remove is only valid for PackedScene instances"); lua_pop(p_state, 1);
				size_t batch = 0;
				while (batch < batches.size() && (batches[batch].mesh != mesh || batches[batch].material != material)) ++batch;
				if (batch == batches.size()) batches.push_back({mesh, material, {}});
				batches[batch].transforms.push_back(transform);
			} else {
				Ref<PackedScene> packed = asset;
				s.require(packed.is_valid(), "scene instances require Mesh or PackedScene handles");
				s.require(material.is_null(), "PackedScene instances cannot use a material override");
				OwnedNode instance(packed->instantiate());
				Node3D *instance_3d = Object::cast_to<Node3D>(instance.get());
				s.require(instance_3d != nullptr, "PackedScene instance root must be Node3D");
				instance_3d->set_transform(transform);
				s.field(item, "name");
				if (!lua_isnil(p_state, -1)) {
					String name = s.string(-1);
					s.require(!name.is_empty() && name.validate_node_name() == name, "invalid PackedScene instance name");
					instance->set_name(name);
				} else instance->set_name("Scene" + itos(i));
				lua_pop(p_state, 1);
				bool localize = false;
				s.field(item, "remove");
				if (!lua_isnil(p_state, -1)) {
					int remove_count = s.array(-1, 128);
					localize = remove_count > 0;
					for (int remove_index = 0; remove_index < remove_count; ++remove_index) {
						lua_rawgeti(p_state, -1, remove_index + 1);
						String path = s.string(-1); lua_pop(p_state, 1);
						s.require(!path.is_empty() && !path.begins_with("/") && path.find("..") == -1, "invalid removal path");
						Node *removed = instance->get_node_or_null(NodePath(path));
						s.require(removed && removed != instance.get() && removed->get_parent(), "PackedScene removal path was not found");
						removed->get_parent()->remove_child(removed); memdelete(removed);
					}
				}
				lua_pop(p_state, 1);
				root->add_child(instance.get());
				if (localize) {
					instance->set_scene_file_path(String());
					make_scene_local(instance.get(), root.get());
				} else instance->set_owner(root.get());
				instance.release();
			}
			lua_pop(p_state, 1);
		}
		for (size_t i = 0; i < batches.size(); ++i) {
			auto &batch = batches[i];
			Ref<MultiMesh> multi; multi.instantiate();
			multi->set_transform_format(MultiMesh::TRANSFORM_3D); multi->set_mesh(batch.mesh);
			multi->set_instance_count(batch.transforms.size());
			for (int j = 0; j < batch.transforms.size(); ++j) multi->set_instance_transform(j, batch.transforms[j]);
			auto *node = memnew(MultiMeshInstance3D);
			node->set_name("Batch" + itos(i)); node->set_multimesh(multi); node->set_material_override(batch.material);
			root->add_child(node); node->set_owner(root.get());
		}
		Ref<PackedScene> scene; scene.instantiate();
		s.require(scene->pack(root.get()) == OK, "could not pack recipe scene");
		return s.push_resource(scene);
	}
	static int input(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		String name = s.string(1);
		s.require(s.inputs.has(name), "input was not declared in the job");
		Ref<Resource> resource = s.inputs[name];
		return s.push_resource(resource);
	}
	void push_vector(Vector3 p_value) {
		lua_createtable(L, 3, 0);
		for (int i = 0; i < 3; ++i) { lua_pushnumber(L, p_value[i]); lua_rawseti(L, -2, i + 1); }
	}
	static int bounds(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		Ref<Resource> resource = s.handle(1);
		AABB box;
		bool has_bounds = false;
		Ref<Mesh> mesh = resource;
		if (mesh.is_valid()) { box = mesh->get_aabb(); has_bounds = true; }
		Ref<PackedScene> packed = resource;
		if (packed.is_valid()) {
			OwnedNode root(packed->instantiate());
			s.require(root != nullptr, "could not instantiate PackedScene for bounds");
			collect_bounds(root.get(), Transform3D(), box, has_bounds);
		}
		s.require(has_bounds, "bounds requires a Mesh or nonempty PackedScene handle");
		lua_createtable(p_state, 0, 2);
		s.push_vector(box.position); lua_setfield(p_state, -2, "min");
		s.push_vector(box.get_end()); lua_setfield(p_state, -2, "max");
		return 1;
	}
	void push_json(const Variant &p_value, int p_depth = 0) {
		require(p_depth <= 16, "parameters exceed nesting limit");
		switch (p_value.get_type()) {
			case Variant::NIL: lua_pushnil(L); break;
			case Variant::BOOL: lua_pushboolean(L, bool(p_value)); break;
			case Variant::INT: case Variant::FLOAT:
				require(std::isfinite(double(p_value)), "parameters must contain finite numbers");
				lua_pushnumber(L, double(p_value)); break;
			case Variant::STRING: {
				CharString text = String(p_value).utf8(); lua_pushlstring(L, text.get_data(), text.length()); break;
			}
			case Variant::ARRAY: {
				Array data = p_value; require(data.size() <= 4096, "too many parameter entries");
				lua_createtable(L, data.size(), 0);
				for (int i = 0; i < data.size(); ++i) { require(data[i].get_type() != Variant::NIL, "null array parameters are not supported"); push_json(data[i], p_depth + 1); lua_rawseti(L, -2, i + 1); }
				lua_setreadonly(L, -1, true); break;
			}
			case Variant::DICTIONARY: {
				Dictionary data = p_value; require(data.size() <= 4096, "too many parameter entries");
				lua_createtable(L, 0, data.size());
				for (const Variant *key = data.next(); key; key = data.next(key)) {
					require(key->get_type() == Variant::STRING && data[*key].get_type() != Variant::NIL, "parameters require string keys and non-null values");
					CharString name = String(*key).utf8(); lua_pushlstring(L, name.get_data(), name.length()); push_json(data[*key], p_depth + 1); lua_rawset(L, -3);
				}
				lua_setreadonly(L, -1, true); break;
			}
			default: require(false, "unsupported parameter type");
		}
	}
	static int setup(lua_State *p_state) {
		auto &s = self(p_state);
		for (lua_CFunction library : {luaopen_base, luaopen_table, luaopen_string, luaopen_math, luaopen_bit32, luaopen_utf8}) { library(p_state); lua_settop(p_state, 0); }
		for (const char *name : {"print", "loadstring", "getfenv", "setfenv", "collectgarbage", "newproxy"}) { lua_pushnil(p_state); lua_setglobal(p_state, name); }
		lua_getglobal(p_state, "math");
		for (const char *name : {"random", "randomseed", "noise"}) { lua_pushnil(p_state); lua_setfield(p_state, -2, name); }
		lua_pop(p_state, 1);
		lua_newtable(p_state);
		const luaL_Reg methods[] = {{"primitive", primitive}, {"material", material}, {"mesh", mesh}, {"scene", scene}, {"input", input}, {"bounds", bounds}, {"random", random}, {nullptr, nullptr}};
		for (const auto *method = methods; method->name; ++method) { lua_pushcfunction(p_state, method->func, method->name); lua_setfield(p_state, -2, method->name); }
		s.push_json(s.parameters); lua_setfield(p_state, -2, "parameters");
		lua_setreadonly(p_state, -1, true); lua_setglobal(p_state, "cooker");
		luaL_sandbox(p_state); luaL_sandboxthread(p_state);
		return 0;
	}
	bool execute(const std::string &p_source, String &r_error) {
		try {
			Luau::CompileOptions options; options.optimizationLevel = 1; options.debugLevel = 1;
			const char *const absent[] = {"buffer", "vector", "integer", "os", "debug", "coroutine", nullptr};
			const char *const removed[] = {"math.random", "math.randomseed", "math.noise", "print", "loadstring", "getfenv", "setfenv", "collectgarbage", "newproxy", nullptr};
			options.mutableGlobals = absent; options.disabledBuiltins = removed;
			// Only this compiler can produce VM input; external bytecode is never loaded.
			bytecode = Luau::compile(p_source, options);
			L = lua_newstate(allocate, this);
			if (!L) { r_error = "cannot allocate recipe VM"; return false; }
			lua_callbacks(L)->userdata = this;
			int status = lua_cpcall(L, setup, nullptr);
			if (status == LUA_OK) status = luau_load(L, "=cooker_recipe", bytecode.data(), bytecode.size(), 0);
			deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.time_ms);
			lua_callbacks(L)->interrupt = interrupt;
			if (status == LUA_OK) status = lua_resume(L, nullptr, 0);
			if (status == LUA_OK && !fault && !memory_failed) {
				if (lua_gettop(L) != 1 || !lua_touserdatatagged(L, 1, HANDLE_TAG)) fault = "recipe must return exactly one Cooker asset handle";
				else result = handle(1);
			}
			if (status == LUA_OK && !fault && !memory_failed && std::chrono::steady_clock::now() < deadline) return true;
			if (memory_failed) r_error = "recipe memory limit exceeded";
			else if (fault) r_error = fault;
			else if (std::chrono::steady_clock::now() >= deadline) r_error = "recipe execution budget exceeded";
			else r_error = lua_type(L, -1) == LUA_TSTRING ? String::utf8(lua_tostring(L, -1)).left(1024) : String("recipe failed");
		} catch (const std::exception &error) { r_error = String::utf8(error.what()).left(1024); }
		return false;
	}
};

Error reject(Dictionary &r_result, const String &p_message, Error p_error = ERR_INVALID_PARAMETER) {
	r_result["message"] = p_message;
	return p_error;
}
}

Dictionary capabilities() {
	Dictionary result;
	result["language"] = "Luau"; result["version"] = LUAU_VERSION; result["commit"] = LUAU_COMMIT;
	result["api_version"] = 2; result["operation"] = "run-recipe";
	Array apis;
	for (const char *name : {"primitive", "mesh", "material", "scene", "input", "bounds", "random", "parameters"}) apis.push_back(name);
	result["apis"] = apis;
	return result;
}

Error run(const Dictionary &p_job, Dictionary &r_result) {
	for (const Variant *key = p_job.next(); key; key = p_job.next(key)) {
		String name = *key;
		if (name != "operation" && name != "source" && name != "output" && name != "parameters" && name != "inputs" && name != "seed" && name != "limits") return reject(r_result, "Unknown recipe job field: " + name);
	}
	if (p_job.get("source", Variant()).get_type() != Variant::STRING || p_job.get("output", Variant()).get_type() != Variant::STRING) return reject(r_result, "source and output must be strings");
	String source = p_job["source"], output = p_job["output"];
	if (source.get_extension() != "luau") return reject(r_result, "recipe source must be .luau text");
	if (output.get_extension() != "scn" && output.get_extension() != "res") return reject(r_result, "recipe output must be .scn or .res");
	Error error = CookerFiles::check_path(source);
	if (error == OK) error = CookerFiles::check_path(output, true);
	if (error != OK) return reject(r_result, "recipe path rejected", error);
	if (FileAccess::exists(output) || DirAccess::dir_exists_absolute(output)) return reject(r_result, "recipe output already exists; use a new revision", ERR_ALREADY_EXISTS);
	Recipe recipe;
	for (const char *key : {"parameters", "inputs", "limits"}) if (p_job.has(key) && p_job[key].get_type() != Variant::DICTIONARY) return reject(r_result, String(key) + " must be an object");
	recipe.parameters = p_job.get("parameters", Dictionary());
	if (JSON::stringify(recipe.parameters).utf8().length() > SOURCE_BYTES) return reject(r_result, "parameters exceed 64 KiB");
	Variant seed = p_job.get("seed", 1);
	if ((seed.get_type() != Variant::INT && seed.get_type() != Variant::FLOAT) || !std::isfinite(double(seed)) || double(seed) < 0 || double(seed) > UINT32_MAX || double(seed) != std::floor(double(seed))) return reject(r_result, "seed must be a uint32");
	recipe.random_state = uint32_t(int64_t(seed));
	if (!recipe.random_state) recipe.random_state = 1;
	Dictionary limits = p_job.get("limits", Dictionary());
	for (const Variant *key = limits.next(); key; key = limits.next(key)) {
		String name = *key; int *target = nullptr;
		if (name == "memory_mb") target = &recipe.limits.memory_mb;
		else if (name == "time_ms") target = &recipe.limits.time_ms;
		else if (name == "max_vertices") target = &recipe.limits.max_vertices;
		else if (name == "max_instances") target = &recipe.limits.max_instances;
		else if (name == "max_resources") target = &recipe.limits.max_resources;
		else if (name == "input_mb") target = &recipe.limits.input_mb;
		else if (name == "output_mb") target = &recipe.limits.output_mb;
		if (!target) return reject(r_result, "unknown recipe limit: " + name);
		Variant value = limits[*key];
		if ((value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) || !std::isfinite(double(value)) || double(value) < 1 || double(value) > *target || double(value) != std::floor(double(value))) return reject(r_result, "limits may only lower positive integer defaults");
		*target = int(value);
	}
	Dictionary inputs = p_job.get("inputs", Dictionary());
	if (inputs.size() > 32) return reject(r_result, "at most 32 declared inputs are supported");
	HashSet<String> inspected_inputs;
	uint64_t input_bytes = 0;
	for (const Variant *key = inputs.next(); key; key = inputs.next(key)) {
		if (key->get_type() != Variant::STRING || String(*key).utf8().length() > 256 || inputs[*key].get_type() != Variant::STRING) return reject(r_result, "input names and paths must be bounded strings");
		String path = inputs[*key];
		if (!path.begins_with("res://assets/generated/")) return reject(r_result, "inputs must already be cooked assets");
		HashSet<String> files;
		error = CookerFiles::collect(path, files);
		if (error != OK) return reject(r_result, "input dependency validation failed", error);
		for (const String &file : files) {
			if (inspected_inputs.has(file)) continue;
			inspected_inputs.insert(file);
			Ref<FileAccess> input = FileAccess::open(file, FileAccess::READ);
			if (input.is_null()) return reject(r_result, "cannot read input", ERR_FILE_CANT_READ);
			input_bytes += input->get_length();
			if (input_bytes > uint64_t(recipe.limits.input_mb) * 1024 * 1024) return reject(r_result, "input dependency bytes exceed recipe limit");
			recipe.input_hashes[file] = FileAccess::get_sha256(file);
		}
		Ref<Resource> resource = ResourceLoader::load(path);
		Ref<Mesh> mesh = resource; Ref<Material> material = resource; Ref<PackedScene> scene = resource;
		if (mesh.is_null() && material.is_null() && scene.is_null()) return reject(r_result, "recipe inputs must be Mesh, Material or PackedScene resources");
		// Shader includes are not all exposed by ResourceLoader's dependency list.
		// Resolve them through the same bounded collector used by pack jobs.
		HashSet<ObjectID> visited;
		error = CookerFiles::collect_shader_includes(resource, visited, files);
		if (error != OK) return reject(r_result, "input shader dependency validation failed", error);
		for (const String &dependency : files) {
			if (inspected_inputs.has(dependency)) continue;
			inspected_inputs.insert(dependency);
			Ref<FileAccess> input = FileAccess::open(dependency, FileAccess::READ);
			if (input.is_null()) return reject(r_result, "cannot read input dependency", ERR_FILE_CANT_READ);
			input_bytes += input->get_length();
			if (input_bytes > uint64_t(recipe.limits.input_mb) * 1024 * 1024) return reject(r_result, "input dependency bytes exceed recipe limit");
			recipe.input_hashes[dependency] = FileAccess::get_sha256(dependency);
		}
		recipe.inputs[*key] = resource;
	}
	Ref<FileAccess> file = FileAccess::open(source, FileAccess::READ);
	if (file.is_null() || file->get_length() == 0 || file->get_length() > SOURCE_BYTES) return reject(r_result, "recipe source must contain 1..65536 bytes");
	PackedByteArray code = file->get_buffer(file->get_length());
	String text;
	if (std::memchr(code.ptr(), 0, code.size()) || text.append_utf8(reinterpret_cast<const char *>(code.ptr()), code.size()) != OK) return reject(r_result, "recipe must be UTF-8 text without NUL (not bytecode)");
	String message;
	if (!recipe.execute(std::string(reinterpret_cast<const char *>(code.ptr()), code.size()), message)) return reject(r_result, message, ERR_SCRIPT_FAILED);
	Ref<PackedScene> scene = recipe.result;
	if ((output.get_extension() == "scn") != scene.is_valid()) return reject(r_result, ".scn requires a scene; use .res for a mesh or material");
	// One immutable output, only published after the VM and serialization succeed.
	// Supervisor owns this workspace exclusively; this is not a hostile-filesystem sandbox.
	error = DirAccess::make_dir_recursive_absolute(output.get_base_dir());
	if (error != OK) return reject(r_result, "cannot create output directory", error);
	String temporary = output.get_basename() + ".recipe-" + itos(OS::get_singleton()->get_ticks_usec()) + "." + output.get_extension();
	if (FileAccess::exists(temporary)) return reject(r_result, "temporary output collision", ERR_ALREADY_EXISTS);
	error = ResourceSaver::save(recipe.result, temporary, ResourceSaver::FLAG_COMPRESS);
	if (error == OK) {
		Ref<FileAccess> saved = FileAccess::open(temporary, FileAccess::READ);
		if (saved.is_null() || saved->get_length() > uint64_t(recipe.limits.output_mb) * 1024 * 1024) error = ERR_OUT_OF_MEMORY;
	}
	if (error == OK && FileAccess::exists(output)) error = ERR_ALREADY_EXISTS;
	if (error == OK) error = DirAccess::rename_absolute(temporary, output);
	if (error != OK) { if (FileAccess::exists(temporary)) DirAccess::remove_absolute(temporary); return reject(r_result, "could not publish recipe output", error); }
	r_result["output"] = output; r_result["type"] = recipe.result->get_class();
	r_result["sha256"] = FileAccess::get_sha256(output);
	r_result["source_sha256"] = FileAccess::get_sha256(source);
	r_result["input_sha256"] = recipe.input_hashes;
	r_result["parameters"] = recipe.parameters; r_result["seed"] = seed;
	Dictionary effective_limits;
	effective_limits["memory_mb"] = recipe.limits.memory_mb; effective_limits["time_ms"] = recipe.limits.time_ms;
	effective_limits["max_vertices"] = recipe.limits.max_vertices; effective_limits["max_instances"] = recipe.limits.max_instances;
	effective_limits["max_resources"] = recipe.limits.max_resources; effective_limits["input_mb"] = recipe.limits.input_mb;
	effective_limits["output_mb"] = recipe.limits.output_mb;
	r_result["limits"] = effective_limits;
	r_result["runtime"] = capabilities();
	r_result["resources"] = int64_t(recipe.resources.size()); r_result["vertices"] = recipe.vertices;
	r_result["instances"] = recipe.instances; r_result["vm_peak_bytes"] = int64_t(recipe.peak_memory);
	return OK;
}
}
/*>>----- VEYA_COOKER */
