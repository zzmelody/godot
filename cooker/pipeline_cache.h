/*<<----- VEYA_COOKER: verified immutable job receipts; metadata, never an execution language. */
#pragma once
#include "asset_files.h"
#include "core/io/json.h"

namespace CookerCache {
struct Receipt { String path, output, fingerprint; Dictionary dependencies; };
inline Error dependencies(const Variant &value,Dictionary &result,int depth=0,const String &field="") {
	ERR_FAIL_COND_V(depth>16,ERR_INVALID_DATA);
	if(field=="output" || field=="output_dir") return OK;
	if(value.get_type()==Variant::STRING) {
		const String path=value;
		if(!path.begins_with("res://")) return OK;
		if(CookerFiles::is_generated(path) && DirAccess::dir_exists_absolute(path)) {
			Array roots;HashSet<String> metadata;int directories=0;
			Error error=CookerFiles::collect_directory(path,roots,metadata,directories);ERR_FAIL_COND_V(error!=OK,error);
			for(const String &entry:metadata)result[entry]=FileAccess::get_sha256(entry);
			for(const Variant &entry:roots) {error=dependencies(entry,result,depth+1);ERR_FAIL_COND_V(error!=OK,error);}
			ERR_FAIL_COND_V(result.size()>4096,ERR_OUT_OF_MEMORY);
			return OK;
		}
		if(!FileAccess::exists(path))return OK;
		Error error=CookerFiles::check_path(path);ERR_FAIL_COND_V(error!=OK,error);
		HashSet<String> closure;
		if(path.ends_with(".res") || path.ends_with(".scn")) {error=CookerFiles::collect(path,closure);ERR_FAIL_COND_V(error!=OK,error);}
		else closure.insert(path);
		for(const String &source:closure)result[source]=FileAccess::get_sha256(source);
		ERR_FAIL_COND_V(result.size()>4096,ERR_OUT_OF_MEMORY);
	} else if(value.get_type()==Variant::ARRAY) {
		const Array values=value;ERR_FAIL_COND_V(values.size()>16384,ERR_OUT_OF_MEMORY);
		for(const Variant &entry:values) {const Error error=dependencies(entry,result,depth+1);ERR_FAIL_COND_V(error!=OK,error);}
	} else if(value.get_type()==Variant::DICTIONARY) {
		const Dictionary values=value;ERR_FAIL_COND_V(values.size()>16384,ERR_OUT_OF_MEMORY);
		for(const Variant *key=values.next();key;key=values.next(key)) {const Error error=dependencies(values[*key],result,depth+1,*key);ERR_FAIL_COND_V(error!=OK,error);}
	}
	return OK;
}
inline Error read(const String &path,Dictionary &out) {
	Ref<FileAccess> file=FileAccess::open(path,FileAccess::READ);
	ERR_FAIL_COND_V(file.is_null() || file->get_length()>1024*1024,ERR_INVALID_DATA);
	const Variant value=JSON::parse_string(file->get_as_text());ERR_FAIL_COND_V(value.get_type()!=Variant::DICTIONARY,ERR_INVALID_DATA);
	out=value;return OK;
}
inline Error validate_manifest_files(const String &path) {
	Dictionary manifest;Error error=read(path,manifest);ERR_FAIL_COND_V(error!=OK,error);
	ERR_FAIL_COND_V(manifest.get("files",Variant()).get_type()!=Variant::ARRAY,ERR_INVALID_DATA);
	const Array files=manifest["files"];ERR_FAIL_COND_V(files.is_empty() || files.size()>256,ERR_INVALID_DATA);
	const String base=path.get_base_dir();HashSet<String> seen;
	for(const Variant &value:files) {
		ERR_FAIL_COND_V(value.get_type()!=Variant::DICTIONARY,ERR_INVALID_DATA);
		const Dictionary record=value;
		ERR_FAIL_COND_V(record.get("path",Variant()).get_type()!=Variant::STRING || record.get("sha256",Variant()).get_type()!=Variant::STRING,ERR_INVALID_DATA);
		const String relative=record["path"],digest=record["sha256"];
		ERR_FAIL_COND_V(relative.is_empty() || relative.is_absolute_path() || relative.contains(":") || relative.contains("\\") || seen.has(relative),ERR_INVALID_DATA);
		const String resource=base.path_join(relative),extension=relative.get_extension().to_lower();
		ERR_FAIL_COND_V(extension!="scn" && extension!="res",ERR_INVALID_DATA);
		error=CookerFiles::check_path(resource);ERR_FAIL_COND_V(error!=OK || !resource.begins_with(base+"/"),ERR_INVALID_DATA);
		const Ref<FileAccess> file=FileAccess::open(resource,FileAccess::READ);
		ERR_FAIL_COND_V_MSG(file.is_null() || int64_t(record.get("bytes",-1))!=int64_t(file->get_length()) || digest.length()!=64 || digest!=FileAccess::get_sha256(resource),ERR_INVALID_DATA,"Cached manifest resource changed; choose a new immutable revision: "+resource);
		seen.insert(relative);
	}
	return OK;
}
inline Error prepare(const Dictionary &job,const String &output,Receipt &receipt,bool &hit,bool rebuild_unverified=false) {
	hit=false;receipt={};receipt.output=output;receipt.path=output+".cook.json";
	Error error=CookerFiles::check_path(output,true,String(job.get("operation",""))=="pack");ERR_FAIL_COND_V(error!=OK,error);
	error=dependencies(job,receipt.dependencies);ERR_FAIL_COND_V(error!=OK,error);
	Dictionary request;request["schema_version"]=1;request["job"]=job;request["dependencies"]=receipt.dependencies;
	receipt.fingerprint=JSON::stringify(request,"",true).sha256_text();
	if(!FileAccess::exists(output))return OK;
	if(!FileAccess::exists(receipt.path) && rebuild_unverified) {
		// Regenerate legacy bytes from this declared step; do not reuse them.
		error=DirAccess::remove_absolute(output);ERR_FAIL_COND_V(error!=OK,error);
		return OK;
	}
	ERR_FAIL_COND_V_MSG(!FileAccess::exists(receipt.path),ERR_INVALID_DATA,"Existing output has no verified provenance; choose a new immutable revision: "+output);
	Dictionary prior;error=read(receipt.path,prior);ERR_FAIL_COND_V(error!=OK,error);
	// Generated assets have stable current paths. A changed Luau request or
	// source invalidates its cache, but a modified output is still rejected.
	// Only remove the exact verified output/receipt owned by this step.
	ERR_FAIL_COND_V_MSG(String(prior.get("output_sha256",""))!=FileAccess::get_sha256(output),ERR_INVALID_DATA,"Cook cache output integrity failed: "+output);
	if(String(prior.get("request_sha256",""))!=receipt.fingerprint) {
		error=DirAccess::remove_absolute(output);ERR_FAIL_COND_V(error!=OK,error);
		error=DirAccess::remove_absolute(receipt.path);ERR_FAIL_COND_V(error!=OK,error);
		return OK;
	}
	// Manifest file names are relative to their generated directory, so they
	// do not appear in the generic res:// dependency walk. Preserve existing
	// receipts and validate their recorded bytes/hashes before accepting a hit.
	if(String(job.get("operation",""))=="asset-manifest") {error=validate_manifest_files(output);ERR_FAIL_COND_V(error!=OK,error);}
	hit=true;return OK;
}
inline Error commit(const Receipt &receipt) {
	const String digest=FileAccess::get_sha256(receipt.output);ERR_FAIL_COND_V(digest.length()!=64,ERR_FILE_CORRUPT);
	if(FileAccess::exists(receipt.path)) {
		Dictionary prior;const Error error=read(receipt.path,prior);ERR_FAIL_COND_V(error!=OK,error);
		ERR_FAIL_COND_V(String(prior.get("request_sha256",""))!=receipt.fingerprint || String(prior.get("output_sha256",""))!=digest,ERR_INVALID_DATA);
		return OK;
	}
	Dictionary data;data["schema_version"]=1;data["output"]=receipt.output;data["request_sha256"]=receipt.fingerprint;data["output_sha256"]=digest;data["dependencies"]=receipt.dependencies;
	const String temporary=receipt.path+".partial";ERR_FAIL_COND_V(FileAccess::exists(temporary),ERR_ALREADY_EXISTS);
	Ref<FileAccess> file=FileAccess::open(temporary,FileAccess::WRITE);ERR_FAIL_COND_V(file.is_null(),ERR_FILE_CANT_WRITE);
	file->store_string(JSON::stringify(data)+"\n");file.unref();
	return DirAccess::rename_absolute(temporary,receipt.path);
}
}
/*>>----- VEYA_COOKER */
