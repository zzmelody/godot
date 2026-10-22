#include "usd_import.h"

#include "asset_files.h"
#include "atomic_save.h"
#include "composition.hh"
#include "render_dependencies.h"
#include "thirdparty/meshoptimizer/meshoptimizer.h"
#include "tinyusdz.hh"
#include "tydra/scene-access.hh"
#include "usdGeom.hh"
#include "usdSkel.hh"
#include "virtual_geometry_mesh.h"

#include "core/crypto/crypto_core.h"
#include "core/io/resource_saver.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/packed_scene.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace CookerUsd {
namespace {
namespace u = tinyusdz;
using u::tydra::XformNode;
template<class T> bool sample(const u::TypedAttribute<u::Animatable<T>> &a, T &out) {
	const auto v = a.get_value(); return v && v->get(u::value::TimeCode::Default(), &out);
}
template<class T> bool constant(const u::TypedAttribute<T> &a, T &out) {
	const auto v = a.get_value(); if (!v) return false; out = *v; return true;
}
Transform3D transform(const u::value::matrix4d &m) {
	Basis b; for (int c=0;c<3;++c) b.set_column(c, Vector3(m.m[c][0],m.m[c][1],m.m[c][2]));
	return Transform3D(b,Vector3(m.m[3][0],m.m[3][1],m.m[3][2]));
}
template<class T> Vector3 vector(const T &v) { return Vector3(v[0],v[1],v[2]); }
std::vector<u::Path> targets(const u::Relationship &r) {
	if (r.is_path()) return {r.targetPath}; if (r.is_pathvector()) return r.targetPathVector; return {};
}
Error load(const String &path, u::Stage &stage, bool composed) {
	u::USDLoadOptions opts; opts.num_threads=4; opts.max_memory_limit_in_mb=4096; opts.load_assets=false;
	std::string warn, err;
	const String absolute=ProjectSettings::get_singleton()->globalize_path(path);
	if(!composed) {
		ERR_FAIL_COND_V_MSG(!u::LoadUSDFromFile(absolute.utf8().get_data(),&stage,&warn,&err,opts),ERR_INVALID_DATA,String::utf8(err.c_str()));
	} else {
		// Upstream deprecated load flags do not compose USDC. Explicitly flatten
		// local layers and references before constructing a typed stage.
		u::Layer layer,flattened;
		ERR_FAIL_COND_V_MSG(!u::LoadLayerFromFile(absolute.utf8().get_data(),&layer,&warn,&err,opts),ERR_INVALID_DATA,String::utf8(err.c_str()));
		u::AssetResolutionResolver resolver;resolver.set_search_paths({absolute.get_base_dir().utf8().get_data()});resolver.set_current_working_path(absolute.get_base_dir().utf8().get_data());
		u::SublayersCompositionOptions sub;sub.max_depth=32;sub.error_when_asset_not_found=true;sub.error_when_unsupported_fileformat=true;
		ERR_FAIL_COND_V_MSG(!u::CompositeSublayers(resolver,layer,&flattened,&warn,&err,sub),ERR_INVALID_DATA,String::utf8(err.c_str()));
		layer=std::move(flattened);flattened=u::Layer();
		u::ReferencesCompositionOptions refs;refs.max_depth=32;refs.error_when_asset_not_found=true;refs.error_when_unsupported_fileformat=true;
		ERR_FAIL_COND_V_MSG(!u::CompositeReferences(resolver,layer,&flattened,&warn,&err,refs),ERR_INVALID_DATA,String::utf8(err.c_str()));
		ERR_FAIL_COND_V_MSG(!u::LayerToStage(flattened,&stage,&warn,&err),ERR_INVALID_DATA,String::utf8(err.c_str()));
		ERR_FAIL_COND_V(!stage.compute_absolute_prim_path_and_assign_prim_id(),ERR_INVALID_DATA);
	}
	return OK;
}
Error gather(const String &path, HashSet<String> &files, HashSet<String> &pending) {
	if (files.has(path)) return OK;
	ERR_FAIL_COND_V_MSG(pending.has(path) || files.size()>=64,ERR_INVALID_DATA,"Cyclic or excessive USD layers.");
	Error e=CookerFiles::check_path(path); ERR_FAIL_COND_V(e!=OK,e);
	pending.insert(path); u::Stage stage; e=load(path,stage,false); ERR_FAIL_COND_V(e!=OK,e);
	std::set<std::string> refs;
	for (const auto &s : stage.metas().subLayers) refs.insert(s.assetPath.GetAssetPath());
	std::function<Error(const u::Prim &)> visit=[&](const u::Prim &p) {
		if(p.metas().references) for(const auto &r:p.metas().references->second) if(!r.asset_path.GetAssetPath().empty()) refs.insert(r.asset_path.GetAssetPath());
		ERR_FAIL_COND_V_MSG(p.metas().payload && !p.metas().payload->second.empty(),ERR_UNAVAILABLE,"Static USD import does not load payloads.");
		for(const auto &child:p.children()) {Error v=visit(child);if(v!=OK)return v;} return OK;
	};
	for(const auto &p:stage.root_prims()) {e=visit(p);ERR_FAIL_COND_V(e!=OK,e);}
	for(const auto &r:refs) {
		const String relative=String::utf8(r.c_str());
		ERR_FAIL_COND_V(relative.is_absolute_path() || relative.contains(":") || relative.contains("\\"),ERR_UNAUTHORIZED);
		const String dependency=path.get_base_dir().path_join(relative).simplify_path();
		ERR_FAIL_COND_V(!dependency.begins_with(path.get_base_dir()+"/") || !dependency.get_extension().to_lower().begins_with("usd"),ERR_UNAUTHORIZED);
		e=gather(dependency,files,pending);ERR_FAIL_COND_V(e!=OK,e);
	}
	pending.erase(path); files.insert(path); return OK;
}
struct Surface {
	static constexpr size_t MAX_VERTICES = 16000000;
	static constexpr size_t MAX_INDICES = 48000000;
	std::vector<Vector3> points, normals;
	std::vector<Vector2> uv;
	std::vector<unsigned int> indices;
	Error append(const Surface &s, const Transform3D &t) {
		ERR_FAIL_COND_V_MSG(points.size() > MAX_VERTICES || indices.size() > MAX_INDICES || s.points.size() > MAX_VERTICES - points.size() || s.indices.size() > MAX_INDICES - indices.size(), ERR_OUT_OF_MEMORY,
				"USD expanded geometry exceeds the bounded ordinary Mesh capacity; preserve source instancing or use a bounded import.");
		const unsigned int start = points.size();
		const Basis normal = t.basis.inverse().transposed();
		for (size_t i = 0; i < s.points.size(); ++i) {
			points.push_back(t.xform(s.points[i]));
			normals.push_back(normal.xform(s.normals[i]).normalized());
			uv.push_back(s.uv[i]);
		}
		const bool reverse = t.basis.determinant() < 0;
		for (size_t i = 0; i < s.indices.size(); i += 3) {
			indices.push_back(start + s.indices[i]);
			indices.push_back(start + s.indices[i + (reverse ? 2 : 1)]);
			indices.push_back(start + s.indices[i + (reverse ? 1 : 2)]);
		}
		return OK;
	}
	void compact() {
		std::vector<unsigned int> remap(points.size(),~0u);
		Surface compacted;
		for(auto index:indices) {
			if(remap[index]==~0u) {remap[index]=compacted.points.size();compacted.points.push_back(points[index]);compacted.normals.push_back(normals[index]);compacted.uv.push_back(uv[index]);}
			compacted.indices.push_back(remap[index]);
		}
		*this=std::move(compacted);
	}
	Error weld_values() {
		struct VertexHash {
			size_t operator()(const std::array<uint32_t, 8> &p_key) const {
				uint64_t hash = 14695981039346656037ull;
				for (uint32_t word : p_key) {
					hash = (hash ^ word) * 1099511628211ull;
				}
				return static_cast<size_t>(hash);
			}
		};
		std::unordered_map<std::array<uint32_t, 8>, unsigned int, VertexHash> values;
		std::vector<unsigned int> remap(points.size(), ~0u);
		Surface compacted;
		compacted.indices.reserve(indices.size());
		for (unsigned int index : indices) {
			if (remap[index] == ~0u) {
				ERR_FAIL_COND_V(!points[index].is_finite() || !normals[index].is_finite() || !uv[index].is_finite(), ERR_INVALID_DATA);
				const std::array<float, 8> vertex = { points[index].x, points[index].y, points[index].z,
					normals[index].x, normals[index].y, normals[index].z, uv[index].x, uv[index].y };
				std::array<uint32_t, 8> key;
				std::memcpy(key.data(), vertex.data(), sizeof(key));
				const auto [found, added] = values.emplace(key, compacted.points.size());
				if (added) {
					compacted.points.push_back(points[index]);
					compacted.normals.push_back(normals[index]);
					compacted.uv.push_back(uv[index]);
				}
				remap[index] = found->second;
			}
			compacted.indices.push_back(remap[index]);
		}
		*this = std::move(compacted);
		return OK;
	}
	void reduce(size_t triangles) {
		if (triangles == 0) {
			return; // Preserve authored geometry for cluster building.
		}
		if (indices.size() <= triangles * 3 || points.empty()) {
			return;
		}
		static_assert(sizeof(Vector3) == sizeof(float) * 3, "USD mesh optimization needs float engine vectors");
		std::vector<unsigned int> selected(indices.size());
		float error = 0;
		const size_t count = meshopt_simplifySloppy(selected.data(), indices.data(), indices.size(), reinterpret_cast<const float *>(points.data()), points.size(), sizeof(Vector3), nullptr, triangles * 3, 1.0f, &error);
		if (count >= 3) {
			selected.resize(count);
			indices = std::move(selected);
			compact();
		}
	}
	Array arrays() const {
		PackedVector3Array p, n;
		PackedVector2Array tex;
		PackedInt32Array ix;
		if (points.size() > MAX_VERTICES || indices.size() > MAX_INDICES || p.resize(points.size()) != OK || n.resize(normals.size()) != OK || tex.resize(uv.size()) != OK || ix.resize(indices.size()) != OK) {
			return Array();
		}
		for (size_t i = 0; i < points.size(); ++i) {
			p.set(i, points[i]);
			n.set(i, normals[i]);
			tex.set(i, uv[i]);
		}
		for (size_t i = 0; i < indices.size(); ++i) {
			ix.set(i, indices[i]);
		}
		Array out;
		out.resize(Mesh::ARRAY_MAX);
		out[Mesh::ARRAY_VERTEX] = p;
		out[Mesh::ARRAY_NORMAL] = n;
		out[Mesh::ARRAY_TEX_UV] = tex;
		out[Mesh::ARRAY_INDEX] = ix;
		return out;
	}
};
struct Import {
	u::Stage stage; XformNode hierarchy;
	std::map<std::string,const XformNode *> nodes;
	std::map<std::string,std::array<Surface,2>> prototypes;
	std::array<Surface,2> surfaces;
	std::set<std::string> library_roots;
	int prototype_budget=1200,trunk_budget=6000,leaf_budget=40000,instance_count=0;
	int64_t source_triangles=0;
	Error skin(const XformNode &node,const u::GeomMesh &mesh,std::vector<u::value::point3f> &points,std::vector<u::value::normal3f> &normals) {
		u::GeomPrimvar ji,jw;std::string err;std::vector<int> indices;std::vector<float> weights;
		if(!u::tydra::GetGeomPrimvar(stage,&mesh,"skel:jointIndices",&ji,&err))return OK;
		ERR_FAIL_COND_V(!ji.flatten_with_indices(&indices,&err) || !u::tydra::GetGeomPrimvar(stage,&mesh,"skel:jointWeights",&jw,&err) || !jw.flatten_with_indices(&weights,&err),ERR_INVALID_DATA);
		const int stride=ji.get_elementSize();ERR_FAIL_COND_V(stride<1 || stride>16 || indices.size()!=points.size()*stride || weights.size()!=indices.size(),ERR_INVALID_DATA);
		const XformNode *binding=&node;u::Relationship rel;u::Property relationship;
		if(mesh.skeleton)rel=*mesh.skeleton;
		while(binding && !mesh.skeleton) {
			if(binding->prim && u::tydra::GetProperty(*binding->prim,"skel:skeleton",&relationship,&err) && relationship.is_relationship()) {rel=relationship.get_relationship();break;}
			binding=binding->parent;
		}
		ERR_FAIL_COND_V_MSG(!binding || targets(rel).empty(),ERR_INVALID_DATA,"Skinned USD mesh has no inherited skeleton: "+String::utf8(node.absolute_path.full_path_name().c_str()));
		const std::string skel_path=targets(rel)[0].full_path_name();
		ERR_FAIL_COND_V(!nodes.count(skel_path),ERR_INVALID_DATA);
		const auto *skel=nodes.at(skel_path)->prim->as<u::Skeleton>();ERR_FAIL_NULL_V(skel,ERR_INVALID_DATA);
		std::vector<u::value::matrix4d> binds,rests;std::vector<u::value::token> joints;
		ERR_FAIL_COND_V(!constant(skel->bindTransforms,binds) || !constant(skel->restTransforms,rests) || !constant(skel->joints,joints) || binds.size()!=joints.size() || rests.size()!=joints.size(),ERR_INVALID_DATA);
		std::vector<Transform3D> local,world,skin_matrices;for(const auto &r:rests)local.push_back(transform(r));
		if(skel->animationSource && !targets(*skel->animationSource).empty()) {
			const auto ap=targets(*skel->animationSource)[0].full_path_name();ERR_FAIL_COND_V(!nodes.count(ap),ERR_INVALID_DATA);
			const auto *a=nodes.at(ap)->prim->as<u::SkelAnimation>();ERR_FAIL_NULL_V(a,ERR_INVALID_DATA);
			std::vector<u::value::token> aj;std::vector<u::value::float3> tr;std::vector<u::value::quatf> qr;std::vector<u::value::half3> sc;
			if(constant(a->joints,aj) && sample(a->translations,tr) && sample(a->rotations,qr) && sample(a->scales,sc)) {
				ERR_FAIL_COND_V(aj.size()!=tr.size() || aj.size()!=qr.size() || aj.size()!=sc.size(),ERR_INVALID_DATA);
				for(size_t i=0;i<aj.size();++i) {auto j=std::find(joints.begin(),joints.end(),aj[i]);ERR_FAIL_COND_V(j==joints.end(),ERR_INVALID_DATA);Basis b(Quaternion(qr[i][0],qr[i][1],qr[i][2],qr[i][3]).normalized());b.scale(Vector3(u::value::half_to_float(sc[i][0]),u::value::half_to_float(sc[i][1]),u::value::half_to_float(sc[i][2])));local[j-joints.begin()]=Transform3D(b,vector(tr[i]));}
			}
		}
		std::map<std::string,int> order;
		for(size_t i=0;i<joints.size();++i) {
			std::string name=joints[i].str();const auto slash=name.rfind('/');Transform3D parent;
			if(slash!=std::string::npos) {const std::string p=name.substr(0,slash);ERR_FAIL_COND_V(!order.count(p),ERR_INVALID_DATA);parent=world[order[p]];}
			order[name]=i;world.push_back(parent*local[i]);skin_matrices.push_back(world.back()*transform(binds[i]).affine_inverse());
		}
		const auto mesh_joints=mesh.get_joints();
		std::vector<int> mapping;
		if(!mesh_joints.empty())for(const auto &j:mesh_joints) {ERR_FAIL_COND_V(!order.count(j.str()),ERR_INVALID_DATA);mapping.push_back(order[j.str()]);}
		else for(size_t i=0;i<joints.size();++i)mapping.push_back(i);
		u::Attribute geom;Transform3D geom_bind;
		if(u::tydra::GetAttribute(*node.prim,"primvars:skel:geomBindTransform",&geom,&err)) {auto m=geom.get_value<u::value::matrix4d>();ERR_FAIL_COND_V(!m,ERR_INVALID_DATA);geom_bind=transform(*m);}
		const Transform3D inverse_bind=geom_bind.affine_inverse();
		const bool vertex_normals=normals.size()==points.size();
		for(size_t i=0;i<points.size();++i) {
			Vector3 p,n;float sum=0;
			for(int k=0;k<stride;++k) {const auto slot=i*stride+k;const int joint=indices[slot];ERR_FAIL_COND_V(joint<0 || joint>=int(mapping.size()) || !Math::is_finite(weights[slot]) || weights[slot]<0,ERR_INVALID_DATA);const Transform3D t=inverse_bind*skin_matrices[mapping[joint]]*geom_bind;p+=t.xform(vector(points[i]))*weights[slot];if(vertex_normals)n+=t.basis.xform(vector(normals[i]))*weights[slot];sum+=weights[slot];}
			ERR_FAIL_COND_V(sum<.0001,ERR_INVALID_DATA);p/=sum;points[i]={float(p.x),float(p.y),float(p.z)};
			if(vertex_normals) {n.normalize();normals[i]={float(n.x),float(n.y),float(n.z)};}
		}
		return OK;
	}
	Error mesh(const XformNode &node,std::array<Surface,2> &out) {
		const auto &m=*node.prim->as<u::GeomMesh>();auto points=m.get_points();auto normals=m.get_normals();const auto counts=m.get_faceVertexCounts(),indices=m.get_faceVertexIndices();
		ERR_FAIL_COND_V(points.empty() || points.size()>16000000 || indices.size()>48000000,ERR_INVALID_DATA);
		Error e=skin(node,m,points,normals);ERR_FAIL_COND_V(e!=OK,e);
		u::GeomPrimvar uvvar;std::vector<u::value::texcoord2f> uv;std::string err;
		if(u::tydra::GetGeomPrimvar(stage,&m,"st",&uvvar,&err))ERR_FAIL_COND_V(!uvvar.flatten_with_indices(&uv,&err),ERR_INVALID_DATA);
		std::vector<int> role(counts.size(),0);
		for(const auto &child:node.prim->children()) if(const auto *subset=child.as<u::GeomSubset>()) {
			std::vector<int> faces;ERR_FAIL_COND_V(!sample(subset->indices,faces),ERR_INVALID_DATA);
			const int r=String::utf8(subset->name.c_str()).to_lower().contains("twosided")?1:0;
			for(const int f:faces) {ERR_FAIL_COND_V(f<0 || f>=int(role.size()),ERR_INVALID_DATA);role[f]=r;}
		}
		std::map<std::tuple<int,size_t,size_t>,unsigned int> remap[2];size_t corner=0;
		for(size_t face=0;face<counts.size();++face) {
			const int count=counts[face];ERR_FAIL_COND_V(count<3 || count>4 || corner+count>indices.size(),ERR_UNAVAILABLE);
			Surface &s=out[role[face]];std::vector<unsigned int> polygon;
			for(int k=0;k<count;++k) {
				const size_t c=corner+k;const int v=indices[c];ERR_FAIL_COND_V(v<0 || v>=int(points.size()),ERR_INVALID_DATA);
				const size_t ni=normals.size()==indices.size()?c:(normals.size()==points.size()?v:(normals.size()==counts.size()?face:0));
				const size_t ti=uv.size()==indices.size()?c:(uv.size()==points.size()?v:0);
				const auto key=std::make_tuple(v,ni,ti);auto it=remap[role[face]].find(key);
				if(it==remap[role[face]].end()) {const unsigned int next=s.points.size();remap[role[face]][key]=next;polygon.push_back(next);s.points.push_back(vector(points[v]));s.normals.push_back(normals.empty()?Vector3():vector(normals[ni]));s.uv.push_back(uv.empty()?Vector2():Vector2(uv[ti][0],1.0f-uv[ti][1]));}else polygon.push_back(it->second);
			}
			// USD right-handed winding is opposite Godot's clockwise triangles.
			for(int k=1;k<count-1;++k) {s.indices.push_back(polygon[0]);s.indices.push_back(polygon[k+1]);s.indices.push_back(polygon[k]);}
			corner+=count;source_triangles+=count-2;
		}
		ERR_FAIL_COND_V(corner!=indices.size(),ERR_INVALID_DATA);
		if(normals.empty())for(auto &s:out) {for(size_t i=0;i<s.indices.size();i+=3) {const auto a=s.indices[i],b=s.indices[i+1],c=s.indices[i+2];const Vector3 n=(s.points[c]-s.points[a]).cross(s.points[b]-s.points[a]).normalized();s.normals[a]+=n;s.normals[b]+=n;s.normals[c]+=n;}for(auto &n:s.normals)n.normalize();}
		return OK;
	}
	Error branch(const XformNode &root, const Transform3D &relative, std::array<Surface, 2> &out) {
		if (root.prim && (root.prim->specifier() == u::Specifier::Class || root.prim->element_name() == "Prototypes")) {
			return OK;
		}
		if (root.prim && root.prim->as<u::PointInstancer>()) {
			return expand(root, relative, out, false);
		}
		if (root.prim && root.prim->as<u::GeomMesh>()) {
			std::array<Surface, 2> raw;
			Error e = mesh(root, raw);
			ERR_FAIL_COND_V(e != OK, e);
			for (int r = 0; r < 2; ++r) {
				raw[r].reduce(prototype_budget);
				e = out[r].append(raw[r], relative * transform(root.get_world_matrix()));
				ERR_FAIL_COND_V(e != OK, e);
			}
		}
		for (const auto &child : root.children) {
			Error e = branch(child, relative, out);
			ERR_FAIL_COND_V(e != OK, e);
		}
		return OK;
	}
	std::set<std::string> active_prototypes;
	Error expand(const XformNode &node, const Transform3D &relative, std::array<Surface, 2> &out, bool authored) {
		const auto *inst = node.prim->as<u::PointInstancer>();
		ERR_FAIL_COND_V(!inst || !inst->prototypes, ERR_INVALID_DATA);
		const auto paths = targets(*inst->prototypes);
		std::vector<int> ids;
		std::vector<u::value::point3f> p;
		std::vector<u::value::quath> q;
		std::vector<u::value::float3> sc;
		ERR_FAIL_COND_V(!sample(inst->protoIndices, ids) || !sample(inst->positions, p) || ids.size() != p.size() || ids.size() > 20000, ERR_INVALID_DATA);
		sample(inst->orientations, q);
		sample(inst->scales, sc);
		ERR_FAIL_COND_V(!q.empty() && q.size() != p.size() || !sc.empty() && sc.size() != p.size(), ERR_INVALID_DATA);
		for (const auto &prototype : paths) {
			const auto key = prototype.full_path_name();
			ERR_FAIL_COND_V(!nodes.count(key), ERR_INVALID_DATA);
			if (!prototypes.count(key)) {
				ERR_FAIL_COND_V(active_prototypes.count(key) || active_prototypes.size() >= 32, ERR_INVALID_DATA);
				active_prototypes.insert(key);
				std::array<Surface, 2> raw;
				Error e = branch(*nodes.at(key), transform(nodes.at(key)->get_world_matrix()).affine_inverse(), raw);
				ERR_FAIL_COND_V(e != OK, e);
				for (auto &surface : raw) {
					surface.reduce(prototype_budget);
				}
				active_prototypes.erase(key);
				prototypes[key] = std::move(raw);
			}
		}
		for (size_t i = 0; i < ids.size(); ++i) {
			ERR_FAIL_COND_V(ids[i] < 0 || ids[i] >= int(paths.size()), ERR_INVALID_DATA);
			Basis basis;
			if (!q.empty()) {
				basis = Basis(Quaternion(u::value::half_to_float(q[i][0]), u::value::half_to_float(q[i][1]), u::value::half_to_float(q[i][2]), u::value::half_to_float(q[i][3])).normalized());
			}
			if (!sc.empty()) {
				basis.scale(vector(sc[i]));
			}
			const Transform3D placement = relative * transform(node.get_world_matrix()) * Transform3D(basis, vector(p[i]));
			const auto &source = prototypes.at(paths[ids[i]].full_path_name());
			for (int r = 0; r < 2; ++r) {
				Error e = out[r].append(source[r], placement);
				ERR_FAIL_COND_V(e != OK, e);
			}
			if (authored) {
				++instance_count;
			}
		}
		return OK;
	}
	Error visit(const XformNode &node) {
		if (!node.prim) {
			for (const auto &child : node.children) {
				Error e = visit(child);
				if (e != OK) {
					return e;
				}
			}
			return OK;
		}
		const std::string path = node.absolute_path.full_path_name();
		if (library_roots.count(path)) {
			return OK;
		}
		// Class libraries supply prototype geometry but are not visible objects.
		if (node.prim->specifier() == u::Specifier::Class) {
			return OK;
		}
		u::Attribute visibility;
		std::string visibility_error;
		if (u::tydra::GetAttribute(*node.prim, "visibility", &visibility, &visibility_error)) {
			auto value = visibility.get_value<u::value::token>();
			if (value && value->str() == "invisible") {
				return OK;
			}
		}
		if (path.find("/Prototypes/") != std::string::npos || path.size() >= 11 && path.compare(path.size() - 11, 11, "/Prototypes") == 0) {
			return OK;
		}
		if (node.prim->as<u::PointInstancer>()) {
			return expand(node, Transform3D(), surfaces, true);
		}
		if (node.prim->as<u::GeomMesh>()) {
			std::array<Surface, 2> raw;
			Error e = mesh(node, raw);
			ERR_FAIL_COND_V(e != OK, e);
			for (int r = 0; r < 2; ++r) {
				raw[r].reduce(r == 0 ? trunk_budget : leaf_budget);
				e = surfaces[r].append(raw[r], transform(node.get_world_matrix()));
				ERR_FAIL_COND_V(e != OK, e);
			}
		}
		for (const auto &child : node.children) {
			Error e = visit(child);
			if (e != OK) {
				return e;
			}
		}
		return OK;
	}
};
// PackedScene imports retain top-level instances and bounded prototype geometry.
// The ordinary ArrayMesh transport below deliberately remains a bounded flatten.
struct ScenePart {
	Ref<ArrayMesh> mesh;
	Transform3D transform;
};

struct SceneImport {
	static constexpr size_t MAX_PARTS = 1000000;
	static constexpr size_t MAX_PROTOTYPE_VERTICES = 4000000;
	Import &source;
	std::array<Ref<Material>, 2> materials;
	std::map<std::string, Ref<ArrayMesh>> meshes;
	std::map<std::array<uint8_t, 32>, Ref<ArrayMesh>> geometry_values;
	std::map<std::array<uint8_t, 32>, Ref<ArrayMesh>> combined_geometry_values;
	std::map<ObjectID, std::array<Surface, 2>> source_surfaces;
	std::map<std::string, std::vector<ScenePart>> prototypes;
	std::map<std::string, std::vector<ScenePart>> render_prototypes;
	std::set<std::string> active;
	uint64_t unique_triangles[2] = {};
	uint64_t instanced_triangles[2] = {};
	uint64_t authored_triangles[2] = {};
	uint64_t zero_area_triangles = 0;
	uint64_t reused_mesh_values = 0;
	uint64_t combined_zero_area_triangles = 0;
	uint64_t reused_combined_mesh_values = 0;
	uint64_t combined_unique_triangles = 0;
	uint64_t combined_prototype_parts = 0;
	uint64_t render_instances = 0;
	int authored_instances = 0;
	uint64_t visible_point_instances = 0;
	bool asset_coordinates = false;
	double target_height = 0;
	bool rooted = false;

