/*<<----- VEYA_COOKER: sandboxed Luau pipeline loader; it describes jobs but performs no IO itself. */
#include "pipeline.h"

#include "asset_files.h"

#include "core/io/file_access.h"

#include "Luau/Compiler.h"
#include "lua.h"
#include "lualib.h"

#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <map>
#include <set>
#include <string>

namespace CookerPipeline {
namespace {
constexpr const char *LUAU_VERSION = "0.738";
constexpr const char *LUAU_COMMIT = "c54f558b4d5748ab0658610b8ce0c432053e41eb";
constexpr int SOURCE_BYTES = 256 * 1024;
constexpr int MEMORY_BYTES = 16 * 1024 * 1024;
constexpr int TIME_MS = 1000;
constexpr int MAX_STEPS = 256;
constexpr int MAX_DEPTH = 16;
constexpr int MAX_ENTRIES = 65536;
constexpr int MAX_MODULES = 12;
constexpr double MAX_EXACT_INTEGER = 9007199254740991.0;

struct Pipeline {
	lua_State *L = nullptr;
	bool memory_failed = false;
	size_t allocated = 0;
	int entries = 0;
	uint64_t interrupts = 0;
	std::chrono::steady_clock::time_point deadline;
	String fault;
	std::string bytecode;
	std::map<std::string, int> layout_cache;
	std::set<std::string> importing;
	size_t imported_bytes = 0;
	Dictionary dependencies;

	~Pipeline() {
		if (L) {
			lua_close(L);
		}
	}

	static Pipeline &self(lua_State *p_state) {
		return *static_cast<Pipeline *>(lua_callbacks(p_state)->userdata);
	}

	static void *allocate(void *p_ud, void *p_ptr, size_t p_old, size_t p_new) {
		auto &pipeline = *static_cast<Pipeline *>(p_ud);
		if (!p_ptr) {
			p_old = 0;
		}
		if (!p_new) {
			std::free(p_ptr);
			pipeline.allocated -= p_old;
			return nullptr;
		}
		if (p_new > p_old && p_new - p_old > MEMORY_BYTES - pipeline.allocated) {
			pipeline.memory_failed = true;
			return nullptr;
		}
		void *next = std::realloc(p_ptr, p_new);
		if (!next) {
			pipeline.memory_failed = true;
			return nullptr;
		}
		pipeline.allocated = pipeline.allocated - p_old + p_new;
		return next;
	}

	static void interrupt(lua_State *p_state, int p_gc) {
		if (p_gc >= 0) {
			return;
		}
		auto &pipeline = self(p_state);
		if (++pipeline.interrupts > 1000000 || std::chrono::steady_clock::now() >= pipeline.deadline || pipeline.memory_failed) {
			pipeline.fault = pipeline.memory_failed ? "pipeline memory limit exceeded" : "pipeline execution budget exceeded";
			if (lua_isyieldable(p_state)) {
				lua_break(p_state);
			} else {
				luaL_error(p_state, "%s", pipeline.fault.utf8().get_data());
			}
		}
	}

	static int setup(lua_State *p_state) {
		for (lua_CFunction library : { luaopen_base, luaopen_table, luaopen_string, luaopen_math, luaopen_bit32, luaopen_utf8 }) {
			library(p_state);
			lua_settop(p_state, 0);
		}
		for (const char *name : { "print", "loadstring", "getfenv", "setfenv", "collectgarbage", "newproxy" }) {
			lua_pushnil(p_state);
			lua_setglobal(p_state, name);
		}
		lua_getglobal(p_state, "math");
		for (const char *name : { "random", "randomseed", "noise" }) {
			lua_pushnil(p_state);
			lua_setfield(p_state, -2, name);
		}
		lua_pop(p_state, 1);
		lua_newtable(p_state);
		lua_pushcfunction(p_state, import_layout, "pipeline.import");
		lua_setfield(p_state, -2, "import");
		lua_setreadonly(p_state, -1, true);
		lua_setglobal(p_state, "pipeline");
		luaL_sandbox(p_state);
		luaL_sandboxthread(p_state);
		return 0;
	}

	bool fail(const String &p_message) {
		if (fault.is_empty()) {
			fault = p_message;
		}
		return false;
	}

