#include "usd_import.h"
#include "asset_files.h"
#include "scene/resources/mesh.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"
#include "scene/3d/mesh_instance_3d.h"
#include "core/io/resource_saver.h"
#include "thirdparty/meshoptimizer/meshoptimizer.h"
#include "tinyusdz.hh"
#include "usdGeom.hh"
#include "usdSkel.hh"
#include "composition.hh"
#include "tydra/scene-access.hh"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <tuple>

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
		ERR_FAIL_COND_V_MSG(!u::LayerToStage(std::move(flattened),&stage,&warn,&err),ERR_INVALID_DATA,String::utf8(err.c_str()));
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
	std::vector<Vector3> points,normals;
	std::vector<Vector2> uv;
	std::vector<unsigned int> indices;
	void append(const Surface &s,const Transform3D &t) {
		const unsigned int start=points.size(); const Basis normal=t.basis.inverse().transposed();
		for(size_t i=0;i<s.points.size();++i) { points.push_back(t.xform(s.points[i]));normals.push_back(normal.xform(s.normals[i]).normalized());uv.push_back(s.uv[i]); }
		const bool reverse=t.basis.determinant()<0;
		for(size_t i=0;i<s.indices.size();i+=3) {indices.push_back(start+s.indices[i]);indices.push_back(start+s.indices[i+(reverse?2:1)]);indices.push_back(start+s.indices[i+(reverse?1:2)]);}
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
	void reduce(size_t triangles) {
		if(indices.size()<=triangles*3 || points.empty())return;
		static_assert(sizeof(Vector3)==sizeof(float)*3,"USD mesh optimization needs float engine vectors");
		std::vector<unsigned int> selected(indices.size());float error=0;
		const size_t count=meshopt_simplifySloppy(selected.data(),indices.data(),indices.size(),reinterpret_cast<const float *>(points.data()),points.size(),sizeof(Vector3),nullptr,triangles*3,1.0f,&error);
		if(count>=3) {selected.resize(count);indices=std::move(selected);compact();}
	}
	Array arrays() const {
		PackedVector3Array p,n;PackedVector2Array tex;PackedInt32Array ix;
		p.resize(points.size());n.resize(normals.size());tex.resize(uv.size());ix.resize(indices.size());
		for(size_t i=0;i<points.size();++i) {p.set(i,points[i]);n.set(i,normals[i]);tex.set(i,uv[i]);}
		for(size_t i=0;i<indices.size();++i)ix.set(i,indices[i]);
		Array out;out.resize(Mesh::ARRAY_MAX);out[Mesh::ARRAY_VERTEX]=p;out[Mesh::ARRAY_NORMAL]=n;out[Mesh::ARRAY_TEX_UV]=tex;out[Mesh::ARRAY_INDEX]=ix;return out;
	}
};
struct Import {
	u::Stage stage; XformNode hierarchy;
	std::map<std::string,const XformNode *> nodes;
	std::map<std::string,std::array<Surface,2>> prototypes;
	Surface surfaces[2];
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
	Error branch(const XformNode &root,const Transform3D &relative,std::array<Surface,2> &out) {
		if(root.prim && root.prim->as<u::GeomMesh>()) {std::array<Surface,2> raw;Error e=mesh(root,raw);ERR_FAIL_COND_V(e!=OK,e);for(int r=0;r<2;++r) {raw[r].reduce(prototype_budget);out[r].append(raw[r],relative*transform(root.get_world_matrix()));}}
		for(const auto &child:root.children) {Error e=branch(child,relative,out);ERR_FAIL_COND_V(e!=OK,e);}return OK;
	}
	Error visit(const XformNode &node) {
		if(!node.prim) {for(const auto &child:node.children) {Error e=visit(child);if(e!=OK)return e;}return OK;}
		const std::string path=node.absolute_path.full_path_name();
		if(path.find("/Prototypes/")!=std::string::npos || path.size()>=11 && path.compare(path.size()-11,11,"/Prototypes")==0)return OK;
		if(const auto *inst=node.prim->as<u::PointInstancer>()) {
			ERR_FAIL_COND_V(!inst->prototypes,ERR_INVALID_DATA);const auto paths=targets(*inst->prototypes);
			std::vector<int> ids;std::vector<u::value::point3f> p;std::vector<u::value::quath> q;std::vector<u::value::float3> sc;
			ERR_FAIL_COND_V(!sample(inst->protoIndices,ids) || !sample(inst->positions,p) || ids.size()!=p.size() || ids.size()>20000,ERR_INVALID_DATA);
			sample(inst->orientations,q);sample(inst->scales,sc);ERR_FAIL_COND_V(!q.empty() && q.size()!=p.size() || !sc.empty() && sc.size()!=p.size(),ERR_INVALID_DATA);
			for(const auto &prototype:paths) {
				const auto key=prototype.full_path_name();ERR_FAIL_COND_V(!nodes.count(key),ERR_INVALID_DATA);
				if(!prototypes.count(key)) {std::array<Surface,2> raw;Error e=branch(*nodes.at(key),transform(nodes.at(key)->get_world_matrix()).affine_inverse(),raw);ERR_FAIL_COND_V(e!=OK,e);prototypes[key]=std::move(raw);}
			}
			for(size_t i=0;i<ids.size();++i) {
				ERR_FAIL_COND_V(ids[i]<0 || ids[i]>=int(paths.size()),ERR_INVALID_DATA);Basis basis;
				if(!q.empty())basis=Basis(Quaternion(u::value::half_to_float(q[i][0]),u::value::half_to_float(q[i][1]),u::value::half_to_float(q[i][2]),u::value::half_to_float(q[i][3])).normalized());
				if(!sc.empty())basis.scale(vector(sc[i]));
				const Transform3D placement=transform(node.get_world_matrix())*Transform3D(basis,vector(p[i]));
				const auto &source=prototypes.at(paths[ids[i]].full_path_name());for(int r=0;r<2;++r)surfaces[r].append(source[r],placement);++instance_count;
			}
			return OK;
		}
		if(node.prim->as<u::GeomMesh>()) {std::array<Surface,2> raw;Error e=mesh(node,raw);ERR_FAIL_COND_V(e!=OK,e);for(int r=0;r<2;++r){raw[r].reduce(r==0?trunk_budget:leaf_budget);surfaces[r].append(raw[r],transform(node.get_world_matrix()));}}
		for(const auto &child:node.children) {Error e=visit(child);if(e!=OK)return e;}return OK;
	}
};
int budget(const Dictionary &options,const String &key,int fallback) {
	const Variant v=options.get(key,fallback);
	if((v.get_type()!=Variant::INT && v.get_type()!=Variant::FLOAT) || !Math::is_finite(double(v)) || double(v)!=Math::floor(double(v)) || double(v)<32 || double(v)>500000)return -1;
	return int(v);
}
}
Error dependencies(const String &source,HashSet<String> &files) {HashSet<String> pending;return gather(source,files,pending);}
Error import_scene(const Dictionary &job,Dictionary &result) {
	const String source=job.get("source",""),output=job.get("output",""),type=job.get("type","PackedScene");
	ERR_FAIL_COND_V(type!="ArrayMesh" && type!="PackedScene",ERR_UNAVAILABLE);
	HashSet<String> files;Error e=dependencies(source,files);ERR_FAIL_COND_V(e!=OK,e);
	Import importer;e=load(source,importer.stage,true);ERR_FAIL_COND_V(e!=OK,e);
	ERR_FAIL_COND_V(importer.stage.metas().upAxis.get_value()!=u::Axis::Y || importer.stage.metas().metersPerUnit.get_value()!=1.0,ERR_UNAVAILABLE);
	const Dictionary options=job.get("options",Dictionary());
	for(const Variant &key:options.keys())ERR_FAIL_COND_V(key!="prototype_triangles" && key!="bark_triangles" && key!="leaf_triangles",ERR_INVALID_PARAMETER);
	importer.prototype_budget=budget(options,"prototype_triangles",1200);importer.trunk_budget=budget(options,"bark_triangles",6000);importer.leaf_budget=budget(options,"leaf_triangles",40000);
	ERR_FAIL_COND_V(importer.prototype_budget<0 || importer.trunk_budget<0 || importer.leaf_budget<0,ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(!u::tydra::BuildXformNodeFromStage(importer.stage,&importer.hierarchy,u::value::TimeCode::Default(),u::value::TimeSampleInterpolationType::Linear),ERR_INVALID_DATA,"USD transform hierarchy could not be evaluated.");
	// Tydra copies its temporary hierarchy; repair ancestor pointers only after
	// the final vector storage is stable. Never retain pointers into temporaries.
	std::function<void(XformNode &,XformNode *)> index=[&](XformNode &n,XformNode *parent){n.parent=parent;importer.nodes[n.absolute_path.full_path_name()]=&n;for(auto &c:n.children)index(c,&n);};index(importer.hierarchy,nullptr);
	e=importer.visit(importer.hierarchy);ERR_FAIL_COND_V(e!=OK,e);
	ERR_FAIL_COND_V_MSG(importer.surfaces[1].indices.empty(),ERR_INVALID_DATA,"Plant import has no leaf material faces; refusing a bare-tree replacement.");
	Ref<ArrayMesh> mesh;mesh.instantiate();const Dictionary overrides=job.get("material_overrides",Dictionary());
	for(int r=0;r<2;++r) {
		auto &s=importer.surfaces[r];if(s.indices.empty())continue;s.reduce(r==0?importer.trunk_budget:importer.leaf_budget);
		const String name=r==0?"bark":"leaves";Ref<Material> material;
		if(overrides.has(name)) {const String path=overrides[name];HashSet<String> closure;e=CookerFiles::collect(path,closure);ERR_FAIL_COND_V(e!=OK,e);material=ResourceLoader::load(path);ERR_FAIL_COND_V(material.is_null(),ERR_INVALID_DATA);}
		else {Ref<StandardMaterial3D> standard;standard.instantiate();standard->set_albedo(r==0?Color(.24,.16,.09):Color(.2,.38,.12));standard->set_cull_mode(BaseMaterial3D::CULL_DISABLED);standard->set_roughness(.8);material=standard;}
		material->set_name(name);
		Dictionary lods;for(int level=1;level<=4;++level) {PackedInt32Array ix;
			// Keep the base vertex buffer: LOD indices refer to original vertices.
			std::vector<unsigned int> selected(s.indices.size());float error=0;const size_t count=meshopt_simplifySloppy(selected.data(),s.indices.data(),s.indices.size(),reinterpret_cast<const float *>(s.points.data()),s.points.size(),sizeof(Vector3),nullptr,MAX(size_t(96),s.indices.size()>>level),1.0f,&error);
			if(count>=3 && count<s.indices.size()){ix.resize(count);for(size_t i=0;i<count;++i)ix.set(i,selected[i]);const float distance=MAX(.01f,error*meshopt_simplifyScale(reinterpret_cast<const float *>(s.points.data()),s.points.size(),sizeof(Vector3)));lods[distance+level*.005f]=ix;}
		}
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES,s.arrays(),Array(),lods);mesh->surface_set_material(mesh->get_surface_count()-1,material);mesh->surface_set_name(mesh->get_surface_count()-1,name);
		result[name+"_triangles"]=int(s.indices.size()/3);
	}
	ERR_FAIL_COND_V(mesh->get_surface_count()==0,ERR_INVALID_DATA);
	e=DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));ERR_FAIL_COND_V(e!=OK,e);
	if(type=="ArrayMesh")e=ResourceSaver::save(mesh,output,ResourceSaver::FLAG_COMPRESS|ResourceSaver::FLAG_RELATIVE_PATHS);
	else {MeshInstance3D *root=memnew(MeshInstance3D);root->set_name("SourceTree");root->set_mesh(mesh);Ref<PackedScene> scene;scene.instantiate();e=scene->pack(root);memdelete(root);if(e==OK)e=ResourceSaver::save(scene,output,ResourceSaver::FLAG_COMPRESS|ResourceSaver::FLAG_RELATIVE_PATHS);}
	result["source_prototypes"]=int(importer.prototypes.size());result["source_instances"]=importer.instance_count;result["source_unique_triangles"]=importer.source_triangles;result["output"]=output;return e;
}
}