	explicit SceneImport(Import &p_source) :
			source(p_source) {}

	Error load_materials(const Dictionary &p_overrides) {
		for (int role = 0; role < 2; ++role) {
			const String name = role == 0 ? "bark" : "leaves";
			if (p_overrides.has(name)) {
				const String path = p_overrides[name];
				HashSet<String> closure;
				const Error error = CookerFiles::collect(path, closure);
				ERR_FAIL_COND_V(error != OK, error);
				materials[role] = ResourceLoader::load(path);
				ERR_FAIL_COND_V(materials[role].is_null(), ERR_INVALID_DATA);
			} else {
				Ref<StandardMaterial3D> material;
				material.instantiate();
				material->set_albedo(role == 0 ? Color(.24, .16, .09) : Color(.2, .38, .12));
				material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
				material->set_roughness(.8);
				material->set_name(name);
				materials[role] = material;
			}
		}
		return OK;
	}

	Error geometry_value(const std::string &p_key, std::array<Surface, 2> &surfaces, Ref<ArrayMesh> &r_mesh, bool p_source) {
		auto &values = p_source ? geometry_values : combined_geometry_values;
		uint64_t &removed = p_source ? zero_area_triangles : combined_zero_area_triangles;
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->set_name(String::utf8(p_key.c_str()));
		CryptoCore::SHA256Context content;
		ERR_FAIL_COND_V(content.start() != OK, FAILED);
		for (int role = 0; role < 2; ++role) {
			auto &surface = surfaces[role];
			size_t kept = 0;
			for (size_t triangle = 0; triangle < surface.indices.size(); triangle += 3) {
				const Vector3 &a = surface.points[surface.indices[triangle]];
				const Vector3 &b = surface.points[surface.indices[triangle + 1]];
				const Vector3 &c = surface.points[surface.indices[triangle + 2]];
				ERR_FAIL_COND_V(!a.is_finite() || !b.is_finite() || !c.is_finite(), ERR_INVALID_DATA);
				std::array<double, 3> ab, ac;
				for (int axis = 0; axis < 3; ++axis) {
					ab[axis] = double(b[axis]) - a[axis];
					ac[axis] = double(c[axis]) - a[axis];
				}
				if (ab[1] * ac[2] == ab[2] * ac[1] && ab[2] * ac[0] == ab[0] * ac[2] && ab[0] * ac[1] == ab[1] * ac[0]) {
					++removed;
					continue;
				}
				for (int corner = 0; corner < 3; ++corner) {
					surface.indices[kept++] = surface.indices[triangle + corner];
				}
			}
			surface.indices.resize(kept);
			if (surface.indices.empty()) {
				continue;
			}
			const Error welded = surface.weld_values();
			ERR_FAIL_COND_V(welded != OK, welded);
			// This key is local to one import, never a file/protocol identity.
			// The role keeps equal bark/leaf values with different bindings apart.
			static_assert(sizeof(Vector3) == sizeof(float) * 3 && sizeof(Vector2) == sizeof(float) * 2);
			const std::array<uint64_t, 5> header = { uint64_t(role), surface.points.size(), surface.normals.size(), surface.uv.size(), surface.indices.size() };
			ERR_FAIL_COND_V(content.update(reinterpret_cast<const uint8_t *>(header.data()), sizeof(header)) != OK ||
							content.update(reinterpret_cast<const uint8_t *>(surface.points.data()), surface.points.size() * sizeof(Vector3)) != OK ||
							content.update(reinterpret_cast<const uint8_t *>(surface.normals.data()), surface.normals.size() * sizeof(Vector3)) != OK ||
							content.update(reinterpret_cast<const uint8_t *>(surface.uv.data()), surface.uv.size() * sizeof(Vector2)) != OK ||
							content.update(reinterpret_cast<const uint8_t *>(surface.indices.data()), surface.indices.size() * sizeof(unsigned int)) != OK,
					FAILED);
			const Array arrays = surface.arrays();
			ERR_FAIL_COND_V(arrays.size() != Mesh::ARRAY_MAX, ERR_OUT_OF_MEMORY);
			const int slot = mesh->get_surface_count();
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
			ERR_FAIL_COND_V(mesh->get_surface_count() != slot + 1, ERR_INVALID_DATA);
			mesh->surface_set_material(slot, materials[role]);
			mesh->surface_set_name(slot, role == 0 ? "bark" : "leaves");
		}
		if (mesh->get_surface_count() == 0) {
			mesh.unref(); // A zero-area prototype contributes no visible coverage.
		}
		std::array<uint8_t, 32> digest;
		ERR_FAIL_COND_V(content.finish(digest.data()) != OK, FAILED);
		const auto same_value = values.find(digest);
		if (same_value != values.end()) {
			mesh = same_value->second;
			++(p_source ? reused_mesh_values : reused_combined_mesh_values);
		} else {
			ERR_FAIL_COND_V(values.size() >= 65536, ERR_OUT_OF_MEMORY);
			values.emplace(digest, mesh);
			for (int role = 0; role < 2; ++role) {
				if (p_source) {
					unique_triangles[role] += surfaces[role].indices.size() / 3;
				} else {
					combined_unique_triangles += surfaces[role].indices.size() / 3;
				}
			}
		}
		r_mesh = mesh;
		return OK;
	}

