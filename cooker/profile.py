# /*<<----- VEYA_COOKER: reproducible asset-only tools build; see cooker/README.md. */
target = "editor"
veya_cooker = True
build_profile = "cooker/cooker.gdbuild"
arch = "x86_64"
production = True
optimize = "size"
debug_symbols = False
extra_suffix = "veya_cooker"
windows_subsystem = "console"

# Import/generation jobs do not render or create a desktop interface.
vulkan = False
d3d12 = False
angle = False
opengl3 = False
metal = False
disable_xr = True
disable_physics_2d = True
disable_navigation_2d = True
disable_advanced_gui = True
accesskit = False
sdl = False
xaudio2 = False
winrt = False
engine_update_check = False
deprecated = False
tests = False

# Keep upstream module defaults, then disable services and editor-facing modules.
module_gdscript_enabled = False
module_mono_enabled = False
module_enet_enabled = False
module_multiplayer_enabled = False
module_webrtc_enabled = False
module_websocket_enabled = False
module_upnp_enabled = False
module_jsonrpc_enabled = False
module_openxr_enabled = False
module_mobile_vr_enabled = False
module_webxr_enabled = False
module_camera_enabled = False
module_godot_physics_2d_enabled = False
module_navigation_2d_enabled = False
module_jolt_physics_enabled = False
module_interactive_music_enabled = False
module_mp3_enabled = False
module_ogg_enabled = False
module_vorbis_enabled = False
module_theora_enabled = False
module_text_server_adv_enabled = False
module_text_server_fb_enabled = False
module_freetype_enabled = False
module_svg_enabled = False
module_msdfgen_enabled = False
module_lightmapper_rd_enabled = False
module_glslang_enabled = False
module_betsy_enabled = False
module_visual_shader_enabled = False
module_csg_enabled = False
module_gridmap_enabled = False
module_raycast_enabled = False
module_objectdb_profiler_enabled = False

# Asset dependencies are explicit even when they currently default to enabled.
module_gltf_enabled = True
module_fbx_enabled = True
module_meshoptimizer_enabled = True
module_vhacd_enabled = True
module_godot_physics_3d_enabled = True
module_navigation_3d_enabled = True
module_noise_enabled = True
module_xatlas_unwrap_enabled = True
module_basis_universal_enabled = True
module_astcenc_enabled = True
module_bcdec_enabled = True
module_cvtt_enabled = True
module_etcpak_enabled = True
module_dds_enabled = True
module_ktx_enabled = True
module_jpg_enabled = True
module_webp_enabled = True
module_tinyexr_enabled = True
module_hdr_enabled = True
module_mbedtls_enabled = True
# /*>>----- VEYA_COOKER */