	// Imports share the existing bounded VM. Data-only layout exports and Cooker
	// job arrays are copied; author/behavior callbacks are never exposed or called.
	void push_data(const Variant &p_value) {
		switch (p_value.get_type()) {
			case Variant::NIL: lua_pushnil(L); break;
			case Variant::BOOL: lua_pushboolean(L, bool(p_value)); break;
			case Variant::INT: lua_pushnumber(L, int64_t(p_value)); break;
			case Variant::FLOAT: lua_pushnumber(L, double(p_value)); break;
			case Variant::STRING: { const CharString text = String(p_value).utf8(); lua_pushlstring(L, text.get_data(), text.length()); } break;
			case Variant::ARRAY: {
				const Array values = p_value; lua_createtable(L, values.size(), 0);
				for (int i=0; i<values.size(); ++i) { push_data(values[i]); lua_rawseti(L, -2, i+1); }
				lua_setreadonly(L, -1, true);
			} break;
			case Variant::DICTIONARY: {
				const Dictionary values = p_value; lua_createtable(L, 0, values.size());
				for (const Variant *key=values.next(nullptr); key; key=values.next(key)) {
					const CharString name=String(*key).utf8(); push_data(values[*key]); lua_setfield(L, -2, name.get_data());
				}
				lua_setreadonly(L, -1, true);
			} break;
			default: lua_pushnil(L); break; // value() rejects every other type first.
		}
	}

	bool load_layout() {
		if (lua_gettop(L)<1 || lua_gettop(L)>2) return fail("pipeline.import expects a module path and optional export kind");
		String path;
		if (!string(1,path,512)) return false;
		String kind;
		if (lua_gettop(L)==2) {if(!string(2,kind,16))return false;} else kind="layout";
		if (kind!="layout" && kind!="steps") return fail("pipeline.import export must be layout or steps");
		const String world_root=path.begins_with("res://content/mod-worlds/") ? "res://content/mod-worlds/" : "res://content/worlds/";
		const String world_relative=path.trim_prefix(world_root);
		const int world_separator=world_relative.find("/");
		const bool world_cooker=path.begins_with(world_root) && world_separator>0 && world_relative.substr(world_separator).begins_with("/cooker/");
		const bool cooker_module=path.begins_with("res://content/cooker/") || path.begins_with("res://content/shared/cooker/") || world_cooker;
		const bool content_module=cooker_module || path.begins_with("res://content/runtime/") || path.begins_with("res://content/worlds/") || path.begins_with("res://content/mod-worlds/");
		if (!content_module || !path.ends_with(".luau") || CookerFiles::check_path(path)!=OK)
			return fail("layout module path rejected");
		if (kind=="steps" && !cooker_module) return fail("step imports require Cooker modules");
		Ref<DirAccess> directory=DirAccess::open("res://");
		String prefix="res://";
		for (const String &part:path.trim_prefix("res://").split("/")) {
			prefix=prefix.path_join(part);
			if (directory.is_null() || directory->is_link(ProjectSettings::get_singleton()->globalize_path(prefix))) return fail("layout module links are forbidden");
		}
		const std::string key=(path+"#"+kind).utf8().get_data();
		if (importing.count(key)) return fail("layout module import cycle");
		const auto cached=layout_cache.find(key);
		if (cached!=layout_cache.end()) { lua_getref(L,cached->second); return true; }
		if (layout_cache.size()+importing.size()>=MAX_MODULES) return fail("pipeline module count exceeded");
		Ref<FileAccess> file=FileAccess::open(path,FileAccess::READ);
		if (file.is_null() || !file->get_length() || file->get_length()>SOURCE_BYTES || imported_bytes+file->get_length()>1024*1024)
			return fail("layout module source budget exceeded");
		const PackedByteArray bytes=file->get_buffer(file->get_length()); String text;
		if (std::memchr(bytes.ptr(),0,bytes.size()) || text.append_utf8(reinterpret_cast<const char *>(bytes.ptr()),bytes.size())!=OK)
			return fail("layout module must be UTF-8 source without NUL");
		imported_bytes+=bytes.size(); importing.insert(key);
		Luau::CompileOptions options; options.optimizationLevel=1; options.debugLevel=1;
		const std::string compiled=Luau::compile(std::string(reinterpret_cast<const char *>(bytes.ptr()),bytes.size()),options);
		int status=luau_load(L,key.c_str(),compiled.data(),compiled.size(),0);
		if (status==LUA_OK) status=lua_pcall(L,0,1,0);
		importing.erase(key);
		if (status!=LUA_OK) return fail(lua_type(L,-1)==LUA_TSTRING?String::utf8(lua_tostring(L,-1)).left(1024):String("layout module failed"));
		if (lua_type(L,-1)!=LUA_TTABLE) return fail("layout module must return a table");
		if(kind=="layout") {
			lua_getfield(L,-1,"layout");
			if (lua_type(L,-1)!=LUA_TTABLE) return fail("layout module requires a data-only layout export");
		}
		Variant layout;
		if (!value(-1,layout)) return false;
		if(kind=="steps" && (layout.get_type()!=Variant::ARRAY || Array(layout).is_empty() || Array(layout).size()>MAX_STEPS)) return fail("step module requires 1..256 data-only jobs");
		lua_settop(L,1); push_data(layout);
		layout_cache.emplace(key,lua_ref(L,-1)); dependencies[path]=FileAccess::get_sha256(path);
		return true;
	}
	static int import_layout(lua_State *p_state) {
		auto &pipeline=self(p_state);
		try { if (pipeline.load_layout()) return 1; }
		catch (const std::exception &error) { pipeline.fail(String::utf8(error.what()).left(1024)); }
		luaL_error(p_state,"%s",pipeline.fault.utf8().get_data()); return 0;
	}