	Error geometry(const XformNode &p_node, Ref<ArrayMesh> &r_mesh) {
		const std::string key = p_node.absolute_path.full_path_name();
		const auto cached = meshes.find(key);
		if (cached != meshes.end()) {
			r_mesh = cached->second;
			return OK;
		}
		std::array<Surface, 2> surfaces;
		Error error = source.mesh(p_node, surfaces);
		ERR_FAIL_COND_V(error != OK, error);
		error = geometry_value(key, surfaces, r_mesh, true);
		ERR_FAIL_COND_V(error != OK, error);
		meshes.emplace(key, r_mesh);
		return OK;
	}

	Error render_prototype(const std::string &p_key) {
		if (render_prototypes.count(p_key)) {
			return OK;
		}
		const auto &parts = prototypes.at(p_key);
		if (parts.size() <= 1) {
			render_prototypes.emplace(p_key, parts);
			return OK;
		}
		// Keep authored top-level instances. Bake their nested static parts once
		// into bounded prototype chunks, instead of replicating a leaf-instance
		// list for every placed tree. There is no triangle reduction here.
		std::array<Surface, 2> combined;
		size_t vertices = 0, indices = 0;
		std::vector<ScenePart> chunks;
		auto flush = [&]() -> Error {
			if (vertices == 0) {
				return OK;
			}
			Ref<ArrayMesh> mesh;
			const Error error = geometry_value(p_key + "/RenderPart" + std::to_string(chunks.size()), combined, mesh, false);
			if (error != OK) {
				return error;
			}
			if (mesh.is_valid()) {
				chunks.push_back({ mesh, Transform3D() });
			}
			combined = {};
			vertices = indices = 0;
			return OK;
		};
		for (const auto &part : parts) {
			const ObjectID id = part.mesh->get_instance_id();
			auto cached = source_surfaces.find(id);
			if (cached == source_surfaces.end()) {
				std::array<Surface, 2> surfaces;
				for (int slot = 0; slot < part.mesh->get_surface_count(); ++slot) {
					const int role = part.mesh->surface_get_name(slot) == "leaves" ? 1 : 0;
					auto &surface = surfaces[role];
					const Array arrays = part.mesh->surface_get_arrays(slot);
					ERR_FAIL_COND_V(arrays.size() != Mesh::ARRAY_MAX, ERR_INVALID_DATA);
					const PackedVector3Array points = arrays[Mesh::ARRAY_VERTEX], normals = arrays[Mesh::ARRAY_NORMAL];
					const PackedVector2Array uv = arrays[Mesh::ARRAY_TEX_UV];
					const PackedInt32Array triangles = arrays[Mesh::ARRAY_INDEX];
					ERR_FAIL_COND_V(points.size() != normals.size() || points.size() != uv.size(), ERR_INVALID_DATA);
					surface.points.assign(points.ptr(), points.ptr() + points.size());
					surface.normals.assign(normals.ptr(), normals.ptr() + normals.size());
					surface.uv.assign(uv.ptr(), uv.ptr() + uv.size());
					surface.indices.assign(triangles.ptr(), triangles.ptr() + triangles.size());
				}
				cached = source_surfaces.emplace(id, std::move(surfaces)).first;
			}
			size_t part_vertices = 0, part_indices = 0;
			for (const auto &surface : cached->second) {
				part_vertices += surface.points.size();
				part_indices += surface.indices.size();
			}
			ERR_FAIL_COND_V(part_vertices > MAX_PROTOTYPE_VERTICES || part_indices > Surface::MAX_INDICES, ERR_OUT_OF_MEMORY);
			if (part_vertices > MAX_PROTOTYPE_VERTICES - vertices || part_indices > Surface::MAX_INDICES - indices) {
				const Error error = flush();
				ERR_FAIL_COND_V(error != OK, error);
			}
			for (int role = 0; role < 2; ++role) {
				const Error error = combined[role].append(cached->second[role], part.transform);
				ERR_FAIL_COND_V(error != OK, error);
			}
			vertices += part_vertices;
			indices += part_indices;
		}
		const Error error = flush();
		ERR_FAIL_COND_V(error != OK, error);
		combined_prototype_parts += parts.size();
		render_prototypes.emplace(p_key, std::move(chunks));
		return OK;
	}

