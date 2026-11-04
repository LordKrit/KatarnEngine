# rd-sojourn: Movie Duels SP renderer ported from Warzone's rd-warzone (OpenWarzone/Warzone, GPLv2) - large open worlds.
if(NOT InMovieDuels)
	message(FATAL_ERROR "Use the top-level cmake script!")
endif(NOT InMovieDuels)

set(SJDir "${SPDir}/rd-sojourn")
set(SPSojournIncludeDirectories "${SharedDir}" "${SPDir}" "${SJDir}" "${GSLIncludeDirectory}")

file(GLOB SPSojournFiles "${SJDir}/*.cpp" "${SJDir}/*.h")
list(FILTER SPSojournFiles EXCLUDE REGEX "tr_awesomium|tr_openvr|tr_nuklear_gui")
file(GLOB SPSojournCompat "${SJDir}/compat/qcommon/*.cpp" "${SJDir}/compat/game/*.cpp" "${SJDir}/compat/client/*.cpp" "${SJDir}/compat/SharedMemory/*.cpp" "${SJDir}/meshOptimization/*.cpp")
list(APPEND SPSojournFiles ${SPSojournCompat})

file(GLOB SPSojournGLSL1 "${SJDir}/glsl/*.glsl")
file(GLOB SPSojournGLSL2 "${SJDir}/glsl2/*.glsl")
file(GLOB SPSojournGLSL3 "${SJDir}/glsl3/*.glsl")

set(SPSojournFiles ${SPSojournFiles}
	"${SPDir}/rd-common/tr_font.cpp"
	"${SPDir}/rd-common/tr_image_load.cpp"
	"${SPDir}/rd-common/tr_image_jpg.cpp"
	"${SPDir}/rd-common/tr_image_tga.cpp"
	"${SPDir}/rd-common/tr_image_png.cpp"
	"${SPDir}/rd-common/tr_noise.cpp"
	"${SPDir}/qcommon/matcomp.cpp"
	"${SPDir}/qcommon/q_shared.cpp"
	${SharedCommonFiles} ${SharedCommonSafeFiles}
	${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp
	${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders2.cpp
	${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders3.cpp)

list(APPEND SPSojournIncludeDirectories ${JPEG_INCLUDE_DIR} ${PNG_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIR} ${MINIZIP_INCLUDE_DIRS} ${OpenJKLibDir})
list(APPEND SPSojournLibraries ${JPEG_LIBRARIES} ${PNG_LIBRARIES} ${ZLIB_LIBRARIES} ${MINIZIP_LIBRARIES})
find_package(OpenGL REQUIRED)
list(APPEND SPSojournIncludeDirectories ${OPENGL_INCLUDE_DIR})
list(APPEND SPSojournLibraries ${OPENGL_LIBRARIES})

add_library(${SPSojourn} SHARED ${SPSojournFiles})
if(NOT MSVC)
	set_target_properties(${SPSojourn} PROPERTIES PREFIX "")
endif()
set_target_properties(${SPSojourn} PROPERTIES COMPILE_DEFINITIONS "${SharedDefines};RENDERER;REND2_SP;RD_SOJOURN;NOMINMAX" CXX_STANDARD 14)
set_property(TARGET ${SPSojourn} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})
set_target_properties(${SPSojourn} PROPERTIES INCLUDE_DIRECTORIES "${SPSojournIncludeDirectories}")
target_link_libraries(${SPSojourn} ${SPSojournLibraries})
target_include_directories(${SPSojourn} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
install(TARGETS ${SPSojourn} RUNTIME DESTINATION ${JKAInstallDir} COMPONENT ${JKASPClientComponent})

# Warzone's shader packer (plain file-to-string); built natively when cross compiling
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp ${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders2.cpp ${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders3.cpp
	COMMAND sojourn_compact_glsl ${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp ${SPSojournGLSL1}
	COMMAND sojourn_compact_glsl ${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders2.cpp ${SPSojournGLSL2}
	COMMAND sojourn_compact_glsl ${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders3.cpp ${SPSojournGLSL3}
	DEPENDS ${SPSojournGLSL1} ${SPSojournGLSL2} ${SPSojournGLSL3})
