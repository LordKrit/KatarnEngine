#============================================================================
# KatarnEngine - DirectX 12 renderer for Movie Duels SP (MovieDuels-rdsp-katarn_x86_64.dll)
#
# Front end (BSP, shaders, models, Ghoul2) comes from rd-vanilla. The OpenGL back end is gone:
#   katarn/kt_device.*   D3D12 device, swap chain, descriptor heaps, upload ring, PSO cache, HLSL compiler
#   katarn/kt_gl.*       fixed-function translation for legacy content (menus, HUD, BSP maps, effects)
#   katarn/kt_render.*   the frame: GBuffer, shadows, deferred lighting, sky, post-processing
#   katarn/kt_world.*    HD worlds (.kworld) cooked from Blender
#============================================================================

if(NOT InMovieDuels)
	message(FATAL_ERROR "Use the top-level cmake script!")
endif(NOT InMovieDuels)

set(KT ${SPDir}/rd-katarn)
set(KatarnIncludeDirectories ${SharedDir} ${SPDir} "${KT}" "${KT}/katarn" "${SPDir}/rd-common" "${GSLIncludeDirectory}"
	${JPEG_INCLUDE_DIR} ${PNG_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIR} ${MINIZIP_INCLUDE_DIRS} ${OpenJKLibDir})
set(KatarnLibraries ${JPEG_LIBRARIES} ${PNG_LIBRARIES} ${ZLIB_LIBRARIES} ${MINIZIP_LIBRARIES})
set(KatarnDefines ${SharedDefines} "RENDERER" "_JK2EXE" "KATARN" "WIDL_EXPLICIT_AGGREGATE_RETURNS")

set(KatarnFiles
	"${SPDir}/ghoul2/G2.h"
	"${SPDir}/ghoul2/ghoul2_gore.h"
	"${KT}/G2_API.cpp" "${KT}/G2_bolts.cpp" "${KT}/G2_bones.cpp" "${KT}/G2_misc.cpp" "${KT}/G2_surfaces.cpp"
	"${SPDir}/qcommon/matcomp.cpp"
	"${SPDir}/qcommon/q_shared.cpp"
	${SharedCommonFiles}
	${SharedCommonSafeFiles}
	"${KT}/tr_backend.cpp" "${KT}/tr_bsp.cpp" "${KT}/tr_cmds.cpp" "${KT}/tr_curve.cpp" "${KT}/tr_draw.cpp"
	"${KT}/tr_ghoul2.cpp" "${KT}/tr_meshcloth.cpp" "${KT}/tr_image.cpp" "${KT}/tr_init.cpp" "${KT}/tr_light.cpp"
	"${KT}/tr_main.cpp" "${KT}/tr_marks.cpp" "${KT}/tr_mesh.cpp" "${KT}/tr_model.cpp" "${KT}/tr_quicksprite.cpp"
	"${KT}/tr_scene.cpp" "${KT}/tr_shade.cpp" "${KT}/tr_shade_calc.cpp" "${KT}/tr_shader.cpp" "${KT}/tr_shadows.cpp"
	"${KT}/tr_skin.cpp" "${KT}/tr_sky.cpp" "${KT}/tr_stl.cpp" "${KT}/tr_subs.cpp" "${KT}/tr_surface.cpp"
	"${KT}/tr_surfacesprites.cpp" "${KT}/tr_world.cpp" "${KT}/tr_WorldEffects.cpp"
	"${SPDir}/rd-common/tr_font.cpp" "${SPDir}/rd-common/tr_image_load.cpp" "${SPDir}/rd-common/tr_image_jpg.cpp"
	"${SPDir}/rd-common/tr_video_encoder.cpp" "${SPDir}/rd-common/tr_image_tga.cpp" "${SPDir}/rd-common/tr_image_png.cpp"
	"${SPDir}/rd-common/tr_noise.cpp"
	"${KT}/katarn/kt_device.cpp"
	"${KT}/katarn/kt_gl.cpp"
	"${KT}/katarn/kt_render.cpp"
	"${KT}/katarn/kt_crash.cpp"
	"${KT}/katarn/kt_world.cpp"
	)

add_library(${SPKatarn} SHARED ${KatarnFiles})
if(NOT MSVC)
	set_target_properties(${SPKatarn} PROPERTIES PREFIX "")
endif()
install(TARGETS ${SPKatarn} RUNTIME DESTINATION ${JKAInstallDir} COMPONENT ${JKASPClientComponent})
# Ghoul2 surfaces up to 16000 vertices (HD ports; rend2 has no such limit), CPU-skinned through tess
set_target_properties(${SPKatarn} PROPERTIES COMPILE_DEFINITIONS "${KatarnDefines};SHADER_MAX_VERTEXES=16000")
set_property(TARGET ${SPKatarn} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})
set_target_properties(${SPKatarn} PROPERTIES INCLUDE_DIRECTORIES "${KatarnIncludeDirectories}")
set_target_properties(${SPKatarn} PROPERTIES PROJECT_LABEL "SP KatarnEngine Renderer")
target_link_libraries(${SPKatarn} ${KatarnLibraries} dxgi dxguid)
