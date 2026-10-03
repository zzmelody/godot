#pragma once
#include "asset_files.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/version.h"

namespace CookerLightmap {
inline Error bake(const Dictionary &job, Dictionary &report) {
	ERR_FAIL_COND_V(job.get("source",Variant()).get_type()!=Variant::STRING || job.get("output",Variant()).get_type()!=Variant::STRING, ERR_INVALID_PARAMETER);
	const String source=job["source"], output=job["output"];
	Error error=CookerFiles::check_path(source);
	ERR_FAIL_COND_V(error!=OK || !CookerFiles::is_generated(source) || source.get_extension()!="scn",ERR_INVALID_PARAMETER);
	error=CookerFiles::check_path(output,true);
	ERR_FAIL_COND_V(error!=OK || output.get_extension()!="scn" || source==output || FileAccess::exists(output),ERR_INVALID_PARAMETER);
	String executable=OS::get_singleton()->get_executable_path().get_base_dir().path_join(
#ifdef WINDOWS_ENABLED
		"godot.windows.editor.x86_64.exe"
#elif defined(MACOS_ENABLED)
		"godot.macos.editor.arm64"
#else
		"godot.linuxbsd.editor.x86_64"
#endif
	);
	ERR_FAIL_COND_V_MSG(!FileAccess::exists(executable),ERR_FILE_NOT_FOUND,"GPU lightmap baking requires the sibling pinned Veya editor executable.");
	const String executable_hash=FileAccess::get_sha256(executable);
	List<String> args;args.push_back("--version"); String version; int status=-1;
	error=OS::get_singleton()->execute(executable,args,&version,&status,true);
	ERR_FAIL_COND_V_MSG(error!=OK || status!=0 || !version.contains(String(GODOT_VERSION_HASH).left(9)),ERR_INVALID_DATA,"Lightmap baker and Cooker must use the same Veya engine revision.");
	error=DirAccess::make_dir_recursive_absolute(output.get_base_dir());ERR_FAIL_COND_V(error!=OK,error);
	args.clear();args.push_back("--path");args.push_back(ProjectSettings::get_singleton()->globalize_path("res://"));
	args.push_back("--rendering-method");args.push_back("forward_plus");
	args.push_back("--rendering-driver");
#ifdef MACOS_ENABLED
	args.push_back("metal");
#else
	args.push_back("vulkan");
#endif
	args.push_back("--minimized");args.push_back("--audio-driver");args.push_back("Dummy");
	args.push_back("--resolution");args.push_back("64x64");args.push_back("--");
	args.push_back("--veya-bake-lightmap="+source);args.push_back("--veya-bake-output="+output);
	String log;status=-1;error=OS::get_singleton()->execute(executable,args,&log,&status,true);
	report["baker_sha256"]=executable_hash;
	for(const String &line:log.split("\n"))if(line.begins_with("VEYA_LIGHTMAP_RESULT=")) {
		const Variant result=JSON::parse_string(line.trim_prefix("VEYA_LIGHTMAP_RESULT="));
		if(result.get_type()==Variant::DICTIONARY)report["bake"]=result;
	}
	ERR_FAIL_COND_V_MSG(error!=OK || status!=0 || !report.has("bake") || !FileAccess::exists(output),ERR_CANT_CREATE,"Veya GPU lightmap bake failed: "+log.right(4096));
	const Dictionary result=report["bake"];
	ERR_FAIL_COND_V(String(result.get("engine_revision",""))!=String(GODOT_VERSION_HASH) || int(result.get("error",-1))!=0 || FileAccess::get_sha256(executable)!=executable_hash,ERR_INVALID_DATA);
	report["output"]=output;report["sha256"]=FileAccess::get_sha256(output);return OK;
}
}
