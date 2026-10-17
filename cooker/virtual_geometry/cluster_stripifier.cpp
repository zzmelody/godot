#include "cluster_stripifier.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <utility>

namespace VirtualGeometry {
namespace {

uint32_t next_corner(uint32_t p_corner) {
	return p_corner / 3 * 3 + (p_corner + 1) % 3;
}

uint32_t previous_corner(uint32_t p_corner) {
	return p_corner / 3 * 3 + (p_corner + 2) % 3;
}

struct Candidate {
	uint32_t corner = UINT32_MAX;
	std::array<uint32_t, 3> mapped = {};
	uint32_t new_vertices = 0;
	int32_t score = -1;
	double priority = -std::numeric_limits<double>::infinity();
};

class Stripifier {
	const TriangleMeshInput &mesh;
	std::vector<uint32_t> latest;
	std::array<uint32_t, MAX_CLUSTER_TRIANGLES * 3> opposite;
	std::array<bool, MAX_CLUSTER_TRIANGLES> enabled = {};
	std::array<uint32_t, 4> block_new = {};
	std::array<uint32_t, 4> block_refs = {};
	StripifiedCluster output;
	uint32_t reference_bits = 0;

	bool available(uint32_t p_corner) const {
		return p_corner != UINT32_MAX && enabled[p_corner / 3];
	}

	std::array<uint32_t, 3> old_vertices(uint32_t p_corner) const {
		return { mesh.indices[next_corner(p_corner)], mesh.indices[previous_corner(p_corner)], mesh.indices[p_corner] };
	}

	void build_opposites() {
		opposite.fill(UINT32_MAX);
		std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> owners;
		for (uint32_t corner = 0; corner < mesh.indices.size(); ++corner) {
			owners[{ mesh.indices[next_corner(corner)], mesh.indices[previous_corner(corner)] }].push_back(corner);
		}
		for (uint32_t corner = 0; corner < mesh.indices.size(); ++corner) {
			const auto found = owners.find({ mesh.indices[previous_corner(corner)], mesh.indices[next_corner(corner)] });
			if (found != owners.end() && !found->second.empty()) {
				opposite[corner] = found->second.back();
				found->second.pop_back();
			}
		}
	}

	Candidate evaluate(uint32_t p_corner, bool p_start) const {
		Candidate result;
		if (!available(p_corner)) {
			return result;
		}
		const auto vertices = old_vertices(p_corner);
		const uint32_t created = static_cast<uint32_t>(output.source_vertices.size());
		uint32_t after = created;
		for (size_t side = 0; side < 3; ++side) {
			result.mapped[side] = latest[vertices[side]];
			after += result.mapped[side] == UINT32_MAX ? 1u : 0u;
		}
		for (;;) {
			bool changed = false;
			for (auto &index : result.mapped) {
				if (index != UINT32_MAX && after - index >= 32) {
					index = UINT32_MAX;
					++after;
					changed = true;
				}
			}
			if (!changed) {
				break;
			}
		}
		if (p_start) {
			for (size_t side = 1; side < 3; ++side) {
				if (result.mapped[side - 1] == UINT32_MAX && result.mapped[side] != UINT32_MAX) {
					return {};
				}
			}
		} else if (result.mapped[0] == UINT32_MAX || result.mapped[1] == UINT32_MAX) {
			return {};
		}
		result.corner = p_corner;
		result.new_vertices = after - created;
		result.score = 0;
		for (uint32_t vertex : vertices) {
			const uint32_t index = latest[vertex];
			if (index != UINT32_MAX && created - 1 - index < 32) {
				result.score += static_cast<int32_t>(32 - (created - 1 - index)) * 4;
			}
		}
		result.score += available(opposite[p_corner]) ? 2 : 0;
		result.score += available(opposite[next_corner(p_corner)]) ? 4 : 0;
		result.score += available(opposite[previous_corner(p_corner)]) ? 4 : 0;
		result.priority = static_cast<double>(mesh.positions[vertices[0]][0]) + mesh.positions[vertices[1]][0] + mesh.positions[vertices[2]][0];
		return result;
	}

	void put_reference(uint32_t p_value) {
		for (uint32_t bit = 0; bit < 5; ++bit, ++reference_bits) {
			if (reference_bits / 8 == output.topology.references.size()) {
				output.topology.references.push_back(0);
			}
			output.topology.references[reference_bits / 8] |= static_cast<uint8_t>(((p_value >> bit) & 1u) << (reference_bits % 8));
		}
	}

