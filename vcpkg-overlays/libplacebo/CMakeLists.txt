cmake_minimum_required(VERSION 3.21)
project(libplacebo_cpu LANGUAGES C)

if(NOT DEFINED LIBPLACEBO_SOURCE_DIR OR NOT IS_DIRECTORY "${LIBPLACEBO_SOURCE_DIR}")
    message(FATAL_ERROR "LIBPLACEBO_SOURCE_DIR must name the fetched libplacebo source tree")
endif()

set(PL_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PL_GENERATED_INCLUDE}/libplacebo")
set(majorver 7)
set(apiver 387)
set(buildver "v7.387.0")
set(extra_defs "#undef PL_HAVE_D3D11\n#undef PL_HAVE_DOVI\n#undef PL_HAVE_GLSLANG\n#undef PL_HAVE_LCMS\n#undef PL_HAVE_LIBDOVI\n#undef PL_HAVE_OPENGL\n#undef PL_HAVE_SHADERC\n#undef PL_HAVE_VK_PROC_ADDR\n#undef PL_HAVE_VULKAN\n#undef PL_HAVE_XXHASH")
configure_file("${LIBPLACEBO_SOURCE_DIR}/src/include/libplacebo/config.h.in"
               "${PL_GENERATED_INCLUDE}/libplacebo/config.h" @ONLY)
configure_file("${LIBPLACEBO_SOURCE_DIR}/src/version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/config_internal.h"
"#pragma once\n#define BUILD_API_VER 387\n#define BUILD_FIX_VER 0\n#undef PL_DEBUG_ABORT\n#undef PL_HAVE_DBGHELP\n#undef PL_HAVE_EXECINFO\n#undef PL_HAVE_UNWIND\n#define BUILD_VERSION \"v7.387.0\"\n")

add_library(placebo STATIC
    "${LIBPLACEBO_SOURCE_DIR}/src/common.c"
    "${LIBPLACEBO_SOURCE_DIR}/src/colorspace.c"
    "${LIBPLACEBO_SOURCE_DIR}/src/tone_mapping.c"
    "${LIBPLACEBO_SOURCE_DIR}/src/gamut_mapping.c")
target_compile_features(placebo PRIVATE c_std_11)
target_compile_definitions(placebo PUBLIC PL_STATIC)
if(MSVC)
    target_compile_definitions(placebo PRIVATE _USE_MATH_DEFINES)
endif()
target_include_directories(placebo PRIVATE
    "${LIBPLACEBO_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PL_GENERATED_INCLUDE}"
    PUBLIC
    "$<BUILD_INTERFACE:${LIBPLACEBO_SOURCE_DIR}/src/include>"
    "$<BUILD_INTERFACE:${PL_GENERATED_INCLUDE}>")
if(NOT WIN32)
    target_compile_definitions(placebo PRIVATE PL_HAVE_PTHREAD)
    find_package(Threads REQUIRED)
    target_link_libraries(placebo PUBLIC Threads::Threads m)
endif()

install(TARGETS placebo ARCHIVE DESTINATION lib)
install(FILES
    "${PL_GENERATED_INCLUDE}/libplacebo/config.h"
    "${LIBPLACEBO_SOURCE_DIR}/src/include/libplacebo/common.h"
    "${LIBPLACEBO_SOURCE_DIR}/src/include/libplacebo/colorspace.h"
    "${LIBPLACEBO_SOURCE_DIR}/src/include/libplacebo/tone_mapping.h"
    "${LIBPLACEBO_SOURCE_DIR}/src/include/libplacebo/gamut_mapping.h"
    DESTINATION include/libplacebo)