	bool string(int p_index, String &r_value, int p_limit = 4096) {
		if (lua_type(L, p_index) != LUA_TSTRING) {
			return fail("pipeline keys and text values must be strings");
		}
		size_t length = 0;
		const char *text = lua_tolstring(L, p_index, &length);
		if (length > size_t(p_limit) || std::memchr(text, 0, length) != nullptr || r_value.append_utf8(text, length) != OK) {
			return fail("pipeline text must be bounded UTF-8 without NUL");
		}
		return true;
	}

	bool value(int p_index, Variant &r_value, int p_depth = 0) {
		if (p_depth > MAX_DEPTH || ++entries > MAX_ENTRIES) {
			return fail("pipeline data exceeds nesting or entry limit");
		}
		p_index = lua_absindex(L, p_index);
		switch (lua_type(L, p_index)) {
			case LUA_TBOOLEAN:
				r_value = bool(lua_toboolean(L, p_index));
				return true;
			case LUA_TNUMBER: {
				double number = lua_tonumber(L, p_index);
				if (!std::isfinite(number)) {
					return fail("pipeline numbers must be finite");
				}
				if (number >= -MAX_EXACT_INTEGER && number <= MAX_EXACT_INTEGER && number == std::floor(number)) {
					r_value = int64_t(number);
				} else {
					r_value = number;
				}
				return true;
			}
			case LUA_TSTRING: {
				String text;
				if (!string(p_index, text)) {
					return false;
				}
				r_value = text;
				return true;
			}
			case LUA_TTABLE:
				break;
			default:
				return fail("pipeline values must be booleans, finite numbers, strings or tables");
		}

		int length = lua_objlen(L, p_index);
		int count = 0;
		bool has_numeric = false;
		bool has_string = false;
		lua_pushnil(L);
		while (lua_next(L, p_index)) {
			++count;
			if (lua_type(L, -2) == LUA_TNUMBER) {
				double key = lua_tonumber(L, -2);
				if (!std::isfinite(key) || key < 1 || key > length || key != std::floor(key)) {
					lua_pop(L, 2);
					return fail("pipeline arrays must be dense and 1-based");
				}
				has_numeric = true;
			} else if (lua_type(L, -2) == LUA_TSTRING) {
				has_string = true;
			} else {
				lua_pop(L, 2);
				return fail("pipeline table keys must be strings or dense array indices");
			}
			lua_pop(L, 1);
		}
		if (has_numeric && has_string) {
			return fail("pipeline tables cannot mix array indices and named fields");
		}
		if (has_numeric) {
			if (count != length || length > MAX_ENTRIES) {
				return fail("pipeline arrays must be dense and bounded");
			}
			Array array;
			array.resize(length);
			for (int i = 0; i < length; ++i) {
				lua_rawgeti(L, p_index, i + 1);
				Variant item;
				bool valid = value(-1, item, p_depth + 1);
				lua_pop(L, 1);
				if (!valid) {
					return false;
				}
				array[i] = item;
			}
			r_value = array;
			return true;
		}

		Dictionary dictionary;
		lua_pushnil(L);
		while (lua_next(L, p_index)) {
			String key;
			if (!string(-2, key, 256)) {
				lua_pop(L, 2);
				return false;
			}
			Variant item;
			bool valid = value(-1, item, p_depth + 1);
			lua_pop(L, 1);
			if (!valid) {
				lua_pop(L, 1);
				return false;
			}
			dictionary[key] = item;
		}
		r_value = dictionary;
		return true;
	}