	void visit(const Candidate &p_candidate, bool p_start, bool p_left) {
		const uint32_t triangle = static_cast<uint32_t>(output.source_triangles.size());
		const uint32_t block = triangle / 32;
		const uint32_t bit = 1u << (triangle % 32);
		const uint32_t created = static_cast<uint32_t>(output.source_vertices.size());
		auto &masks = output.topology.layout.masks[block];
		if (p_start) {
			const uint32_t references = 3 - p_candidate.new_vertices;
			masks[0] |= bit;
			masks[1] |= (references >> 1) != 0 ? bit : 0;
			masks[2] |= (references & 1u) != 0 ? bit : 0;
			for (uint32_t side = 0; side < references; ++side) {
				put_reference(created - 1 - p_candidate.mapped[side]);
			}
			block_refs[block] += references;
		} else {
			masks[1] |= p_left ? bit : 0;
			if (p_candidate.mapped[2] != UINT32_MAX) {
				masks[2] |= bit;
				put_reference(created - 1 - p_candidate.mapped[2]);
				++block_refs[block];
			}
		}
		const auto vertices = old_vertices(p_candidate.corner);
		for (size_t side = 0; side < 3; ++side) {
			if (p_candidate.mapped[side] == UINT32_MAX) {
				latest[vertices[side]] = static_cast<uint32_t>(output.source_vertices.size());
				output.source_vertices.push_back(vertices[side]);
			}
		}
		block_new[block] += p_candidate.new_vertices;
		output.source_triangles.push_back(p_candidate.corner / 3);
		enabled[p_candidate.corner / 3] = false;
	}

	Candidate start_candidate() const {
		Candidate best;
		for (uint32_t corner = 0; corner < mesh.indices.size(); ++corner) {
			const auto candidate = evaluate(corner, true);
			if (candidate.corner != UINT32_MAX && (candidate.score > best.score || (candidate.score == best.score && candidate.priority > best.priority))) {
				best = candidate;
			}
		}
		return best;
	}

public:
	explicit Stripifier(const TriangleMeshInput &p_mesh) :
			mesh(p_mesh), latest(p_mesh.positions.size(), UINT32_MAX) {}

	StripBuildResult run(StripifiedCluster &r_cluster) {
		build_opposites();
		for (uint32_t begin = 0; begin < mesh.materials.size();) {
			uint32_t end = begin + 1;
			while (end < mesh.materials.size() && mesh.materials[end] == mesh.materials[begin]) {
				++end;
			}
			std::fill(enabled.begin() + begin, enabled.begin() + end, true);
			for (;;) {
				Candidate current = start_candidate();
				if (current.corner == UINT32_MAX) {
					break;
				}
				visit(current, true, false);
				while (output.source_triangles.size() % 32 != 0) {
					const auto left = evaluate(opposite[next_corner(current.corner)], false);
					const auto right = evaluate(opposite[previous_corner(current.corner)], false);
					const bool choose_left = right.corner == UINT32_MAX || (left.corner != UINT32_MAX && left.score >= right.score);
					current = choose_left ? left : right;
					if (current.corner == UINT32_MAX) {
						break;
					}
					visit(current, false, choose_left);
				}
			}
			begin = end;
		}
		if (output.source_triangles.size() != mesh.materials.size()) {
			return { StripBuildError::TOPOLOGY, TopologyCodecError::INVALID_TRIANGLE };
		}
		output.topology.references.resize(((reference_bits + 31) / 32) * 4, 0);
		auto &layout = output.topology.layout;
		layout.triangles = static_cast<uint32_t>(mesh.materials.size());
		layout.vertices = static_cast<uint32_t>(output.source_vertices.size());
		uint32_t created = 0;
		uint32_t references = 0;
		for (uint32_t block = 0; block < 3; ++block) {
			created += block_new[block];
			references += block_refs[block];
			layout.new_vertices_before |= created << (block * 10);
			layout.references_before |= references << (block * 10);
		}
		const auto decoded = unpack_cluster_topology(layout, output.topology.references.data(), output.topology.references.size(), output.indices);
		if (decoded != TopologyCodecError::OK) {
			return { StripBuildError::TOPOLOGY, decoded };
		}
		for (uint32_t triangle = 0; triangle < output.source_triangles.size(); ++triangle) {
			const uint32_t source = output.source_triangles[triangle];
			uint32_t rotation = 0;
			const uint32_t first = output.source_vertices[output.indices[triangle * 3]];
			while (rotation < 3 && mesh.indices[source * 3 + rotation] != first) {
				++rotation;
			}
			for (uint32_t side = 0; side < 3; ++side) {
				if (rotation == 3 || output.source_vertices[output.indices[triangle * 3 + side]] != mesh.indices[source * 3 + (rotation + side) % 3]) {
					return { StripBuildError::TOPOLOGY, TopologyCodecError::INVALID_TRIANGLE };
				}
			}
			output.corner_rotations.push_back(static_cast<uint8_t>(rotation));
		}
		r_cluster = std::move(output);
		return {};
	}
};

} // namespace

StripBuildResult stripify_cluster(const TriangleMeshInput &p_mesh, StripifiedCluster &r_cluster) {
	if (p_mesh.materials.size() > MAX_CLUSTER_TRIANGLES || p_mesh.positions.size() > MAX_CLUSTER_TRIANGLES * 3 ||
			validate_triangle_mesh(p_mesh, TriangleValidation::BUILD_INTERMEDIATE) != TopologyError::OK) {
		return { StripBuildError::INVALID_INPUT };
	}
	try {
		return Stripifier(p_mesh).run(r_cluster);
	} catch (const std::bad_alloc &) {
		return { StripBuildError::MEMORY };
	}
}

} // namespace VirtualGeometry
