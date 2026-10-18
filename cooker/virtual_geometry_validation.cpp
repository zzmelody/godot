#include "virtual_geometry_validation.h"

#include "asset_files.h"
#include "content_hash.h"
#include "core/crypto/crypto_core.h"
#include "core/os/thread.h"

#include <array>
#include <new>
#include <set>

namespace CookerVirtualGeometry {
namespace vg = VirtualGeometry;

Error validate_container(const Ref<VirtualGeometryMesh> &p_mesh, const String &p_resource_path) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_mesh.is_null() || !p_mesh->has_geometry(), ERR_INVALID_DATA);
	const String path = p_resource_path.get_base_dir().path_join(p_mesh->get_container_file());
	Error error = CookerFiles::check_path(path);
	ERR_FAIL_COND_V(error != OK, error);
	const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &error);
	ERR_FAIL_COND_V(file.is_null(), error == OK ? ERR_FILE_CANT_READ : error);
	ERR_FAIL_COND_V(file->get_length() != p_mesh->get_container_bytes(), ERR_INVALID_DATA);
	// A bounded snapshot ties the cache key to the exact bytes being validated,
	// even if another process atomically replaces the source path during a job.
	Vector<uint8_t> snapshot;
	String digest;
	auto hash_open_file = [&](String &r_digest) -> Error {
		if (file->get_length() != p_mesh->get_container_bytes()) {
			return ERR_FILE_CORRUPT;
		}
		file->seek(0);
		cooker_hash::Sha256 hash;
		if (hash.Start() != OK) {
			return FAILED;
		}
		std::array<uint8_t, 64 * 1024> block;
		uint64_t remaining = p_mesh->get_container_bytes();
		while (remaining != 0) {
			const uint32_t count = uint32_t(MIN(remaining, uint64_t(block.size())));
			if (file->get_buffer(block.data(), count) != count) {
				return ERR_FILE_CANT_READ;
			}
			if (hash.Update(block.data(), count) != OK) {
				return FAILED;
			}
			remaining -= count;
		}
		std::array<uint8_t, 32> bytes;
		if (hash.Finish(bytes.data()) != OK) {
			return FAILED;
		}
		r_digest = String::hex_encode_buffer(bytes.data(), bytes.size());
		return OK;
	};
	if (file->get_length() <= 16 * 1024 * 1024) {
		snapshot = file->get_buffer(file->get_length());
		ERR_FAIL_COND_V(uint64_t(snapshot.size()) != p_mesh->get_container_bytes(), ERR_FILE_CANT_READ);
		std::array<uint8_t, 32> hash;
		cooker_hash::Sha256 container_hash;
		ERR_FAIL_COND_V(container_hash.Start() != OK || container_hash.Update(snapshot.ptr(), snapshot.size()) != OK || container_hash.Finish(hash.data()) != OK, FAILED);
		digest = String::hex_encode_buffer(hash.data(), hash.size());
	} else {
		// Keep the same open file through hashing and page validation. A path
		// reopened for hashing could refer to a concurrent atomic replacement.
		error = hash_open_file(digest);
		ERR_FAIL_COND_V(error != OK, error);
	}
	ERR_FAIL_COND_V(digest.length() != 64, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(p_mesh->get_container_file().begins_with("geometry--") && p_mesh->get_container_file() != "geometry--" + digest + ".vgp", ERR_INVALID_DATA);
	// A pipeline can publish hundreds of fragments of the same immutable Mesh.
	// Hash actual bytes on every call, but decode/transcode an identical pair only
	// once. The cache owns value digests; it never retains a Resource or file.
	std::array<uint8_t, 96> validation_key = {};
	for (uint32_t index = 0; index < 64; ++index) {
		validation_key[index] = static_cast<uint8_t>(digest[index]);
	}
	const PackedByteArray root = p_mesh->get_geometry_data()["root"];
	CryptoCore::SHA256Context root_hash;
	ERR_FAIL_COND_V(root_hash.start() != OK || root_hash.update(root.ptr(), root.size()) != OK || root_hash.finish(validation_key.data() + 64) != OK, FAILED);
	static std::set<std::array<uint8_t, 96>> validated;
	if (validated.count(validation_key) != 0) {
		return OK;
	}
	const auto &model = p_mesh->get_model();
	Vector<uint8_t> bytes;
	for (uint32_t page = 0; page < model.directory.pages.size(); ++page) {
		const auto &location = model.directory.pages[page];
		const uint8_t *page_bytes = nullptr;
		if (!snapshot.is_empty()) {
			page_bytes = snapshot.ptr() + location.container_offset;
		} else {
			ERR_FAIL_COND_V(bytes.resize(location.disk_bytes) != OK, ERR_OUT_OF_MEMORY);
			file->seek(location.container_offset);
			ERR_FAIL_COND_V(file->get_buffer(bytes.ptrw(), bytes.size()) != static_cast<uint64_t>(bytes.size()), ERR_FILE_CANT_READ);
			page_bytes = bytes.ptr();
		}
		vg::GeometryPage geometry;
		vg::HierarchyPage fixups;
		ERR_FAIL_COND_V(vg::decode_geometry_model_page(model, page, page_bytes, location.disk_bytes, geometry, fixups).error != vg::ModelCodecError::OK, ERR_INVALID_DATA);
		std::vector<uint8_t> gpu;
		ERR_FAIL_COND_V(vg::transcode_geometry_page_cpu(page_bytes, location.fixup_offset, gpu).error != vg::PageCodecError::OK || gpu.size() != location.gpu_bytes, ERR_INVALID_DATA);
	}
	if (snapshot.is_empty()) {
		String checked_digest;
		error = hash_open_file(checked_digest);
		ERR_FAIL_COND_V(error != OK, error);
		ERR_FAIL_COND_V_MSG(checked_digest != digest, ERR_FILE_CORRUPT, "Geometry container changed during validation.");
	}
	try {
		if (validated.size() == 256) {
			validated.clear();
		}
		validated.insert(validation_key);
	} catch (const std::bad_alloc &) {
		// Validation completed. Failure to remember it only loses reuse.
	}
	return OK;
}

uint64_t metadata_bytes(const Ref<VirtualGeometryMesh> &p_mesh) {
	const auto &model = p_mesh->get_model();
	const PackedByteArray root = p_mesh->get_geometry_data()["root"];
	// Both the serialized root and the decoded value arrays remain owned by the
	// Mesh. Include vector capacity and a fixed allowance for Godot/COW metadata.
	return sizeof(VirtualGeometryMesh) + uint64_t(root.size()) + 1024 +
			uint64_t(p_mesh->get_surface_count()) * sizeof(Ref<Material>) +
			model.materials.capacity() * sizeof(uint32_t) +
			model.groups.capacity() * sizeof(vg::ModelGroupPages) +
			model.hierarchy.nodes.capacity() * sizeof(vg::HierarchyNode) +
			model.hierarchy.parts.capacity() * sizeof(vg::HierarchyPart) +
			model.hierarchy.pages.capacity() * sizeof(vg::HierarchyPage) +
			model.directory.pages.capacity() * sizeof(vg::PageLocation) +
			model.directory.dependencies.capacity() * sizeof(uint32_t);
}

} // namespace CookerVirtualGeometry