	bool execute(const char *p_source, size_t p_length, Array &r_steps) {
		try {
			Luau::CompileOptions options;
			options.optimizationLevel = 1;
			options.debugLevel = 1;
			const char *const absent[] = { "buffer", "vector", "integer", "os", "debug", "coroutine", nullptr };
			const char *const removed[] = { "math.random", "math.randomseed", "math.noise", "print", "loadstring", "getfenv", "setfenv", "collectgarbage", "newproxy", nullptr };
			options.mutableGlobals = absent;
			options.disabledBuiltins = removed;
			bytecode = Luau::compile(std::string(p_source, p_length), options);
			L = lua_newstate(allocate, this);
			if (!L) {
				return fail("cannot allocate pipeline VM");
			}
			lua_callbacks(L)->userdata = this;
			int status = lua_cpcall(L, setup, nullptr);
			if (status == LUA_OK) {
				status = luau_load(L, "=cooker_pipeline", bytecode.data(), bytecode.size(), 0);
			}
			deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TIME_MS);
			lua_callbacks(L)->interrupt = interrupt;
			if (status == LUA_OK) {
				status = lua_resume(L, nullptr, 0);
			}
			if (status != LUA_OK || memory_failed || !fault.is_empty() || std::chrono::steady_clock::now() >= deadline) {
				if (memory_failed) {
					return fail("pipeline memory limit exceeded");
				}
				if (fault.is_empty() && std::chrono::steady_clock::now() >= deadline) {
					return fail("pipeline execution budget exceeded");
				}
				if (fault.is_empty()) {
					fault = lua_type(L, -1) == LUA_TSTRING ? String::utf8(lua_tostring(L, -1)).left(1024) : String("pipeline failed");
				}
				return false;
			}
			if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TTABLE) {
				return fail("pipeline must return exactly one dense array of job tables");
			}
			Variant root;
			if (!value(1, root) || root.get_type() != Variant::ARRAY) {
				return fail(fault.is_empty() ? "pipeline must return a dense array" : fault);
			}
			r_steps = root;
			if (r_steps.is_empty() || r_steps.size() > MAX_STEPS) {
				return fail("pipeline must contain 1..256 steps");
			}
			for (const Variant &step : r_steps) {
				if (step.get_type() != Variant::DICTIONARY) {
					return fail("every pipeline step must be a job table");
				}
			}
			return true;
		} catch (const std::exception &error) {
			return fail(String::utf8(error.what()).left(1024));
		}
	}
};
} // namespace

Dictionary capabilities() {
	Dictionary result;
	result["language"] = "Luau";
	result["version"] = LUAU_VERSION;
	result["commit"] = LUAU_COMMIT;
	result["api_version"] = 1;
	result["operation"] = "run-pipeline";
	result["max_steps"] = MAX_STEPS;
	result["layout_import"] = true;
	result["step_import"] = true;
	result["max_layout_modules"] = MAX_MODULES;
	return result;
}

Error load(const String &p_source, Array &r_steps, Dictionary &r_result) {
	if (p_source.get_extension().to_lower() != "luau") {
		r_result["message"] = "pipeline source must be .luau text";
		return ERR_INVALID_PARAMETER;
	}
	Error error = CookerFiles::check_path(p_source);
	if (error != OK) {
		r_result["message"] = "pipeline source path rejected";
		return error;
	}
	Ref<FileAccess> file = FileAccess::open(p_source, FileAccess::READ, &error);
	if (file.is_null() || error != OK || file->get_length() == 0 || file->get_length() > SOURCE_BYTES) {
		r_result["message"] = "pipeline source must contain 1..262144 bytes";
		return error == OK ? ERR_INVALID_DATA : error;
	}
	PackedByteArray code = file->get_buffer(file->get_length());
	String text;
	if (std::memchr(code.ptr(), 0, code.size()) || text.append_utf8(reinterpret_cast<const char *>(code.ptr()), code.size()) != OK) {
		r_result["message"] = "pipeline must be UTF-8 text without NUL (not bytecode)";
		return ERR_INVALID_DATA;
	}
	Pipeline pipeline;
	if (!pipeline.execute(reinterpret_cast<const char *>(code.ptr()), code.size(), r_steps)) {
		r_result["message"] = pipeline.fault;
		return ERR_SCRIPT_FAILED;
	}
	r_result["source"] = p_source;
	r_result["source_sha256"] = FileAccess::get_sha256(p_source);
	r_result["step_count"] = r_steps.size();
	r_result["runtime"] = capabilities();
	r_result["dependencies"] = pipeline.dependencies;
	return OK;
}
} // namespace CookerPipeline
/*>>----- VEYA_COOKER */