	Error append(std::vector<ScenePart> &r_parts, const ScenePart &p_part, const Transform3D &p_transform) {
		ERR_FAIL_COND_V(r_parts.size() >= MAX_PARTS, ERR_OUT_OF_MEMORY);
		const Transform3D transform = p_transform * p_part.transform;
		const real_t determinant = transform.basis.determinant();
		ERR_FAIL_COND_V(!transform.is_finite() || !Math::is_finite(determinant) || Math::abs(determinant) < 1e-12, ERR_INVALID_DATA);
		r_parts.push_back({ p_part.mesh, transform });
		return OK;
	}

	void account_authored_triangles(const Ref<ArrayMesh> &p_mesh) {
		for (int slot = 0; slot < p_mesh->get_surface_count(); ++slot) {
			const int role = p_mesh->surface_get_name(slot) == "leaves" ? 1 : 0;
			authored_triangles[role] += p_mesh->surface_get_array_index_len(slot) / 3;
		}
	}

	Error branch(const XformNode &p_node, const Transform3D &p_relative, std::vector<ScenePart> &r_parts, bool p_visible) {
		if (p_node.prim != nullptr) {
			const std::string path = p_node.absolute_path.full_path_name();
			if (p_node.prim->specifier() == u::Specifier::Class ||
					(p_visible && (source.library_roots.count(path) || path.find("/Prototypes/") != std::string::npos || p_node.prim->element_name() == "Prototypes"))) {
				return OK;
			}
			u::Attribute visibility;
			std::string visibility_error;
			if (u::tydra::GetAttribute(*p_node.prim, "visibility", &visibility, &visibility_error)) {
				const auto value = visibility.get_value<u::value::token>();
				if (value && value->str() == "invisible") {
					return OK;
				}
			}
			if (const auto *instancer = p_node.prim->as<u::PointInstancer>()) {
				ERR_FAIL_COND_V(!instancer->prototypes, ERR_INVALID_DATA);
				const auto paths = targets(*instancer->prototypes);
				std::vector<int> ids;
				std::vector<u::value::point3f> positions;
				std::vector<u::value::quath> orientations;
				std::vector<u::value::float3> scales;
				std::vector<int64_t> instance_ids, invisible_ids;
				ERR_FAIL_COND_V(!sample(instancer->protoIndices, ids) || !sample(instancer->positions, positions) || ids.size() != positions.size() || ids.size() > 20000, ERR_INVALID_DATA);
				sample(instancer->orientations, orientations);
				sample(instancer->scales, scales);
				sample(instancer->ids, instance_ids);
				sample(instancer->invisibleIds, invisible_ids);
				ERR_FAIL_COND_V((!orientations.empty() && orientations.size() != ids.size()) || (!scales.empty() && scales.size() != ids.size()), ERR_INVALID_DATA);
				ERR_FAIL_COND_V(!instance_ids.empty() && instance_ids.size() != ids.size(), ERR_INVALID_DATA);
				const std::set<int64_t> hidden(invisible_ids.begin(), invisible_ids.end());
				for (const auto &prototype : paths) {
					const auto key = prototype.full_path_name();
					ERR_FAIL_COND_V(!source.nodes.count(key), ERR_INVALID_DATA);
					if (prototypes.count(key)) {
						continue;
					}
					ERR_FAIL_COND_V(active.count(key) || active.size() >= 32, ERR_INVALID_DATA);
					active.insert(key);
					const XformNode &root = *source.nodes.at(key);
					const Transform3D world = transform(root.get_world_matrix());
					ERR_FAIL_COND_V(!world.is_finite() || !Math::is_finite(world.basis.determinant()) || Math::abs(world.basis.determinant()) < 1e-12, ERR_INVALID_DATA);
					std::vector<ScenePart> parts;
					// USD applies the prototype root's local transform before its
					// instance SRT; ancestors of the prototype library do not apply.
					const Transform3D relative = transform(root.get_local_matrix()) * world.affine_inverse();
					const Error error = branch(root, relative, parts, false);
					ERR_FAIL_COND_V(error != OK, error);
					active.erase(key);
					prototypes.emplace(key, std::move(parts));
				}
				for (size_t index = 0; index < ids.size(); ++index) {
					ERR_FAIL_COND_V(ids[index] < 0 || ids[index] >= int(paths.size()), ERR_INVALID_DATA);
					if (hidden.count(instance_ids.empty() ? int64_t(index) : instance_ids[index])) {
						continue;
					}
					Basis basis;
					if (!orientations.empty()) {
						const auto &q = orientations[index];
						const Quaternion rotation(u::value::half_to_float(q[0]), u::value::half_to_float(q[1]), u::value::half_to_float(q[2]), u::value::half_to_float(q[3]));
						ERR_FAIL_COND_V(!rotation.is_finite() || rotation.length_squared() < 1e-12, ERR_INVALID_DATA);
						basis = Basis(rotation.normalized());
					}
					if (!scales.empty()) {
						basis.scale(vector(scales[index]));
					}
					const Transform3D placement = p_relative * transform(p_node.get_world_matrix()) * Transform3D(basis, vector(positions[index]));
					const std::string key = paths[ids[index]].full_path_name();
					if (p_visible) {
						for (const auto &part : prototypes.at(key)) {
							account_authored_triangles(part.mesh);
						}
					}
					if (p_visible && asset_coordinates) {
						const Error error = render_prototype(key);
						ERR_FAIL_COND_V(error != OK, error);
					}
					const auto &parts = p_visible && asset_coordinates ? render_prototypes.at(key) : prototypes.at(key);
					for (const auto &part : parts) {
						const Error error = append(r_parts, part, placement);
						ERR_FAIL_COND_V(error != OK, error);
					}
					if (p_visible) {
						++visible_point_instances;
					}
				}
				if (p_visible) {
					authored_instances += ids.size();
				}
				return OK;
			}
			if (p_node.prim->as<u::GeomMesh>()) {
				Ref<ArrayMesh> mesh;
				Error error = geometry(p_node, mesh);
				ERR_FAIL_COND_V(error != OK, error);
				if (mesh.is_valid()) {
					if (p_visible) {
						account_authored_triangles(mesh);
					}
					error = append(r_parts, { mesh, Transform3D() }, p_relative * transform(p_node.get_world_matrix()));
					ERR_FAIL_COND_V(error != OK, error);
				}
			}
		}
		for (const auto &child : p_node.children) {
			const Error error = branch(child, p_relative, r_parts, p_visible);
			ERR_FAIL_COND_V(error != OK, error);
		}
		return OK;
	}

