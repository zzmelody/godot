/*<<----- VEYA_COOKER: bounded Luau asset recipes with native builders and one atomic output. */
#include "recipe.h"
#include "asset_files.h"

#include "core/io/json.h"
#include "core/io/image.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "scene/animation/animation_player.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/curve.h"
#include "scene/resources/curve_texture.h"
#include "scene/resources/environment.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/gradient.h"
#include "scene/resources/gradient_texture.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/particle_process_material.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/portable_compressed_texture.h"
#include "scene/resources/shader.h"
#include "scene/resources/sky.h"
#include "scene/resources/texture.h"

#include "Luau/Compiler.h"
#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <map>
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
	int effect_layers = 0, effect_particle_systems = 0, effect_particles = 0, effect_textures = 0;
	int field_cells = 0, point_count = 0;
	uint64_t interrupts = 0;
	uint32_t random_state = 1, seed_value = 1;
	std::chrono::steady_clock::time_point deadline;
	std::vector<Ref<Resource>> resources;
	Dictionary parameters;
	Dictionary inputs;
	Dictionary input_hashes;
	Ref<Resource> result;
	std::string bytecode;
	// Decoded float rasters, keyed by handle id. Images stay opaque to Luau.
	struct FieldView { int width = 0, height = 0, channels = 0; double origin_x = 0, origin_z = 0, cell = 1; std::vector<float> data; };
	std::map<size_t, FieldView> field_views;

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
	bool boolean_field(int p_index, const char *p_name, bool p_default) {
		field(p_index, p_name);
		require(lua_isnil(L, -1) || lua_type(L, -1) == LUA_TBOOLEAN, "expected a boolean field");
		bool value = lua_isnil(L, -1) ? p_default : bool(lua_toboolean(L, -1));
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
	Vector2 vector2(int p_index) {
		require(array(p_index, 2) == 2, "expected two vector components");
		p_index = lua_absindex(L, p_index); Vector2 value;
		for (int i = 0; i < 2; ++i) { lua_rawgeti(L, p_index, i + 1); value[i] = number(-1); lua_pop(L, 1); }
		return value;
	}
	Vector2 vector2_field(int p_index, const char *p_name, Vector2 p_default) {
		field(p_index, p_name);
		Vector2 value = lua_isnil(L, -1) ? p_default : vector2(-1);
		lua_pop(L, 1); return value;
	}
	Color color_field(int p_index, const char *p_name, Color p_default) {
		field(p_index, p_name);
		Color value = p_default;
		if (!lua_isnil(L, -1)) {
			const int count = array(-1, 4); require(count == 3 || count == 4, "color needs three or four components");
			for (int i = 0; i < count; ++i) { lua_rawgeti(L, -1, i + 1); value[i] = number(-1, 0, 1); lua_pop(L, 1); }
		}
		lua_pop(L, 1); return value;
	}
	Color color(int p_index) {
		int count = array(p_index, 4); require(count == 3 || count == 4, "color needs three or four components");
		p_index = lua_absindex(L, p_index); Color value(1, 1, 1, 1);
		for (int i = 0; i < count; ++i) { lua_rawgeti(L, p_index, i + 1); value[i] = number(-1, 0, 1); lua_pop(L, 1); }
		return value;
	}
	Color effect_color_field(int p_index, const char *p_name, Color p_default) {
		field(p_index, p_name);
		Color value = lua_isnil(L, -1) ? p_default : color(-1);
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
	String string_field(int p_index, const char *p_name, const String &p_default = String()) {
		field(p_index, p_name); String value = lua_isnil(L, -1) ? p_default : string(-1); lua_pop(L, 1); return value;
	}
	size_t handle_id(int p_index) {
		auto *id = static_cast<size_t *>(lua_touserdatatagged(L, p_index, HANDLE_TAG));
		require(id && *id < resources.size(), "expected a Cooker asset handle");
		return *id;
	}
	Ref<Resource> handle(int p_index) { return resources[handle_id(p_index)]; }
	// A field is an Image of 1..4 float channels plus typed `veya_field`
	// metadata. Points use the same Image container (three RGBA rows per point)
	// so runtime consumers decode one bounded format. Returns nullptr on success.
	static const char *decode_field(const Ref<Image> &p_image, FieldView &r_view) {
		if (p_image.is_null() || p_image->is_empty() || !p_image->has_meta("veya_field")) return "field requires veya_field metadata";
		const Variant meta_value = p_image->get_meta("veya_field");
		if (meta_value.get_type() != Variant::DICTIONARY) return "invalid veya_field metadata";
		const Dictionary meta = meta_value;
		if (int(meta.get("schema", 0)) != 1 || String(meta.get("kind", "")) != "field") return "unsupported veya_field schema";
		const Array channels = meta.get("channels", Array()), origin = meta.get("origin", Array());
		const double cell = meta.get("cell", 0.0);
		static const Image::Format formats[] = {Image::FORMAT_RF, Image::FORMAT_RGF, Image::FORMAT_RGBF, Image::FORMAT_RGBAF};
		if (channels.is_empty() || channels.size() > 4 || p_image->get_format() != formats[channels.size() - 1]) return "field channel format mismatch";
		if (origin.size() != 2 || !std::isfinite(cell) || cell < .05 || cell > 64) return "invalid field placement";
		const int width = p_image->get_width(), height = p_image->get_height();
		if (width < 2 || height < 2 || width > 1024 || height > 1024 || p_image->has_mipmaps()) return "field dimensions out of range";
		const Vector<uint8_t> bytes = p_image->get_data();
		const size_t count = size_t(width) * height * channels.size();
		if (size_t(bytes.size()) != count * sizeof(float)) return "field data size mismatch";
		r_view.width = width; r_view.height = height; r_view.channels = channels.size();
		r_view.origin_x = origin[0]; r_view.origin_z = origin[1]; r_view.cell = cell;
		r_view.data.resize(count);
		std::memcpy(r_view.data.data(), bytes.ptr(), count * sizeof(float));
		for (float value : r_view.data) if (!std::isfinite(value)) return "field data must be finite";
		return nullptr;
	}
	Array name_list(int p_index, const char *p_name, int p_max, bool p_required) {
		field(p_index, p_name); Array names;
		if (lua_isnil(L, -1)) { require(!p_required, "name list is required"); lua_pop(L, 1); return names; }
		const int count = array(-1, p_max); require(!p_required || count > 0, "name list must not be empty");
		for (int i = 0; i < count; ++i) {
			lua_rawgeti(L, -1, i + 1); const String name = string(-1); lua_pop(L, 1);
			require(!name.is_empty() && name.length() <= 64 && name.is_valid_ascii_identifier() && !names.has(name), "names must be unique ASCII identifiers");
			names.push_back(name);
		}
		lua_pop(L, 1); return names;
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
		Ref<Image> image = p_resource;
		if (image.is_valid()) {
			const Dictionary meta = image->get_meta("veya_field", Dictionary());
			if (String(meta.get("kind", "")) == "field") {
				FieldView view; const char *error = decode_field(image, view);
				require(error == nullptr, error ? error : "");
				field_views.emplace(id, std::move(view));
			}
		}
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
	static void assign_scene_ids(Node *p_node, Node *p_owner, int32_t &r_next_id) {
		// PackedScene::pack otherwise asks ResourceUID for random node IDs. Assign
		// stable preorder IDs to the nodes owned by this generated scene so an
		// identical recipe/seed serializes byte-for-byte identically.
		if (p_node == p_owner || p_node->get_owner() == p_owner) {
			p_node->set_unique_scene_id(r_next_id++);
		}
		for (int i = 0; i < p_node->get_child_count(); ++i) {
			assign_scene_ids(p_node->get_child(i), p_owner, r_next_id);
		}
	}
	static int random(lua_State *p_state) {
		auto &s = self(p_state); s.arity(0);
		uint32_t x = s.random_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s.random_state = x;
		lua_pushnumber(p_state, double(x) / 4294967296.0); return 1;
	}
	Ref<Texture2D> texture_field(int p_index, const char *p_name) {
		field(p_index, p_name);
		Ref<Texture2D> texture;
		if (!lua_isnil(L, -1)) {
			texture = handle(-1);
			require(texture.is_valid(), "material texture must be a Texture2D handle");
		}
		lua_pop(L, 1);
		return texture;
	}
	static int material(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"color", "roughness", "metallic", "albedo_texture", "normal_texture", "orm_texture",
				"emission_texture", "heightmap_texture", "uv_scale", "triplanar", "normal_scale", "heightmap_scale", "transparency"});
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
		if (const Ref<Texture2D> albedo = s.texture_field(1, "albedo_texture"); albedo.is_valid()) {
			material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, albedo);
		}
		if (const Ref<Texture2D> normal = s.texture_field(1, "normal_texture"); normal.is_valid()) {
			material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
			material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, normal);
			material->set_normal_scale(s.number_field(1, "normal_scale", 1, 0, 4));
		}
		if (const Ref<Texture2D> orm = s.texture_field(1, "orm_texture"); orm.is_valid()) {
			material->set_feature(BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION, true);
			material->set_texture(BaseMaterial3D::TEXTURE_AMBIENT_OCCLUSION, orm);
			material->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, orm);
			material->set_texture(BaseMaterial3D::TEXTURE_METALLIC, orm);
			material->set_ao_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_RED);
			material->set_roughness_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_GREEN);
			material->set_metallic_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_BLUE);
			material->set_ao_light_affect(1);
		}
		if (const Ref<Texture2D> emission = s.texture_field(1, "emission_texture"); emission.is_valid()) {
			material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
			material->set_texture(BaseMaterial3D::TEXTURE_EMISSION, emission);
		}
		if (const Ref<Texture2D> height = s.texture_field(1, "heightmap_texture"); height.is_valid()) {
			material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
			material->set_texture(BaseMaterial3D::TEXTURE_HEIGHTMAP, height);
			material->set_heightmap_scale(s.number_field(1, "heightmap_scale", 1, 0, 16));
		}
		const float uv_scale = s.number_field(1, "uv_scale", 1, 0.1, 64);
		material->set_uv1_scale(Vector3(uv_scale, uv_scale, uv_scale));
		material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, s.boolean_field(1, "triplanar", false));
		const String transparency = s.string_field(1, "transparency", "disabled");
		s.require(transparency == "disabled" || transparency == "alpha", "transparency must be disabled or alpha");
		material->set_transparency(transparency == "alpha" ? BaseMaterial3D::TRANSPARENCY_ALPHA : BaseMaterial3D::TRANSPARENCY_DISABLED);
		return s.push_resource(material);
	}
	static uint32_t texture_hash(uint32_t p_seed, int p_x, int p_y) {
		uint32_t value = p_seed ^ (uint32_t(p_x) * 0x9e3779b9U) ^ (uint32_t(p_y) * 0x85ebca6bU);
		value ^= value >> 16; value *= 0x7feb352dU; value ^= value >> 15; value *= 0x846ca68bU; value ^= value >> 16;
		return value;
	}
	static float texture_noise(uint32_t p_seed, float p_x, float p_y) {
		const int x0 = int(std::floor(p_x)), y0 = int(std::floor(p_y));
		const float tx0 = p_x - x0, ty0 = p_y - y0;
		const float tx = tx0 * tx0 * (3.0f - 2.0f * tx0), ty = ty0 * ty0 * (3.0f - 2.0f * ty0);
		auto value = [p_seed](int x, int y) { return float(texture_hash(p_seed, x, y) & 0xffffU) / 65535.0f; };
		const float a = Math::lerp(value(x0, y0), value(x0 + 1, y0), tx);
		const float b = Math::lerp(value(x0, y0 + 1), value(x0 + 1, y0 + 1), tx);
		return Math::lerp(a, b, ty);
	}
	static int procedural_texture(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"kind", "size", "seed", "color", "secondary", "radius", "width", "softness", "angle", "noise_scale"});
		const String kind = s.string_field(1, "kind");
		s.require(kind == "radial" || kind == "ring" || kind == "spark" || kind == "streak" || kind == "soft-noise", "unknown procedural texture kind");
		const double size_number = s.number_field(1, "size", 128, 32, 1024);
		const int size = int(size_number);
		s.require(size_number == size && (size & (size - 1)) == 0, "texture size must be a power of two from 32 to 1024");
		const double seed_number = s.number_field(1, "seed", s.seed_value, 0, UINT32_MAX);
		s.require(seed_number == std::floor(seed_number), "texture seed must be a uint32");
		const uint32_t seed = uint32_t(seed_number);
		const Color primary = s.effect_color_field(1, "color", Color(1, 1, 1, 1));
		const Color secondary = s.effect_color_field(1, "secondary", Color(primary.r, primary.g, primary.b, 0));
		const float radius = s.number_field(1, "radius", kind == "ring" ? 0.32 : 0.0, 0, 0.7);
		const float width = s.number_field(1, "width", kind == "streak" ? 0.08 : 0.12, 0.005, 1);
		const float softness = s.number_field(1, "softness", 0.45, 0.01, 1);
		const float angle = Math::deg_to_rad(s.number_field(1, "angle", 0, -360, 360));
		const float noise_scale = s.number_field(1, "noise_scale", 5, 0.25, 32);
		Ref<Image> image = Image::create_empty(size, size, false, Image::FORMAT_RGBA8);
		s.require(image.is_valid(), "could not allocate procedural texture");
		const float cs = std::cos(angle), sn = std::sin(angle);
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				const float nx = (float(x) + 0.5f) / size - 0.5f, ny = (float(y) + 0.5f) / size - 0.5f;
				const float rx = nx * cs + ny * sn, ry = -nx * sn + ny * cs;
				const float distance = std::sqrt(nx * nx + ny * ny);
				float alpha = 0;
				if (kind == "radial") alpha = 1.0f - Math::smoothstep(radius, MAX(radius + width, 0.001f), distance);
				else if (kind == "ring") alpha = 1.0f - Math::smoothstep(width * (1.0f - softness), width, std::abs(distance - radius));
				else if (kind == "spark") {
					const float horizontal = 1.0f - Math::smoothstep(width * 0.25f, width, std::abs(ry));
					const float vertical = 1.0f - Math::smoothstep(width * 0.12f, width * 0.55f, std::abs(rx));
					alpha = MAX(horizontal * Math::smoothstep(0.55f, 0.0f, std::abs(rx)), vertical * Math::smoothstep(0.55f, 0.0f, std::abs(ry)));
					alpha = MAX(alpha, 1.0f - Math::smoothstep(0.0f, width * 1.5f, distance));
				} else if (kind == "streak") {
					alpha = (1.0f - Math::smoothstep(width * (1.0f - softness), width, std::abs(ry))) * (1.0f - Math::smoothstep(0.28f, 0.5f, std::abs(rx)));
				} else {
					const float noise = texture_noise(seed, (nx + 0.5f) * noise_scale, (ny + 0.5f) * noise_scale);
					const float envelope = 1.0f - Math::smoothstep(0.34f, 0.7f, distance);
					alpha = Math::smoothstep(0.22f, 0.82f, noise) * envelope;
				}
				alpha = CLAMP(alpha, 0.0f, 1.0f);
				Color pixel = secondary.lerp(primary, alpha);
				pixel.a *= alpha;
				image->set_pixel(x, y, pixel);
			}
		}
		Ref<PortableCompressedTexture2D> texture; texture.instantiate();
		// Packed effect scenes embed this resource. Preserve the compressed payload so
		// ResourceSaver can serialize it into the .scn instead of retaining only the
		// transient renderer texture created by create_from_image().
		texture->set_keep_compressed_buffer(true);
		texture->create_from_image(image, PortableCompressedTexture2D::COMPRESSION_MODE_LOSSLESS);
		return s.push_resource(texture);
	}
	Ref<Texture2D> effect_texture_field(int p_index, const char *p_name, bool p_required) {
		field(p_index, p_name);
		Ref<Texture2D> texture;
		if (!lua_isnil(L, -1)) texture = handle(-1);
		lua_pop(L, 1);
		require(!p_required || texture.is_valid(), "effect layer requires a Texture2D handle");
		return texture;
	}
	Ref<GradientTexture1D> effect_gradient_field(int p_index, const char *p_name) {
		field(p_index, p_name);
		if (lua_isnil(L, -1)) { lua_pop(L, 1); return Ref<GradientTexture1D>(); }
		const int field_index = lua_absindex(L, -1), count = array(field_index, 8);
		require(count >= 2, "effect gradient requires two to eight stops");
		Ref<Gradient> gradient; gradient.instantiate();
		float previous = -1;
		for (int i = 0; i < count; ++i) {
			lua_rawgeti(L, field_index, i + 1); const int stop = lua_absindex(L, -1);
			require(array(stop, 2) == 2, "gradient stop must be {offset, color}");
			lua_rawgeti(L, stop, 1); const float offset = number(-1, 0, 1); lua_pop(L, 1);
			lua_rawgeti(L, stop, 2); const Color value = color(-1); lua_pop(L, 1);
			require(offset > previous, "gradient offsets must be strictly increasing"); previous = offset;
			if (i < 2) { gradient->set_offset(i, offset); gradient->set_color(i, value); }
			else gradient->add_point(offset, value);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		Ref<GradientTexture1D> texture; texture.instantiate(); texture->set_width(128); texture->set_gradient(gradient);
		return texture;
	}
	Ref<CurveTexture> effect_curve_field(int p_index, const char *p_name, float p_max_value) {
		field(p_index, p_name);
		if (lua_isnil(L, -1)) { lua_pop(L, 1); return Ref<CurveTexture>(); }
		const int field_index = lua_absindex(L, -1), count = array(field_index, 8);
		require(count >= 2, "effect curve requires two to eight points");
		Ref<Curve> curve; curve.instantiate(); curve->set_min_value(0); curve->set_max_value(p_max_value);
		float previous = -1;
		for (int i = 0; i < count; ++i) {
			lua_rawgeti(L, field_index, i + 1); const int point = lua_absindex(L, -1);
			require(array(point, 2) == 2, "curve point must be {time, value}");
			lua_rawgeti(L, point, 1); const float time = number(-1, 0, 1); lua_pop(L, 1);
			lua_rawgeti(L, point, 2); const float value = number(-1, 0, p_max_value); lua_pop(L, 1);
			require(time > previous, "curve times must be strictly increasing"); previous = time;
			curve->add_point(Vector2(time, value)); lua_pop(L, 1);
		}
		lua_pop(L, 1);
		Ref<CurveTexture> texture; texture.instantiate(); texture->set_width(128); texture->set_curve(curve);
		return texture;
	}
	Ref<StandardMaterial3D> effect_material(int p_index, const Ref<Texture2D> &p_texture, bool p_particle) {
		Ref<StandardMaterial3D> material; material.instantiate();
		const Color color_value = effect_color_field(p_index, "color", Color(1, 1, 1, 1));
		const String blend = string_field(p_index, "blend", "add");
		require(blend == "mix" || blend == "add" || blend == "multiply", "effect blend must be mix, add or multiply");
		material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		material->set_blend_mode(blend == "mix" ? BaseMaterial3D::BLEND_MODE_MIX : blend == "add" ? BaseMaterial3D::BLEND_MODE_ADD : BaseMaterial3D::BLEND_MODE_MUL);
		material->set_albedo(color_value);
		material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
		material->set_emission(Color(color_value.r, color_value.g, color_value.b));
		material->set_emission_energy_multiplier(number_field(p_index, "emission", 1.6, 0, 8));
		material->set_flag(BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS, true);
		material->set_flag(BaseMaterial3D::FLAG_DISABLE_AMBIENT_LIGHT, true);
		material->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
		if (p_texture.is_valid()) material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, p_texture);
		const String billboard = string_field(p_index, "billboard", p_particle ? "particles" : "camera");
		require(billboard == "particles" || billboard == "camera" || billboard == "fixed_y" || billboard == "none", "unknown effect billboard mode");
		material->set_billboard_mode(billboard == "particles" ? BaseMaterial3D::BILLBOARD_PARTICLES : billboard == "camera" ? BaseMaterial3D::BILLBOARD_ENABLED : billboard == "fixed_y" ? BaseMaterial3D::BILLBOARD_FIXED_Y : BaseMaterial3D::BILLBOARD_DISABLED);
		material->set_flag(BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE, billboard != "none");
		return material;
	}
	static int effect(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"name", "mode", "duration", "layers"});
		const String name = s.string_field(1, "name", "CookedEffect");
		s.require(!name.is_empty() && name.validate_node_name() == name, "effect name must be a valid Node name");
		const String mode = s.string_field(1, "mode", "one_shot");
		s.require(mode == "one_shot" || mode == "loop", "effect mode must be one_shot or loop");
		const float duration = s.number_field(1, "duration", mode == "one_shot" ? 1.0 : 4.0, 0.05, mode == "one_shot" ? 5.0 : 10.0);
		s.field(1, "layers"); const int layers_index = lua_absindex(p_state, -1), layer_count = s.array(layers_index, 4);
		s.require(layer_count > 0, "effect requires one to four layers");
		OwnedNode root(memnew(Node3D)); root->set_name(name);
		Ref<Animation> animation;
		std::vector<ObjectID> texture_ids;
		auto register_texture = [&s, &texture_ids](const Ref<Texture2D> &p_texture) {
			if (p_texture.is_null()) return;
			const ObjectID id = p_texture->get_instance_id();
			bool known = false; for (ObjectID current : texture_ids) known |= current == id;
			if (!known) texture_ids.push_back(id);
			s.require(texture_ids.size() <= 4, "effect uses more than four textures or curve maps");
		};
		int particle_systems = 0, particle_count = 0, node_count = 1;
		for (int i = 0; i < layer_count; ++i) {
			lua_rawgeti(p_state, layers_index, i + 1); const int layer = lua_absindex(p_state, -1);
			s.fields(layer, {"type", "name", "texture", "mesh", "position", "rotation", "scale", "spin", "color", "blend", "emission", "billboard", "size", "amount", "lifetime", "preprocess", "explosiveness", "randomness", "local_coords", "visibility_aabb", "visibility_distance", "direction", "spread", "velocity", "gravity", "damping", "rotation_range", "emission_shape", "emission_radius", "emission_extents", "ring_inner_radius", "color_gradient", "alpha_curve", "scale_curve", "start_scale", "peak_scale", "end_scale", "start_alpha", "peak_alpha", "end_alpha"});
			const String type = s.string_field(layer, "type");
			s.require(type == "billboard_particles" || type == "animated_sprite" || type == "simple_mesh", "unknown effect layer type");
			String layer_name = s.string_field(layer, "name", "Layer" + itos(i + 1));
			s.require(!layer_name.is_empty() && layer_name.validate_node_name() == layer_name, "effect layer name must be a valid Node name");
			Ref<Texture2D> texture = s.effect_texture_field(layer, "texture", type != "simple_mesh");
			register_texture(texture);
			const Vector3 position = s.vector_field(layer, "position", Vector3());
			const Vector3 rotation = s.vector_field(layer, "rotation", Vector3());
			const Vector3 scale = s.vector_field(layer, "scale", Vector3(1, 1, 1));
			s.require(scale.x > 0 && scale.y > 0 && scale.z > 0, "effect layer scale must be positive");
			const float visibility_distance = s.number_field(layer, "visibility_distance", 80, 0.1, 10000);
			if (type == "billboard_particles") {
				const double amount_number = s.number_field(layer, "amount", 32, 1, 512); const int amount = int(amount_number);
				s.require(amount_number == amount, "particle amount must be an integer");
				particle_count += amount; particle_systems += 1;
				s.require(particle_count <= 1024 && particle_systems <= 4, "effect particle budget exceeded");
				const float lifetime = s.number_field(layer, "lifetime", duration, 0.05, mode == "one_shot" ? 5.0 : 10.0);
				Ref<ParticleProcessMaterial> process; process.instantiate();
				Vector3 direction = s.vector_field(layer, "direction", Vector3(0, 1, 0));
				s.require(direction.length_squared() > 0.000001, "particle direction must be nonzero"); process->set_direction(direction.normalized());
				process->set_spread(s.number_field(layer, "spread", 45, 0, 180));
				const Vector2 velocity = s.vector2_field(layer, "velocity", Vector2(0.2, 1));
				s.require(velocity.x >= 0 && velocity.y >= velocity.x && velocity.y <= 100, "particle velocity range is invalid");
				process->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, velocity.x); process->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, velocity.y);
				process->set_gravity(s.vector_field(layer, "gravity", Vector3()));
				const Vector2 damping = s.vector2_field(layer, "damping", Vector2());
				s.require(damping.x >= 0 && damping.y >= damping.x && damping.y <= 100, "particle damping range is invalid");
				process->set_param_min(ParticleProcessMaterial::PARAM_DAMPING, damping.x); process->set_param_max(ParticleProcessMaterial::PARAM_DAMPING, damping.y);
				const Vector2 rotation_range = s.vector2_field(layer, "rotation_range", Vector2(-180, 180));
				s.require(rotation_range.y >= rotation_range.x, "particle rotation range is invalid");
				process->set_param_min(ParticleProcessMaterial::PARAM_ANGLE, rotation_range.x); process->set_param_max(ParticleProcessMaterial::PARAM_ANGLE, rotation_range.y);
				const String shape = s.string_field(layer, "emission_shape", "point");
				if (shape == "point") process->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_POINT);
				else if (shape == "sphere") { process->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_SPHERE); process->set_emission_sphere_radius(s.number_field(layer, "emission_radius", 0.5, 0.001, 100)); }
				else if (shape == "sphere_surface") { process->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_SPHERE_SURFACE); process->set_emission_sphere_radius(s.number_field(layer, "emission_radius", 0.5, 0.001, 100)); }
				else if (shape == "box") { process->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_BOX); Vector3 extents = s.vector_field(layer, "emission_extents", Vector3(0.5, 0.5, 0.5)); s.require(extents.x > 0 && extents.y > 0 && extents.z > 0, "emission extents must be positive"); process->set_emission_box_extents(extents); }
				else if (shape == "ring") { const float radius = s.number_field(layer, "emission_radius", 0.5, 0.001, 100); const float inner = s.number_field(layer, "ring_inner_radius", 0.35, 0, radius); process->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_RING); process->set_emission_ring_axis(Vector3(0, 1, 0)); process->set_emission_ring_radius(radius); process->set_emission_ring_inner_radius(inner); process->set_emission_ring_height(0); }
				else s.require(false, "unknown particle emission shape");
				Ref<GradientTexture1D> gradient = s.effect_gradient_field(layer, "color_gradient"); if (gradient.is_valid()) process->set_color_ramp(gradient);
				Ref<CurveTexture> alpha = s.effect_curve_field(layer, "alpha_curve", 1); if (alpha.is_valid()) process->set_alpha_curve(alpha);
				Ref<CurveTexture> scale_curve = s.effect_curve_field(layer, "scale_curve", 4); if (scale_curve.is_valid()) process->set_param_texture(ParticleProcessMaterial::PARAM_SCALE, scale_curve);
				Ref<QuadMesh> quad; quad.instantiate(); quad->set_size(s.vector2_field(layer, "size", Vector2(0.15, 0.15))); quad->set_material(s.effect_material(layer, texture, true));
				auto *particles = memnew(GPUParticles3D); particles->set_name(layer_name); particles->set_position(position); particles->set_rotation(rotation); particles->set_scale(scale);
				particles->set_amount(amount); particles->set_lifetime(lifetime); particles->set_one_shot(mode == "one_shot"); particles->set_emitting(true);
				particles->set_pre_process_time(s.number_field(layer, "preprocess", mode == "loop" ? MIN(lifetime, 2.0f) : 0, 0, 10)); particles->set_explosiveness_ratio(s.number_field(layer, "explosiveness", mode == "one_shot" ? 0.9 : 0, 0, 1)); particles->set_randomness_ratio(s.number_field(layer, "randomness", 0.35, 0, 1));
				particles->set_use_local_coordinates(s.boolean_field(layer, "local_coords", false)); particles->set_process_material(process); particles->set_draw_passes(1); particles->set_draw_pass_mesh(0, quad); particles->set_fixed_fps(30); particles->set_fractional_delta(true); particles->set_use_fixed_seed(true); particles->set_seed(s.seed_value + uint32_t(i) * 2654435761U); particles->set_visibility_range_end(visibility_distance);
				s.field(layer, "visibility_aabb");
				if (!lua_isnil(p_state, -1)) { const int box = lua_absindex(p_state, -1); s.fields(box, {"position", "size"}); const Vector3 box_position = s.vector_field(box, "position", Vector3(-2, -2, -2)); const Vector3 box_size = s.vector_field(box, "size", Vector3(4, 4, 4)); s.require(box_size.x > 0 && box_size.y > 0 && box_size.z > 0, "visibility AABB size must be positive"); particles->set_visibility_aabb(AABB(box_position, box_size)); }
				else particles->set_visibility_aabb(AABB(Vector3(-4, -4, -4), Vector3(8, 8, 8)));
				lua_pop(p_state, 1); particles->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
				root->add_child(particles); particles->set_owner(root.get()); node_count += 1;
			} else if (type == "animated_sprite") {
				Ref<QuadMesh> quad; quad.instantiate(); quad->set_size(s.vector2_field(layer, "size", Vector2(0.5, 0.5))); quad->set_material(s.effect_material(layer, texture, false));
				auto *sprite = memnew(MeshInstance3D); sprite->set_name(layer_name); sprite->set_mesh(quad); sprite->set_position(position); sprite->set_rotation(rotation); sprite->set_scale(scale); sprite->set_visibility_range_end(visibility_distance); sprite->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
				root->add_child(sprite); sprite->set_owner(root.get()); node_count += 1;
				if (animation.is_null()) { animation.instantiate(); animation->set_length(duration); animation->set_loop_mode(mode == "loop" ? Animation::LOOP_LINEAR : Animation::LOOP_NONE); }
				const float start_scale = s.number_field(layer, "start_scale", 0.05, 0, 8), peak_scale = s.number_field(layer, "peak_scale", 1, 0, 8), end_scale = s.number_field(layer, "end_scale", mode == "loop" ? start_scale : 1.25, 0, 8);
				const float start_alpha = s.number_field(layer, "start_alpha", 0, 0, 1), peak_alpha = s.number_field(layer, "peak_alpha", 1, 0, 1), end_alpha = s.number_field(layer, "end_alpha", mode == "loop" ? start_alpha : 0, 0, 1);
				int track = animation->add_track(Animation::TYPE_SCALE_3D); animation->track_set_path(track, NodePath(layer_name)); animation->track_set_interpolation_type(track, Animation::INTERPOLATION_CUBIC); animation->track_insert_key(track, 0, scale * start_scale); animation->track_insert_key(track, duration * 0.35, scale * peak_scale); animation->track_insert_key(track, duration, scale * end_scale);
				track = animation->add_track(Animation::TYPE_VALUE); animation->track_set_path(track, NodePath(layer_name + ":transparency")); animation->track_set_interpolation_type(track, Animation::INTERPOLATION_CUBIC); animation->track_insert_key(track, 0, 1.0f - start_alpha); animation->track_insert_key(track, duration * 0.35, 1.0f - peak_alpha); animation->track_insert_key(track, duration, 1.0f - end_alpha);
				const Vector3 spin = s.vector_field(layer, "spin", Vector3());
				if (!spin.is_zero_approx()) { track = animation->add_track(Animation::TYPE_VALUE); animation->track_set_path(track, NodePath(layer_name + ":rotation")); animation->track_set_interpolation_type(track, Animation::INTERPOLATION_LINEAR_ANGLE); animation->track_insert_key(track, 0, rotation); animation->track_insert_key(track, duration, rotation + spin * Math::TAU); }
			} else {
				s.field(layer, "mesh"); Ref<Mesh> mesh = s.handle(-1); lua_pop(p_state, 1); s.require(mesh.is_valid(), "simple_mesh requires a Mesh handle");
				auto *instance = memnew(MeshInstance3D); instance->set_name(layer_name); instance->set_mesh(mesh); instance->set_material_override(s.effect_material(layer, texture, false)); instance->set_position(position); instance->set_rotation(rotation); instance->set_scale(scale); instance->set_visibility_range_end(visibility_distance); instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
				root->add_child(instance); instance->set_owner(root.get()); node_count += 1;
			}
			lua_pop(p_state, 1);
		}
		lua_pop(p_state, 1);
		if (animation.is_valid()) {
			Ref<AnimationLibrary> library; library.instantiate(); s.require(library->add_animation("effect", animation) == OK, "could not create effect animation");
			auto *player = memnew(AnimationPlayer); player->set_name("AnimationPlayer"); player->add_animation_library("", library); player->set_root_node(NodePath("..")); player->set_autoplay("effect"); root->add_child(player); player->set_owner(root.get()); node_count += 1;
		}
		s.require(node_count <= 32, "effect node budget exceeded");
		int32_t next_scene_id = 1;
		assign_scene_ids(root.get(), root.get(), next_scene_id);
		Ref<PackedScene> packed; packed.instantiate(); s.require(packed->pack(root.get()) == OK, "could not pack effect scene");
		s.effect_layers = MAX(s.effect_layers, layer_count); s.effect_particle_systems = MAX(s.effect_particle_systems, particle_systems); s.effect_particles = MAX(s.effect_particles, particle_count); s.effect_textures = MAX(s.effect_textures, int(texture_ids.size()));
		return s.push_resource(packed);
	}
	static int atmosphere(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"shader", "cloud_texture", "preset_id", "family", "zenith", "horizon", "cloud_color", "cloud_shadow", "ambient_color", "fog_color", "sun_color", "sun_direction", "cloud_coverage", "cloud_scale", "cloud_softness", "cloud_seed", "cloud_speed", "cloud_wind", "ambient_energy", "sky_contribution", "fog_density", "fog_sky_affect", "exposure", "sun_size", "sun_energy", "fog_height", "fog_height_density", "fog_aerial_perspective", "fog_sun_scatter", "volumetric_density", "volumetric_length", "volumetric_albedo", "volumetric_anisotropy", "volumetric_ambient_inject", "ssao_intensity", "ssao_radius", "ssil_intensity", "ssil_radius", "glow_intensity", "glow_threshold", "contrast", "saturation", "brightness", "ssr_enabled", "sdfgi_enabled", "sdfgi_cascades", "sdfgi_cell_size", "sdfgi_occlusion", "sdfgi_read_sky", "sdfgi_bounce", "sdfgi_energy", "sdfgi_normal_bias", "sdfgi_probe_bias"});
		s.field(1, "preset_id"); const String preset_id = s.string(-1); lua_pop(p_state, 1);
		s.require(!preset_id.is_empty() && preset_id.length() <= 48, "invalid atmosphere preset_id");
		s.field(1, "shader"); Ref<Shader> shader = s.handle(-1); lua_pop(p_state, 1);
		s.require(shader.is_valid() && shader->get_mode() == Shader::MODE_SKY, "atmosphere requires a sky Shader input");
		Ref<Texture2D> cloud_texture;
		s.field(1, "cloud_texture");
		if (!lua_isnil(p_state, -1)) { cloud_texture = s.handle(-1); s.require(cloud_texture.is_valid(), "cloud_texture requires a Texture2D input"); }
		lua_pop(p_state, 1);
		const int family = int(s.number_field(1, "family", 0, 0, 3));
		s.require(double(family) == s.number_field(1, "family", 0, 0, 3), "family must be an integer");
		Vector3 sun_direction = s.vector_field(1, "sun_direction", Vector3(0.4, 0.7, 0.5));
		s.require(sun_direction.length_squared() > 0.001, "sun_direction must be nonzero");
		sun_direction.normalize();
		Ref<ShaderMaterial> material; material.instantiate(); material->set_shader(shader);
		material->set_shader_parameter("family", family);
		material->set_shader_parameter("zenith", s.color_field(1, "zenith", Color(0.14, 0.39, 0.77)));
		material->set_shader_parameter("horizon", s.color_field(1, "horizon", Color(0.73, 0.85, 0.98)));
		material->set_shader_parameter("cloud_color", s.color_field(1, "cloud_color", Color(1, 1, 1)));
		material->set_shader_parameter("cloud_shadow", s.color_field(1, "cloud_shadow", Color(0.51, 0.63, 0.77)));
		const Color sun_color = s.color_field(1, "sun_color", Color(1, 0.9, 0.72));
		const double sun_energy = s.number_field(1, "sun_energy", 0.5, 0, 8);
		material->set_shader_parameter("sun_color", sun_color);
		material->set_shader_parameter("sun_direction", sun_direction);
		material->set_shader_parameter("cloud_coverage", s.number_field(1, "cloud_coverage", 0.4, 0, 1));
		material->set_shader_parameter("cloud_scale", s.number_field(1, "cloud_scale", 2, 0.25, 8));
		material->set_shader_parameter("cloud_softness", s.number_field(1, "cloud_softness", 0.25, 0.02, 1));
		material->set_shader_parameter("cloud_seed", s.number_field(1, "cloud_seed", 1, 0, 10000));
		const double cloud_speed = s.number_field(1, "cloud_speed", 0, 0, 0.05);
		Vector3 cloud_wind = s.vector_field(1, "cloud_wind", Vector3(0.8, 0, 0.6));
		s.require(Math::is_zero_approx(cloud_wind.y) && cloud_wind.length_squared() > 0.001, "cloud_wind must be a nonzero horizontal direction");
		cloud_wind.y = 0;
		material->set_shader_parameter("cloud_drift", cloud_wind.normalized() * cloud_speed);
		material->set_shader_parameter("sun_size", s.number_field(1, "sun_size", 0.025, 0.001, 0.15));
		material->set_shader_parameter("sun_energy", sun_energy);
		material->set_shader_parameter("has_cloud_texture", cloud_texture.is_valid());
		if (cloud_texture.is_valid()) material->set_shader_parameter("cloud_texture", cloud_texture);
		// TIME shaders select the realtime filter; static shaders retain the
		// incremental/quality path. Radiance size does not limit the visible sky.
		Ref<Sky> sky; sky.instantiate(); sky->set_material(material); sky->set_process_mode(Sky::PROCESS_MODE_AUTOMATIC); sky->set_radiance_size(Sky::RADIANCE_SIZE_256);
		Ref<Environment> environment; environment.instantiate();
		environment->set_background(Environment::BG_SKY); environment->set_sky(sky);
		environment->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
		environment->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);
		environment->set_ambient_light_color(s.color_field(1, "ambient_color", Color(0.55, 0.65, 0.78)));
		environment->set_ambient_light_energy(s.number_field(1, "ambient_energy", 1, 0, 8));
		environment->set_ambient_light_sky_contribution(s.number_field(1, "sky_contribution", 0.6, 0, 0.95));
		const double fog_density = s.number_field(1, "fog_density", 0, 0, 0.1);
		environment->set_fog_enabled(fog_density > 0);
		environment->set_fog_density(fog_density);
		environment->set_fog_light_color(s.color_field(1, "fog_color", Color(0.73, 0.79, 0.87)));
		environment->set_fog_sky_affect(s.number_field(1, "fog_sky_affect", 0.04, 0, 1));
		// Optional authored scene treatment. Defaults preserve existing profiles;
		// all visual decisions remain in the bounded project-local Luau recipe.
		environment->set_fog_height(s.number_field(1, "fog_height", 0, -1000, 1000));
		environment->set_fog_height_density(s.number_field(1, "fog_height_density", 0, 0, 1));
		environment->set_fog_aerial_perspective(s.number_field(1, "fog_aerial_perspective", 0, 0, 1));
		environment->set_fog_sun_scatter(s.number_field(1, "fog_sun_scatter", 0, 0, 1));
		const double volumetric_density = s.number_field(1, "volumetric_density", 0, 0, 0.05);
		environment->set_volumetric_fog_enabled(volumetric_density > 0);
		environment->set_volumetric_fog_density(volumetric_density);
		environment->set_volumetric_fog_length(s.number_field(1, "volumetric_length", 64, 16, 512));
		environment->set_volumetric_fog_albedo(s.color_field(1, "volumetric_albedo", Color(1, 1, 1)));
		environment->set_volumetric_fog_anisotropy(s.number_field(1, "volumetric_anisotropy", 0.2, -0.9, 0.9));
		environment->set_volumetric_fog_ambient_inject(s.number_field(1, "volumetric_ambient_inject", 0, 0, 1));
		environment->set_volumetric_fog_sky_affect(0);
		const double ssao_intensity = s.number_field(1, "ssao_intensity", 0, 0, 2);
		environment->set_ssao_enabled(ssao_intensity > 0);
		if (ssao_intensity > 0) {
			environment->set_ssao_intensity(ssao_intensity);
			environment->set_ssao_radius(s.number_field(1, "ssao_radius", 1, 0.1, 4));
		} else { s.number_field(1, "ssao_radius", 1, 0.1, 4); }
		const double ssil_intensity = s.number_field(1, "ssil_intensity", 0, 0, 2);
		environment->set_ssil_enabled(ssil_intensity > 0);
		if (ssil_intensity > 0) {
			environment->set_ssil_intensity(ssil_intensity);
			environment->set_ssil_radius(s.number_field(1, "ssil_radius", 4, 0.5, 16));
		} else { s.number_field(1, "ssil_radius", 4, 0.5, 16); }
		// SDFGI geometry is authored/static; dynamic light energy follows the clock.
		const double cascades = s.number_field(1, "sdfgi_cascades", 4, 4, 8);
		s.require(cascades == 4 || cascades == 6 || cascades == 8, "SDFGI cascades must be 4, 6 or 8");
		environment->set_sdfgi_cascades(int(cascades));
		environment->set_sdfgi_min_cell_size(s.number_field(1, "sdfgi_cell_size", 1, .25, 8));
		environment->set_sdfgi_use_occlusion(s.boolean_field(1, "sdfgi_occlusion", true));
		environment->set_sdfgi_read_sky_light(s.boolean_field(1, "sdfgi_read_sky", true));
		environment->set_sdfgi_bounce_feedback(s.number_field(1, "sdfgi_bounce", .3, 0, .5));
		environment->set_sdfgi_energy(s.number_field(1, "sdfgi_energy", 1, 0, 2));
		environment->set_sdfgi_normal_bias(s.number_field(1, "sdfgi_normal_bias", 1.1, .1, 4));
		environment->set_sdfgi_probe_bias(s.number_field(1, "sdfgi_probe_bias", 1.1, .1, 4));
		environment->set_sdfgi_enabled(s.boolean_field(1, "sdfgi_enabled", false));
		const double glow_intensity = s.number_field(1, "glow_intensity", 0, 0, 1);
		environment->set_glow_enabled(glow_intensity > 0);
		if (glow_intensity > 0) environment->set_glow_intensity(glow_intensity);
		environment->set_glow_hdr_bleed_threshold(s.number_field(1, "glow_threshold", 1, 1, 8));
		environment->set_tonemapper(Environment::TONE_MAPPER_AGX);
		environment->set_tonemap_exposure(s.number_field(1, "exposure", 1, 0.1, 4));
		const double contrast = s.number_field(1, "contrast", 1, .8, 1.25);
		const double saturation = s.number_field(1, "saturation", 1, .6, 1.3);
		const double brightness = s.number_field(1, "brightness", 1, .8, 1.2);
		environment->set_adjustment_enabled(contrast != 1 || saturation != 1 || brightness != 1);
		environment->set_adjustment_contrast(contrast);
		environment->set_adjustment_saturation(saturation);
		environment->set_adjustment_brightness(brightness);
		environment->set_ssr_enabled(s.boolean_field(1, "ssr_enabled", false));
		environment->set_meta("veya_atmosphere_preset", preset_id);
		environment->set_meta("veya_atmosphere_sun_direction", sun_direction);
		environment->set_meta("veya_atmosphere_sun_color", sun_color);
		environment->set_meta("veya_atmosphere_sun_energy", sun_energy);
		return s.push_resource(environment);
	}
	static int camera_attributes(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"preset_id", "distance", "shoulder_offset", "target_height", "zoom_smoothing", "fov", "run_fov", "fov_smoothing", "follow_smoothing", "shoulder_smoothing", "clip_near", "clip_far", "exposure_multiplier", "dof_far_enabled", "dof_far_distance", "dof_far_transition", "dof_near_enabled", "dof_near_distance", "dof_near_transition", "dof_amount"});
		const String id = s.string_field(1, "preset_id", "");
		s.require(!id.is_empty() && id.length() <= 48, "invalid camera preset_id");
		Dictionary profile;
		profile["distance"] = s.number_field(1, "distance", 4.8, 2.3, 8);
		profile["shoulder_offset"] = s.number_field(1, "shoulder_offset", .34, -1.2, 1.2);
		profile["target_height"] = s.number_field(1, "target_height", 1.48, .8, 2.2);
		profile["zoom_smoothing"] = s.number_field(1, "zoom_smoothing", 9, 1, 30);
		profile["fov"] = s.number_field(1, "fov", 62, 25, 100);
		profile["run_fov"] = s.number_field(1, "run_fov", 68, 25, 110);
		profile["fov_smoothing"] = s.number_field(1, "fov_smoothing", 5, .1, 30);
		profile["follow_smoothing"] = s.number_field(1, "follow_smoothing", 16, .1, 30);
		profile["shoulder_smoothing"] = s.number_field(1, "shoulder_smoothing", 12, .1, 30);
		profile["clip_near"] = s.number_field(1, "clip_near", .08, .02, 1);
		profile["clip_far"] = s.number_field(1, "clip_far", 1600, 16, 4000);
		Ref<CameraAttributesPractical> attributes;
		attributes.instantiate();
		attributes->set_exposure_multiplier(s.number_field(1, "exposure_multiplier", 1, .25, 4));
		attributes->set_auto_exposure_enabled(false);
		attributes->set_dof_blur_far_enabled(s.boolean_field(1, "dof_far_enabled", false));
		attributes->set_dof_blur_far_distance(s.number_field(1, "dof_far_distance", 10, .1, 256));
		attributes->set_dof_blur_far_transition(s.number_field(1, "dof_far_transition", 5, .1, 128));
		attributes->set_dof_blur_near_enabled(s.boolean_field(1, "dof_near_enabled", false));
		attributes->set_dof_blur_near_distance(s.number_field(1, "dof_near_distance", 1, .1, 16));
		attributes->set_dof_blur_near_transition(s.number_field(1, "dof_near_transition", 1, .1, 16));
		attributes->set_dof_blur_amount(s.number_field(1, "dof_amount", .1, 0, .3));
		attributes->set_meta("veya_camera_preset", id);
		attributes->set_meta("veya_camera_profile", profile);
		return s.push_resource(attributes);
	}
	static int cloud_mask(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		Ref<Texture2D> texture = s.handle(1);
		s.require(texture.is_valid(), "cloud_mask requires a Texture2D input");
		Ref<Image> source = texture->get_image();
		s.require(source.is_valid() && !source->is_empty() && source->get_width() == source->get_height() &&
			source->get_width() >= 256 && source->get_width() <= 2048, "cloud_mask requires a bounded square image");
		source->resize(1024, 1024, Image::INTERPOLATE_BILINEAR);
		source->convert(Image::FORMAT_RGBA8);
		constexpr int side = 1024;
		std::vector<float> luminance(side * side);
		for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
			const Color pixel = source->get_pixel(x, y);
			luminance[y * side + x] = CLAMP(pixel.r * 0.2126f + pixel.g * 0.7152f + pixel.b * 0.0722f, 0.0f, 1.0f);
		}
		Ref<Image> output = Image::create_empty(side, side, false, Image::FORMAT_RGBA8);
		for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
			const float x_edge = CLAMP(float(MIN(x, side - 1 - x)) / 64.0f, 0.0f, 1.0f);
			const float y_edge = CLAMP(float(MIN(y, side - 1 - y)) / 64.0f, 0.0f, 1.0f);
			const float horizontal = Math::lerp((luminance[y * side + x] + luminance[y * side + side - 1 - x]) * 0.5f,
				luminance[y * side + x], x_edge);
			const float opposite = Math::lerp((luminance[(side - 1 - y) * side + x] + luminance[(side - 1 - y) * side + side - 1 - x]) * 0.5f,
				luminance[(side - 1 - y) * side + x], x_edge);
			const float value = Math::lerp((horizontal + opposite) * 0.5f, horizontal, y_edge);
			output->set_pixel(x, y, Color(value, value, value, 1));
		}
		output->generate_mipmaps();
		Ref<ImageTexture> mask = ImageTexture::create_from_image(output);
		return s.push_resource(mask);
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
			s.fields(item, {"asset", "material", "position", "rotation", "scale", "basis", "name", "remove", "lightmap"});
			s.field(item, "asset"); Ref<Resource> asset = s.handle(-1); lua_pop(p_state, 1);
			s.field(item, "material"); Ref<Material> material;
			if (!lua_isnil(p_state, -1)) { material = s.handle(-1); s.require(material.is_valid(), "material requires a Material handle"); }
			lua_pop(p_state, 1);
			Transform3D transform = s.transform(item);
			Ref<Mesh> mesh = asset;
			if (mesh.is_valid()) {
				if (s.boolean_field(item, "lightmap", false)) {
					auto *node = memnew(MeshInstance3D);
					node->set_name(s.string_field(item, "name", "Mesh" + itos(i)));
					s.require(!node->get_name().is_empty(), "lightmap mesh requires a name");
					node->set_mesh(mesh); node->set_material_override(material); node->set_transform(transform);
					node->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
					root->add_child(node); node->set_owner(root.get());
					lua_pop(p_state, 1); continue;
				}
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
				// Recipe-created PackedScenes have no external scene to instance.
				// Own their descendants in the enclosing scene so nested spatial
				// MultiMesh clusters survive packing and subsequent serialization.
				bool localize = packed->get_path().is_empty();
				s.field(item, "remove");
				if (!lua_isnil(p_state, -1)) {
					int remove_count = s.array(-1, 128);
					localize = localize || remove_count > 0;
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
		int32_t next_scene_id = 1;
		assign_scene_ids(root.get(), root.get(), next_scene_id);
		Ref<PackedScene> scene; scene.instantiate();
		s.require(scene->pack(root.get()) == OK, "could not pack recipe scene");
		return s.push_resource(scene);
	}
	static int lightmap_scene(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"scene", "lights", "quality", "bounces", "directional", "interior", "probes", "max_texture_size", "texel_size", "indirect_only"});
		s.field(1, "scene"); Ref<PackedScene> source = s.handle(-1); lua_pop(p_state, 1);
		s.require(source.is_valid(), "lightmap_scene requires a PackedScene");
		OwnedNode root(source->instantiate());
		s.require(Object::cast_to<Node3D>(root.get()) != nullptr, "lightmap root must be Node3D");
		root->set_scene_file_path(String());
		for(int i=0;i<root->get_child_count();++i)make_scene_local(root->get_child(i),root.get());
		auto *gi = memnew(LightmapGI); gi->set_name("BakedGI");
		const double quality=s.number_field(1,"quality",2,0,3), probes=s.number_field(1,"probes",2,1,3);
		const double bounces=s.number_field(1,"bounces",3,1,8), size=s.number_field(1,"max_texture_size",2048,256,4096);
		s.require(quality==std::floor(quality) && probes==std::floor(probes) && bounces==std::floor(bounces) && size==std::floor(size) && (int(size)&(int(size)-1))==0,"lightmap settings require bounded integers and a power-of-two atlas");
		gi->set_bake_quality(LightmapGI::BakeQuality(int(quality))); gi->set_bounces(int(bounces));
		gi->set_directional(s.boolean_field(1,"directional",true)); gi->set_interior(s.boolean_field(1,"interior",true));
		gi->set_generate_probes(LightmapGI::GenerateProbes(int(probes))); gi->set_max_texture_size(int(size));
		gi->set_environment_mode(LightmapGI::ENVIRONMENT_MODE_DISABLED);
		const bool indirect_only=s.boolean_field(1,"indirect_only",true);
		gi->set_meta("veya_lightmap_indirect_only",indirect_only);
		gi->set_meta("_veya_cooker_texel_size",s.number_field(1,"texel_size",.15,.025,1));
		root->add_child(gi); gi->set_owner(root.get());
		s.field(1,"lights"); const int count=s.array(-1,32);
		for(int i=0;i<count;++i) {
			lua_rawgeti(p_state,-1,i+1); const int item=lua_gettop(p_state);
			s.fields(item,{"position","color","energy","range","size"});
			auto *light=memnew(OmniLight3D); light->set_name("BakeLamp"+itos(i));
			light->set_position(s.vector_field(item,"position",Vector3()));
			light->set_color(s.color_field(item,"color",Color(1,.82,.6)));
			light->set_param(Light3D::PARAM_ENERGY,s.number_field(item,"energy",2,0,16));
			light->set_param(Light3D::PARAM_RANGE,s.number_field(item,"range",10,.1,128));
			light->set_param(Light3D::PARAM_SIZE,s.number_field(item,"size",.15,0,2));
			light->set_bake_mode(indirect_only?Light3D::BAKE_DYNAMIC:Light3D::BAKE_STATIC); light->set_shadow(true);
			light->set_meta("_veya_cooker_bake_only",true);
			root->add_child(light); light->set_owner(root.get()); lua_pop(p_state,1);
		}
		lua_pop(p_state,1);
		int32_t next_scene_id=1; assign_scene_ids(root.get(),root.get(),next_scene_id);
		Ref<PackedScene> packed; packed.instantiate(); s.require(packed->pack(root.get())==OK,"cannot pack lightmap scene");
		return s.push_resource(packed);
	}
	// cooker.field({channels={...},width=,height=,origin={x,z},cell=,data={...},labels={...}})
	// Row-major (z rows, x columns), channel-interleaved float raster.
	static int field_raster(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"channels", "width", "height", "origin", "cell", "data", "labels"});
		const Array channels = s.name_list(1, "channels", 4, true);
		const Array labels = s.name_list(1, "labels", 64, false);
		const double width = s.number_field(1, "width", 0, 2, 1024), height = s.number_field(1, "height", 0, 2, 1024);
		s.require(width == std::floor(width) && height == std::floor(height), "field dimensions must be integers");
		const Vector2 origin = s.vector2_field(1, "origin", Vector2());
		const double cell = s.number_field(1, "cell", 1, .05, 64);
		const int count = int(width) * int(height) * channels.size();
		s.require(s.field_cells + count <= 4 * 1024 * 1024, "field cell budget exceeded");
		s.field(1, "data"); s.require(s.array(-1, count) == count, "field data length must equal width*height*channels");
		Vector<uint8_t> bytes; bytes.resize(count * sizeof(float));
		float *values = reinterpret_cast<float *>(bytes.ptrw());
		for (int i = 0; i < count; ++i) { lua_rawgeti(p_state, -1, i + 1); values[i] = float(s.number(-1)); lua_pop(p_state, 1); }
		lua_pop(p_state, 1);
		s.field_cells += count;
		static const Image::Format formats[] = {Image::FORMAT_RF, Image::FORMAT_RGF, Image::FORMAT_RGBF, Image::FORMAT_RGBAF};
		Ref<Image> image = Image::create_from_data(int(width), int(height), false, formats[channels.size() - 1], bytes);
		s.require(image.is_valid() && !image->is_empty(), "could not create field raster");
		Dictionary meta; meta["schema"] = 1; meta["kind"] = "field"; meta["channels"] = channels;
		meta["origin"] = Array({origin.x, origin.y}); meta["cell"] = cell; meta["labels"] = labels;
		image->set_meta("veya_field", meta);
		return s.push_resource(image);
	}
	// Bilinear, edge-clamped world-space sample. Returns one number per channel.
	static int sample(lua_State *p_state) {
		auto &s = self(p_state); s.arity(3);
		const auto view = s.field_views.find(s.handle_id(1));
		s.require(view != s.field_views.end(), "sample requires a field handle");
		const FieldView &f = view->second;
		const double x = std::clamp((s.number(2) - f.origin_x) / f.cell, 0.0, double(f.width - 1));
		const double z = std::clamp((s.number(3) - f.origin_z) / f.cell, 0.0, double(f.height - 1));
		const int ix = MIN(int(x), f.width - 2), iz = MIN(int(z), f.height - 2);
		const double fx = x - ix, fz = z - iz;
		for (int c = 0; c < f.channels; ++c) {
			auto at = [&](int i, int j) { return double(f.data[(size_t(j) * f.width + i) * f.channels + c]); };
			const double top = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * fx;
			const double bottom = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * fx;
			lua_pushnumber(p_state, top + (bottom - top) * fz);
		}
		return f.channels;
	}
	static int field_info(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		const size_t id = s.handle_id(1);
		const auto view = s.field_views.find(id);
		s.require(view != s.field_views.end(), "field_info requires a field handle");
		const FieldView &f = view->second;
		const Dictionary meta = Ref<Image>(s.resources[id])->get_meta("veya_field");
		auto names = [&](const Array &p_names) {
			lua_createtable(p_state, p_names.size(), 0);
			for (int i = 0; i < p_names.size(); ++i) { const CharString text = String(p_names[i]).utf8(); lua_pushlstring(p_state, text.get_data(), text.length()); lua_rawseti(p_state, -2, i + 1); }
		};
		lua_createtable(p_state, 0, 6);
		lua_pushnumber(p_state, f.width); lua_setfield(p_state, -2, "width");
		lua_pushnumber(p_state, f.height); lua_setfield(p_state, -2, "height");
		lua_pushnumber(p_state, f.cell); lua_setfield(p_state, -2, "cell");
		lua_createtable(p_state, 2, 0); lua_pushnumber(p_state, f.origin_x); lua_rawseti(p_state, -2, 1); lua_pushnumber(p_state, f.origin_z); lua_rawseti(p_state, -2, 2); lua_setfield(p_state, -2, "origin");
		names(meta.get("channels", Array())); lua_setfield(p_state, -2, "channels");
		names(meta.get("labels", Array())); lua_setfield(p_state, -2, "labels");
		return 1;
	}
	// cooker.points({kinds={...},labels={...},points={{kind=1,habitat=1,position={},normal={},radius=}}})
	// Encoded as an RGBA float Image, one row per point:
	// (x,y,z,radius) (nx,ny,nz,kind) (habitat,0,0,0). Indices are 1-based; habitat 0 is none.
	static int points(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		s.fields(1, {"kinds", "labels", "points"});
		const Array kinds = s.name_list(1, "kinds", 64, true);
		const Array labels = s.name_list(1, "labels", 64, false);
		s.field(1, "points"); const int count = s.array(-1, 16384 - s.point_count);
		// An empty set keeps one zero row; `count` in the metadata is authoritative.
		Vector<uint8_t> bytes; bytes.resize(size_t(MAX(count, 1)) * 12 * sizeof(float));
		std::memset(bytes.ptrw(), 0, bytes.size());
		float *values = reinterpret_cast<float *>(bytes.ptrw());
		const int list = lua_gettop(p_state);
		for (int i = 0; i < count; ++i) {
			lua_rawgeti(p_state, list, i + 1); const int item = lua_gettop(p_state);
			s.fields(item, {"kind", "habitat", "position", "normal", "radius"});
			const double kind = s.number_field(item, "kind", 0, 1, kinds.size());
			const double habitat = s.number_field(item, "habitat", 0, 0, labels.size());
			s.require(kind >= 1 && kind == std::floor(kind) && habitat == std::floor(habitat), "point kind and habitat must be integer indices");
			const Vector3 position = s.vector_field(item, "position", Vector3());
			Vector3 normal = s.vector_field(item, "normal", Vector3(0, 1, 0));
			s.require(normal.length() > 1e-6, "point normal must be nonzero"); normal.normalize();
			const float row[12] = {float(position.x), float(position.y), float(position.z), float(s.number_field(item, "radius", .25, 0, 64)),
				float(normal.x), float(normal.y), float(normal.z), float(kind), float(habitat), 0, 0, 0};
			std::memcpy(values + size_t(i) * 12, row, sizeof(row));
			lua_settop(p_state, list);
		}
		lua_pop(p_state, 1);
		s.point_count += count;
		Ref<Image> image = Image::create_from_data(3, MAX(count, 1), false, Image::FORMAT_RGBAF, bytes);
		s.require(image.is_valid() && !image->is_empty(), "could not create point set");
		Dictionary meta; meta["schema"] = 1; meta["kind"] = "points"; meta["kinds"] = kinds; meta["labels"] = labels; meta["count"] = count;
		image->set_meta("veya_field", meta);
		return s.push_resource(image);
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
	// Top-level Node3D children of a PackedScene: name, root-space transform and
	// transformed bounds. Lets a recipe derive anchors from a cooked layout.
	static int children(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		Ref<PackedScene> packed = s.handle(1);
		s.require(packed.is_valid(), "children requires a PackedScene handle");
		OwnedNode root(packed->instantiate());
		s.require(root != nullptr, "could not instantiate PackedScene for children");
		const int count = root->get_child_count();
		s.require(count <= 4096, "children exceeds 4096 instances");
		lua_createtable(p_state, count, 0);
		int index = 0;
		for (int i = 0; i < count; ++i) {
			Node3D *child = Object::cast_to<Node3D>(root->get_child(i));
			if (!child) continue;
			const Transform3D transform = child->get_transform();
			AABB box; bool has_bounds = false;
			collect_bounds(child, Transform3D(), box, has_bounds); // Applies the child's own transform.
			if (!has_bounds) box = AABB(transform.origin, Vector3());
			lua_createtable(p_state, 0, 5);
			const CharString name = String(child->get_name()).utf8();
			lua_pushlstring(p_state, name.get_data(), name.length()); lua_setfield(p_state, -2, "name");
			s.push_vector(transform.origin); lua_setfield(p_state, -2, "position");
			lua_createtable(p_state, 3, 0);
			for (int c = 0; c < 3; ++c) { s.push_vector(transform.basis.get_column(c)); lua_rawseti(p_state, -2, c + 1); }
			lua_setfield(p_state, -2, "basis");
			s.push_vector(box.position); lua_setfield(p_state, -2, "min");
			s.push_vector(box.get_end()); lua_setfield(p_state, -2, "max");
			lua_rawseti(p_state, -2, ++index);
		}
		return 1;
	}
	struct Part { Ref<Mesh> mesh; Ref<Material> material; Transform3D transform; };
	static void collect_parts(Node *p_node, const Transform3D &p_parent, std::vector<Part> &r_parts, int p_depth) {
		Transform3D transform = p_parent;
		if (Node3D *node_3d = Object::cast_to<Node3D>(p_node)) transform = p_parent * node_3d->get_transform();
		if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_node); mesh && mesh->get_mesh().is_valid()) {
			Ref<Material> material = mesh->get_material_override();
			if (material.is_null() && mesh->get_mesh()->get_surface_count() == 1) material = mesh->get_surface_override_material(0);
			r_parts.push_back({mesh->get_mesh(), material, transform});
		}
		// Recipe-batched models expand to one part per MultiMesh instance.
		if (MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_node); multi && multi->get_multimesh().is_valid() && multi->get_multimesh()->get_mesh().is_valid()) {
			const Ref<MultiMesh> batch = multi->get_multimesh();
			for (int i = 0; i < batch->get_instance_count() && r_parts.size() <= 64; ++i)
				r_parts.push_back({batch->get_mesh(), multi->get_material_override(), transform * batch->get_instance_transform(i)});
		}
		if (p_depth < 32 && r_parts.size() <= 64) for (int i = 0; i < p_node->get_child_count(); ++i) collect_parts(p_node->get_child(i), transform, r_parts, p_depth + 1);
	}
	// MeshInstance3D descendants and MultiMesh instances of a PackedScene as
	// {mesh,material,basis,position} in root space, so dense scatter can batch
	// cooked models into MultiMesh. material is the node or single-surface
	// override, else absent (the mesh's own surface materials apply).
	static int parts(lua_State *p_state) {
		auto &s = self(p_state); s.arity(1);
		Ref<PackedScene> packed = s.handle(1);
		s.require(packed.is_valid(), "parts requires a PackedScene handle");
		OwnedNode root(packed->instantiate());
		s.require(root != nullptr, "could not instantiate PackedScene for parts");
		std::vector<Part> found;
		collect_parts(root.get(), Transform3D(), found, 0);
		s.require(!found.empty() && found.size() <= 64, "parts requires 1..64 mesh instances");
		lua_createtable(p_state, found.size(), 0);
		for (size_t i = 0; i < found.size(); ++i) {
			const Transform3D &transform = found[i].transform;
			s.require(transform.basis.is_finite() && std::abs(transform.basis.determinant()) > 1e-12, "part transform must be invertible");
			const Ref<Material> &material = found[i].material;
			lua_createtable(p_state, 0, 4);
			s.push_resource(found[i].mesh); lua_setfield(p_state, -2, "mesh");
			if (material.is_valid()) { s.push_resource(material); lua_setfield(p_state, -2, "material"); }
			s.push_vector(transform.origin); lua_setfield(p_state, -2, "position");
			lua_createtable(p_state, 3, 0);
			for (int c = 0; c < 3; ++c) { s.push_vector(transform.basis.get_column(c)); lua_rawseti(p_state, -2, c + 1); }
			lua_setfield(p_state, -2, "basis");
			lua_rawseti(p_state, -2, i + 1);
		}
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
		const luaL_Reg methods[] = {{"primitive", primitive}, {"material", material}, {"procedural_texture", procedural_texture}, {"effect", effect}, {"atmosphere", atmosphere}, {"camera_attributes", camera_attributes}, {"cloud_mask", cloud_mask}, {"mesh", mesh}, {"scene", scene}, {"lightmap_scene", lightmap_scene}, {"field", field_raster}, {"sample", sample}, {"field_info", field_info}, {"points", points}, {"children", children}, {"parts", parts}, {"input", input}, {"bounds", bounds}, {"random", random}, {nullptr, nullptr}};
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
	result["api_version"] = 6; result["operation"] = "run-recipe";
	Array apis;
	for (const char *name : {"primitive", "mesh", "material", "procedural_texture", "effect", "atmosphere", "camera_attributes", "cloud_mask", "scene", "lightmap_scene", "field", "sample", "field_info", "points", "children", "parts", "input", "bounds", "random", "parameters"}) apis.push_back(name);
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
	recipe.seed_value = uint32_t(int64_t(seed));
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
		if (!CookerFiles::is_generated(path)) return reject(r_result, "inputs must already be cooked assets");
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
		Ref<Mesh> mesh = resource; Ref<Material> material = resource; Ref<PackedScene> scene = resource; Ref<Shader> shader = resource; Ref<Texture2D> texture = resource;
		Ref<Image> raster = resource;
		if (raster.is_valid()) {
			// Only Cooker-authored fields/point sets; arbitrary Images stay rejected.
			const Dictionary meta = raster->get_meta("veya_field", Dictionary());
			Recipe::FieldView view;
			if (String(meta.get("kind", "")) == "field" && Recipe::decode_field(raster, view) != nullptr) return reject(r_result, "invalid field input");
			if (String(meta.get("kind", "")) != "field" && String(meta.get("kind", "")) != "points") return reject(r_result, "Image inputs must be Cooker fields or point sets");
		} else if (mesh.is_null() && material.is_null() && scene.is_null() && shader.is_null() && texture.is_null()) return reject(r_result, "recipe inputs must be Mesh, Material, PackedScene, Shader, Texture2D or field resources");
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
	// Godot seeds built-in subresource IDs from the save path. Keep the private
	// staging path stable so identical Luau inputs/seed produce identical bytes.
	String temporary = output.get_basename() + ".recipe-tmp." + output.get_extension();
	if (FileAccess::exists(temporary)) return reject(r_result, "temporary output collision", ERR_ALREADY_EXISTS);
	error = ResourceSaver::save(recipe.result, temporary, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
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
	Dictionary effect_stats;
	effect_stats["layers"] = recipe.effect_layers; effect_stats["particle_systems"] = recipe.effect_particle_systems;
	effect_stats["declared_particles"] = recipe.effect_particles; effect_stats["textures"] = recipe.effect_textures;
	r_result["effect"] = effect_stats;
	r_result["field_cells"] = recipe.field_cells; r_result["points"] = recipe.point_count;
	return OK;
}
}
/*>>----- VEYA_COOKER */
