#include "virtual_geometry_validation.h"

#include "asset_files.h"

namespace CookerVirtualGeometry {
namespace vg = VirtualGeometry;

Error validate_container(const Ref<VirtualGeometryMesh> &p_mesh, const String &p_resource_path) {
	ERR_FAIL_COND_V(p_mesh.is_null() || !p_mesh->has_geometry(), ERR_INVALID_DATA);
	const String path = p_resource_path.get_base_dir().path_join(p_mesh->get_container_file());
	Error error = CookerFiles::check_path(path);
	ERR_FAIL_COND_V(error != OK, error);
	const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &error);
	ERR_FAIL_COND_V(file.is_null(), error == OK ? ERR_FILE_CANT_READ : error);
	ERR_FAIL_COND_V(file->get_length() != p_mesh->get_container_bytes(), ERR_INVALID_DATA);
	const String digest = FileAccess::get_sha256(path);
	ERR_FAIL_COND_V(digest.length() != 64 || p_mesh->get_container_file() != "geometry--" + digest + ".vgp", ERR_INVALID_DATA);
	const auto &model = p_mesh->get_model();
	Vector<uint8_t> bytes;
	for (uint32_t page = 0; page < model.directory.pages.size(); ++page) {
		const auto &location = model.directory.pages[page];
		ERR_FAIL_COND_V(bytes.resize(location.disk_bytes) != OK, ERR_OUT_OF_MEMORY);
		file->seek(location.container_offset);
		ERR_FAIL_COND_V(file->get_buffer(bytes.ptrw(), bytes.size()) != static_cast<uint64_t>(bytes.size()), ERR_FILE_CANT_READ);
		vg::GeometryPage geometry;
		vg::HierarchyPage fixups;
		ERR_FAIL_COND_V(vg::decode_geometry_model_page(model, page, bytes.ptr(), bytes.size(), geometry, fixups).error != vg::ModelCodecError::OK, ERR_INVALID_DATA);
		std::vector<uint8_t> gpu;
		ERR_FAIL_COND_V(vg::transcode_geometry_page_cpu(bytes.ptr(), location.fixup_offset, gpu).error != vg::PageCodecError::OK || gpu.size() != location.gpu_bytes, ERR_INVALID_DATA);
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