	Error pack(const std::vector<ScenePart> &p_parts, Ref<PackedScene> &r_scene) {
		ERR_FAIL_COND_V(p_parts.empty(), ERR_INVALID_DATA);
		render_instances = p_parts.size();
		struct Batch {
			Ref<ArrayMesh> mesh;
			std::vector<Transform3D> placements;
		};
		std::vector<Batch> batches;
		std::map<ObjectID, size_t> slots;
		AABB bounds;
		bool has_bounds = false;
		for (const auto &part : p_parts) {
			ERR_FAIL_COND_V(asset_coordinates && !part.transform.affine_inverse().is_finite(), ERR_INVALID_DATA);
			const AABB part_bounds = part.transform.xform(part.mesh->get_aabb());
			ERR_FAIL_COND_V(!part_bounds.position.is_finite() || !part_bounds.size.is_finite(), ERR_INVALID_DATA);
			bounds = has_bounds ? bounds.merge(part_bounds) : part_bounds;
			has_bounds = true;
			const auto [slot, added] = slots.emplace(part.mesh->get_instance_id(), batches.size());
			if (added) {
				batches.push_back({ part.mesh, {} });
			}
			batches[slot->second].placements.push_back(part.transform);
			for (int surface = 0; surface < part.mesh->get_surface_count(); ++surface) {
				const int role = part.mesh->surface_get_name(surface) == "leaves" ? 1 : 0;
				instanced_triangles[role] += part.mesh->surface_get_array_index_len(surface) / 3;
			}
		}
		ERR_FAIL_COND_V_MSG(authored_triangles[0] != instanced_triangles[0] || authored_triangles[1] != instanced_triangles[1], ERR_INVALID_DATA,
				"USD prototype grouping changed the complete source triangle coverage.");
		ERR_FAIL_COND_V(!bounds.position.is_finite() || !bounds.size.is_finite() || !bounds.get_end().is_finite(), ERR_INVALID_DATA);
		Transform3D normalization;
		if (target_height > 0) {
			ERR_FAIL_COND_V(bounds.size.y <= 1e-6 || !Math::is_finite(bounds.size.y), ERR_INVALID_DATA);
			const real_t scale = target_height / bounds.size.y;
			normalization.basis.scale(Vector3(scale, scale, scale));
			normalization.origin = Vector3(rooted ? 0 : -bounds.get_center().x * scale, -bounds.position.y * scale, rooted ? 0 : -bounds.get_center().z * scale);
			ERR_FAIL_COND_V(!normalization.is_finite() || !Math::is_finite(normalization.basis.determinant()) || normalization.basis.determinant() == 0, ERR_INVALID_DATA);
		}
		if (asset_coordinates) {
			for (const auto &part : p_parts) {
				ERR_FAIL_COND_V(!(normalization * part.transform).affine_inverse().is_finite(), ERR_INVALID_DATA);
			}
		}
		Node3D *root = memnew(Node3D);
		root->set_name("SourceTree");
		root->set_unique_scene_id(1);
		auto *geometry = memnew(Node3D);
		geometry->set_name("Geometry");
		geometry->set_transform(normalization);
		root->add_child(geometry);
		geometry->set_owner(root);
		geometry->set_unique_scene_id(2);
		for (size_t slot = 0; slot < batches.size(); ++slot) {
			const auto &batch = batches[slot];
			Ref<MultiMesh> multimesh;
			multimesh.instantiate();
			multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
			multimesh->set_use_colors(asset_coordinates);
			multimesh->set_use_custom_data(asset_coordinates);
			multimesh->set_mesh(batch.mesh);
			multimesh->set_instance_count(batch.placements.size());
			for (size_t index = 0; index < batch.placements.size(); ++index) {
				const Transform3D &placement = batch.placements[index];
				multimesh->set_instance_transform(index, placement);
				if (asset_coordinates) {
					// An opted-in shader receives asset-space height coefficients
					// and the asset origin in prototype coordinates. Both remain
					// valid when a later scene composes the full instance basis.
					const Transform3D asset_placement = normalization * placement;
					const Vector3 height(asset_placement.basis.get_column(0).y, asset_placement.basis.get_column(1).y, asset_placement.basis.get_column(2).y);
					const Vector3 origin = asset_placement.affine_inverse().origin;
					multimesh->set_instance_color(index, Color(height.x, height.y, height.z, asset_placement.origin.y));
					multimesh->set_instance_custom_data(index, Color(origin.x, origin.y, origin.z, 1));
				}
			}
			auto *instance = memnew(MultiMeshInstance3D);
			instance->set_name("Prototype" + itos(slot));
			instance->set_multimesh(multimesh);
			geometry->add_child(instance);
			instance->set_owner(root);
			instance->set_unique_scene_id(slot + 3);
		}
		Ref<PackedScene> scene;
		scene.instantiate();
		const Error error = scene->pack(root);
		memdelete(root);
		if (error == OK) {
			r_scene = scene;
		}
		return error;
	}
};

int budget(const Dictionary &options, const String &key, int fallback) {
	const Variant v = options.get(key, fallback);
	if ((v.get_type() != Variant::INT && v.get_type() != Variant::FLOAT) || !Math::is_finite(double(v)) || double(v) != Math::floor(double(v)) || (double(v) != 0 && double(v) < 32) || double(v) > 500000) {
		return -1;
	}
	return int(v);
}
} //namespace
Error dependencies(const String &source, HashSet<String> &files) {
	HashSet<String> pending;
	return gather(source, files, pending);
}
Error import_scene(const Dictionary &job, Dictionary &result) {
	const String source = job.get("source", ""), output = job.get("output", ""), type = job.get("type", "PackedScene");
	ERR_FAIL_COND_V(type != "ArrayMesh" && type != "PackedScene", ERR_UNAVAILABLE);
	HashSet<String> files;
	Error e = dependencies(source, files);
	ERR_FAIL_COND_V(e != OK, e);
	u::Stage authored;
	e = load(source, authored, false);
	ERR_FAIL_COND_V(e != OK, e);
	int authored_instances = 0;
	std::set<std::string> library_roots;
	std::function<void(const u::Prim &, const std::string &)> count_instances = [&](const u::Prim &prim, const std::string &parent) {
		const std::string path=parent+"/"+prim.element_name();
		if(const auto *inst=prim.as<u::PointInstancer>()) {
			std::vector<int> ids;
			if (sample(inst->protoIndices, ids))
				authored_instances += ids.size();
		}
		// Some source packages author their reference libraries as `over`, not
		// `class`. They still only supply instancer prototypes, not scenery.
		if(path.find("/Prototypes/")!=std::string::npos && prim.metas().references) {
			for(const auto &reference:prim.metas().references->second)
				if (reference.asset_path.GetAssetPath().empty()) {
					const auto target = reference.prim_path.full_path_name();
					const auto slash = target.rfind('/');
					if (slash != std::string::npos && slash > 0)
						library_roots.insert(target.substr(0, slash));
				}
		}
		for(const auto &child:prim.children())
			count_instances(child, path);
	};
	for (const auto &prim : authored.root_prims()) {
		count_instances(prim, "");
	}
	Import importer;
	importer.library_roots = std::move(library_roots);
	e = load(source, importer.stage, true);
	ERR_FAIL_COND_V(e != OK, e);
	ERR_FAIL_COND_V(importer.stage.metas().upAxis.get_value() != u::Axis::Y || importer.stage.metas().metersPerUnit.get_value() != 1.0, ERR_UNAVAILABLE);
	const Dictionary options = job.get("options", Dictionary());
	for (const Variant &key : options.keys()) {
		ERR_FAIL_COND_V(key != "prototype_triangles" && key != "bark_triangles" && key != "leaf_triangles" && key != "virtual_geometry" && key != "preserve_area" && key != "asset_coordinates" && key != "height" && key != "rooted", ERR_INVALID_PARAMETER);
	}
	const bool runtime_scene = type == "PackedScene";
	for (const String &key : { String("asset_coordinates"), String("rooted") }) {
		ERR_FAIL_COND_V(options.has(key) && (!runtime_scene || options[key].get_type() != Variant::BOOL), ERR_INVALID_PARAMETER);
	}
	CookerVirtualGeometry::Settings settings;
	bool explicit_geometry = false;
	e = CookerVirtualGeometry::parse_import_settings(options, runtime_scene, settings, explicit_geometry);
	ERR_FAIL_COND_V(e != OK, e);
	const Variant height = options.get("height", 0.0);
	ERR_FAIL_COND_V(options.has("height") && (!runtime_scene || (height.get_type() != Variant::INT && height.get_type() != Variant::FLOAT) || !Math::is_finite(double(height)) || double(height) <= 0 || double(height) > 100000), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(options.has("rooted") && !options.has("height"), ERR_INVALID_PARAMETER);
	importer.prototype_budget = budget(options, "prototype_triangles", runtime_scene ? 0 : 1200);
	importer.trunk_budget = budget(options, "bark_triangles", runtime_scene ? 0 : 6000);
	importer.leaf_budget = budget(options, "leaf_triangles", runtime_scene ? 0 : 40000);
	ERR_FAIL_COND_V(importer.prototype_budget < 0 || importer.trunk_budget < 0 || importer.leaf_budget < 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(!u::tydra::BuildXformNodeFromStage(importer.stage, &importer.hierarchy, u::value::TimeCode::Default(), u::value::TimeSampleInterpolationType::Linear), ERR_INVALID_DATA, "USD transform hierarchy could not be evaluated.");
	// Tydra copies its temporary hierarchy; repair ancestor pointers only after
	// the final vector storage is stable. Never retain pointers into temporaries.
	std::function<void(XformNode &, XformNode *)> index = [&](XformNode &n, XformNode *parent) {
		n.parent = parent;
		importer.nodes[n.absolute_path.full_path_name()] = &n;
		for (auto &child : n.children) {
			index(child, &n);
		}
	};
	index(importer.hierarchy, nullptr);
	if (runtime_scene) {
		try {
			ERR_FAIL_COND_V_MSG(importer.prototype_budget != 0 || importer.trunk_budget != 0 || importer.leaf_budget != 0, ERR_INVALID_PARAMETER,
					"USD PackedScene import preserves complete prototypes. Explicit triangle budgets belong to ArrayMesh source transport.");
			SceneImport shared(importer);
			shared.asset_coordinates = options.get("asset_coordinates", false);
			shared.target_height = height;
			shared.rooted = options.get("rooted", false);
			e = shared.load_materials(job.get("material_overrides", Dictionary()));
			ERR_FAIL_COND_V(e != OK, e);
			std::vector<ScenePart> parts;
			e = shared.branch(importer.hierarchy, Transform3D(), parts, true);
			ERR_FAIL_COND_V(e != OK, e);
			ERR_FAIL_COND_V_MSG(authored_instances != shared.authored_instances, ERR_INVALID_DATA, "USD authored branch count differs from composed visible count.");
			Ref<PackedScene> scene;
			e = shared.pack(parts, scene);
			ERR_FAIL_COND_V(e != OK, e);
			Ref<Resource> prepared;
			std::map<ObjectID, CookerVirtualGeometry::BuildOutput> builds;
			Dictionary report;
			String message;
			// Composed USD models retain every unique prototype. Their aggregate
			// page output is separate from the ordinary recipe/fragment allowance.
			settings.max_source_vertices = SceneImport::MAX_PROTOTYPE_VERTICES;
			settings.max_output_bytes = 1024ull * 1024 * 1024;
			e = CookerVirtualGeometry::convert_imported_scene(scene, prepared, builds, report, message, explicit_geometry ? &settings : nullptr);
			ERR_FAIL_COND_V_MSG(e != OK, e, message);
			e = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));
			ERR_FAIL_COND_V(e != OK, e);
			CookerTextures::Builder textures;
			Variant normalized = prepared;
			e = textures.Normalize(normalized);
			ERR_FAIL_COND_V(e != OK, e);
			Array render_outputs;
			const Ref<PackedScene> published_scene = prepared;
			e = CookerRender::save(published_scene, output.get_base_dir(), render_outputs, settings.max_output_bytes, &builds);
			ERR_FAIL_COND_V(e != OK, e);
			e = CookerAtomicSave::save_resource(prepared, output, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
			result["virtual_geometry"] = report;
			result["render_outputs"] = render_outputs;
			result["source_prototypes"] = int64_t(shared.prototypes.size());
			result["source_instances"] = shared.authored_instances;
			result["visible_point_instances"] = int64_t(shared.visible_point_instances);
			result["source_unique_triangles"] = importer.source_triangles;
			result["source_zero_area_triangles"] = int64_t(shared.zero_area_triangles);
			result["source_mesh_values"] = int64_t(shared.geometry_values.size());
			result["reused_source_mesh_values"] = int64_t(shared.reused_mesh_values);
			result["combined_prototype_parts"] = int64_t(shared.combined_prototype_parts);
			result["combined_mesh_values"] = int64_t(shared.combined_geometry_values.size());
			result["combined_unique_triangles"] = int64_t(shared.combined_unique_triangles);
			result["combined_zero_area_triangles"] = int64_t(shared.combined_zero_area_triangles);
			result["render_instances"] = int64_t(shared.render_instances);
			result["unique_geometry_triangles"] = int64_t(shared.unique_triangles[0] + shared.unique_triangles[1]);
			result["bark_triangles"] = int64_t(shared.instanced_triangles[0]);
			result["leaf_triangles"] = int64_t(shared.instanced_triangles[1]);
			result["source_representation"] = "shared_complete_prototypes";
			result["instance_asset_coordinates"] = shared.asset_coordinates;
			result["output"] = output;
			return e;
		} catch (const std::bad_alloc &) {
			return ERR_OUT_OF_MEMORY;
		}
	}
	e = importer.visit(importer.hierarchy);
	ERR_FAIL_COND_V(e != OK, e);
	ERR_FAIL_COND_V_MSG(authored_instances != importer.instance_count, ERR_INVALID_DATA, "USD authored branch count " + itos(authored_instances) + " differs from composed visible count " + itos(importer.instance_count) + ".");
	ERR_FAIL_COND_V_MSG(importer.surfaces[1].indices.empty(), ERR_INVALID_DATA, "Plant import has no leaf material faces; refusing a bare-tree replacement.");
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	const Dictionary overrides = job.get("material_overrides", Dictionary());
	for (int r = 0; r < 2; ++r) {
		auto &s = importer.surfaces[r];
		if (s.indices.empty()) {
			continue;
		}
		s.reduce(r == 0 ? importer.trunk_budget : importer.leaf_budget);
		const String name = r == 0 ? "bark" : "leaves";
		Ref<Material> material;
		if (overrides.has(name)) {
			const String path = overrides[name];
			HashSet<String> closure;
			e = CookerFiles::collect(path, closure);
			ERR_FAIL_COND_V(e != OK, e);
			material = ResourceLoader::load(path);
			ERR_FAIL_COND_V(material.is_null(), ERR_INVALID_DATA);
		} else {
			Ref<StandardMaterial3D> standard;
			standard.instantiate();
			standard->set_albedo(r == 0 ? Color(.24, .16, .09) : Color(.2, .38, .12));
			standard->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			standard->set_roughness(.8);
			material = standard;
		}
		material->set_name(name);
		Dictionary lods;
		for (int level = 1; level <= 4; ++level) {
			PackedInt32Array ix;
			// Keep the base vertex buffer: LOD indices refer to original vertices.
			std::vector<unsigned int> selected(s.indices.size());
			float error = 0;
			const size_t count = meshopt_simplifySloppy(selected.data(), s.indices.data(), s.indices.size(), reinterpret_cast<const float *>(s.points.data()), s.points.size(), sizeof(Vector3), nullptr, MAX(size_t(96), s.indices.size() >> level), 1.0f, &error);
			if (count >= 3 && count < s.indices.size()) {
				ix.resize(count);
				for (size_t i = 0; i < count; ++i) {
					ix.set(i, selected[i]);
				}
				const float distance = MAX(.01f, error * meshopt_simplifyScale(reinterpret_cast<const float *>(s.points.data()), s.points.size(), sizeof(Vector3)));
				lods[distance + level * .005f] = ix;
			}
		}
		const Array arrays = s.arrays();
		ERR_FAIL_COND_V(arrays.size() != Mesh::ARRAY_MAX, ERR_OUT_OF_MEMORY);
		const int surface_count = mesh->get_surface_count();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), lods);
		ERR_FAIL_COND_V(mesh->get_surface_count() != surface_count + 1, ERR_INVALID_DATA);
		mesh->surface_set_material(mesh->get_surface_count() - 1, material);
		mesh->surface_set_name(mesh->get_surface_count() - 1, name);
		result[name+"_triangles"]=int(s.indices.size()/3);
	}
	ERR_FAIL_COND_V(mesh->get_surface_count()==0,ERR_INVALID_DATA);
	e=DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));ERR_FAIL_COND_V(e!=OK,e);
	e = CookerAtomicSave::save_resource(mesh, output, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
	result["source_prototypes"]=int(importer.prototypes.size());result["source_instances"]=importer.instance_count;result["source_unique_triangles"]=importer.source_triangles;result["output"]=output;return e;
}
}
