cmake_minimum_required(VERSION 3.30)

# Read version from the single source-of-truth file.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AWJ_VERSION_STRING)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$"
    _ver_match "${AWJ_VERSION_STRING}")
if(NOT _ver_match)
    message(FATAL_ERROR "VERSION file does not contain a valid MAJOR.MINOR.PATCH string: \"${AWJ_VERSION_STRING}\"")
endif()

project(AWJimage
    VERSION ${AWJ_VERSION_STRING}
    DESCRIPTION "AWJimage C++23 native batch image converter (AVIF/WebP/JXL/JPGLI) with Slint"
    LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AVIF_STATIC_SLINT "Link Slint statically so AWJ does not need slint_cpp.dll" ON)
option(AVIF_STATIC_MSVC_RUNTIME "Use /MT or /MTd for smaller redistribution bundles when all dependencies allow it" ON)
option(AVIF_ENABLE_RELEASE_IPO "Enable interprocedural optimization for Release builds when the compiler supports it" OFF)
option(AWJ_ENABLE_STRICT_MSVC_CHECKS "Enable MSVC /W4 and /sdl for project targets" ON)
option(AWJ_ENABLE_X64_V3 "Enable x64-v3/AVX2 code generation for project targets in Release builds" OFF)
option(AWJ_ENABLE_ZENRAVIF "Build experimental zenrav1e into AWJ" ON)
option(AWJ_ENABLE_SVTAV1HDR "Build svt-av1-hdr into AWJ" ON)
option(AWJ_ENABLE_JPEGLI "Build Google Jpegli JPGLI codec support into AWJ" ON)
option(AWJ_PATCH_LIBAVIF_PARALLEL_GRID "Patch pinned libavif to encode simple still color grids in parallel" ON)
set(AWJ_UPDATE_PUBLIC_KEY_HEX "ebdb1a49b8686f34a5791f7c0f2a0215e4cdc03ad99281e5bf8606cb62fcf1b1" CACHE STRING
    "Ed25519 update-manifest public key as 64 lowercase hexadecimal characters")
set(AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX "652b506c2fbd71d75ae48cca3d0196cd5914605dd811c1a2b786c6bbd259f05f" CACHE STRING
    "Offline recovery root A public key for the signed updater keyring")
set(AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX "65a49e06e12dfc48bf26dbbcc5380d5fbd61198c6e7cd65af4f860dfe5b6f65a" CACHE STRING
    "Offline recovery root B public key for the signed updater keyring")
set(AWJ_SVTAV1HDR_GIT_TAG "8b4b9f5624cb70c2363a7cebb553110c1447dd4c" CACHE STRING "svt-av1-hdr source commit")
set(AWJ_LIBAVIF_GIT_REPOSITORY "https://github.com/AOMediaCodec/libavif.git" CACHE STRING "libavif repository used for the static SVT-enabled build")
set(AWJ_LIBAVIF_GIT_TAG "c5240fc79fe5c2407e10afd35f5505ef6333ea49" CACHE STRING "libavif git tag or commit")
set(AWJ_JPEGLI_GIT_REPOSITORY "https://github.com/google/jpegli.git" CACHE STRING "google/jpegli repository used for JPGLI support")
set(AWJ_JPEGLI_GIT_TAG "031a0077f5799a6041004267fc12b956c1f52a20" CACHE STRING "google/jpegli git tag or commit")
set(AWJ_SLINT_GIT_REPOSITORY "https://github.com/slint-ui/slint.git" CACHE STRING "Slint repository")
set(AWJ_SLINT_GIT_TAG "cf62c975c311e7036d599ed8ed0b7e6a8386a934" CACHE STRING "Slint git tag or commit")

set(AVIF_RELEASE_IPO_SUPPORTED OFF)
if(AVIF_ENABLE_RELEASE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AVIF_RELEASE_IPO_SUPPORTED
        OUTPUT AVIF_RELEASE_IPO_ERROR
        LANGUAGES CXX)
    if(NOT AVIF_RELEASE_IPO_SUPPORTED)
        message(WARNING "Release IPO/LTO is not available: ${AVIF_RELEASE_IPO_ERROR}")
    endif()
endif()

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper}
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/${config}")
endforeach()

if(MSVC)
    if(AVIF_STATIC_MSVC_RUNTIME)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

set(VCPKG_ROOT "$ENV{VCPKG_ROOT}" CACHE PATH "vcpkg root directory")
if(WIN32 AND NOT VCPKG_ROOT AND EXISTS "D:/Scoop/apps/vcpkg/current")
    set(VCPKG_ROOT "D:/Scoop/apps/vcpkg/current" CACHE PATH "vcpkg root directory" FORCE)
endif()
if(WIN32)
    if(AVIF_STATIC_MSVC_RUNTIME)
        set(AVIF_DEFAULT_VCPKG_TRIPLET "x64-windows-static")
    else()
        set(AVIF_DEFAULT_VCPKG_TRIPLET "x64-windows")
    endif()
else()
    set(AVIF_DEFAULT_VCPKG_TRIPLET "x64-linux")
endif()
if(DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TRIPLET)
    set(VCPKG_TRIPLET "${VCPKG_TARGET_TRIPLET}" CACHE STRING "vcpkg target triplet")
else()
    set(VCPKG_TRIPLET "${AVIF_DEFAULT_VCPKG_TRIPLET}" CACHE STRING "vcpkg target triplet")
endif()
set(VCPKG_TARGET_TRIPLET "${VCPKG_TRIPLET}" CACHE STRING "vcpkg target triplet" FORCE)
set(AWJ_VCPKG_LIBRARY_CONFIG "" CACHE STRING
    "vcpkg library configuration used by upstream single-config Find modules")
set_property(CACHE AWJ_VCPKG_LIBRARY_CONFIG PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
if(NOT AWJ_VCPKG_LIBRARY_CONFIG)
    get_filename_component(AWJ_BINARY_CONFIG_NAME "${CMAKE_BINARY_DIR}" NAME)
    if(AWJ_BINARY_CONFIG_NAME MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$")
        set(AWJ_VCPKG_LIBRARY_CONFIG "${AWJ_BINARY_CONFIG_NAME}" CACHE STRING
            "vcpkg library configuration used by upstream single-config Find modules"
            FORCE)
    else()
        set(AWJ_VCPKG_LIBRARY_CONFIG "Release" CACHE STRING
            "vcpkg library configuration used by upstream single-config Find modules"
            FORCE)
    endif()
endif()
if(VCPKG_ROOT AND NOT CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg[.]cmake")
    list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/${VCPKG_TRIPLET}")
endif()

foreach(package_dir_var IN ITEMS scn_DIR FastFloat_DIR)
    if(DEFINED ${package_dir_var})
        string(FIND "${${package_dir_var}}" "/${VCPKG_TRIPLET}/" package_triplet_position)
        if(package_triplet_position EQUAL -1)
            message(STATUS "Clearing stale cached ${package_dir_var}: ${${package_dir_var}}")
            unset(${package_dir_var} CACHE)
        endif()
    endif()
endforeach()

message(STATUS "AWJimage vcpkg triplet: ${VCPKG_TRIPLET}")
message(STATUS "AWJimage vcpkg library config: ${AWJ_VCPKG_LIBRARY_CONFIG}")
message(STATUS "AWJimage MSVC runtime: ${CMAKE_MSVC_RUNTIME_LIBRARY}")

find_package(scn CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(GIF REQUIRED)
find_package(TIFF REQUIRED)
find_package(libraw CONFIG REQUIRED)
find_package(AOM CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# CMake's FindLibArchive scans debug before release and otherwise pins every
# Release executable to archive.lib built with /MTd. Select the vcpkg release
# archive up front, then give the imported target explicit config locations.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_awj_libarchive_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    if(EXISTS "${_awj_libarchive_root}/lib/archive.lib")
        set(LibArchive_LIBRARY "${_awj_libarchive_root}/lib/archive.lib"
            CACHE FILEPATH "The libarchive library selected by AWJ" FORCE)
    endif()
endif()
find_package(LibArchive REQUIRED)
if(WIN32 AND TARGET LibArchive::LibArchive AND DEFINED _awj_libarchive_root)
    set_property(TARGET LibArchive::LibArchive PROPERTY IMPORTED_CONFIGURATIONS
                 "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL")
    set_target_properties(LibArchive::LibArchive PROPERTIES
        IMPORTED_LOCATION_DEBUG "${_awj_libarchive_root}/debug/lib/archive.lib"
        IMPORTED_LOCATION_RELEASE "${_awj_libarchive_root}/lib/archive.lib"
        IMPORTED_LOCATION_RELWITHDEBINFO "${_awj_libarchive_root}/lib/archive.lib"
        IMPORTED_LOCATION_MINSIZEREL "${_awj_libarchive_root}/lib/archive.lib")
endif()
find_package(unofficial-sodium CONFIG QUIET)
if(NOT TARGET unofficial-sodium::sodium)
    if(WIN32)
        message(FATAL_ERROR "libsodium package did not provide unofficial-sodium::sodium")
    endif()
    find_path(AWJ_SYSTEM_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
    find_library(AWJ_SYSTEM_SODIUM_LIBRARY NAMES sodium REQUIRED)
    add_library(awj_system_sodium UNKNOWN IMPORTED)
    set_target_properties(awj_system_sodium PROPERTIES
        IMPORTED_LOCATION "${AWJ_SYSTEM_SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${AWJ_SYSTEM_SODIUM_INCLUDE_DIR}")
    add_library(unofficial-sodium::sodium ALIAS awj_system_sodium)
endif()
if(WIN32)
    find_package(libyuv CONFIG REQUIRED)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    find_package(CURL REQUIRED)
endif()

set(AWJ_SVTAV1HDR_AVAILABLE OFF)
set(AWJ_SVTAV1HDR_STATIC_DEFINE 0)
set(AWJ_JPEGLI_AVAILABLE OFF)

if(VCPKG_INSTALLED_DIR)
    set(AVIF_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TRIPLET}")
else()
    set(AVIF_VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TRIPLET}")
endif()

find_path(AWJ_LIBPLACEBO_INCLUDE_DIR
    NAMES libplacebo/tone_mapping.h
    PATHS "${AVIF_VCPKG_INSTALLED_DIR}/include"
    NO_DEFAULT_PATH
    REQUIRED)
function(avif_find_vcpkg_library out_var library_name)
    find_library(${out_var}_RELEASE
        NAMES ${library_name}
        PATHS "${AVIF_VCPKG_INSTALLED_DIR}/lib"
        NO_DEFAULT_PATH
        REQUIRED)
    find_library(${out_var}_DEBUG
        NAMES ${library_name}
        PATHS "${AVIF_VCPKG_INSTALLED_DIR}/debug/lib"
        NO_DEFAULT_PATH)
    set(${out_var}_RELEASE "${${out_var}_RELEASE}" PARENT_SCOPE)
    set(${out_var}_DEBUG "${${out_var}_DEBUG}" PARENT_SCOPE)
    if(${out_var}_DEBUG)
        set(${out_var}
            "$<$<CONFIG:Debug>:${${out_var}_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${${out_var}_RELEASE}>"
            PARENT_SCOPE)
    else()
        set(${out_var} "${${out_var}_RELEASE}" PARENT_SCOPE)
    endif()
endfunction()

avif_find_vcpkg_library(AWJ_LIBPLACEBO_LIBRARY placebo)
add_library(awj_libplacebo INTERFACE IMPORTED)
target_include_directories(awj_libplacebo INTERFACE
    "${AWJ_LIBPLACEBO_INCLUDE_DIR}")
target_compile_definitions(awj_libplacebo INTERFACE PL_STATIC)
target_link_libraries(awj_libplacebo INTERFACE "${AWJ_LIBPLACEBO_LIBRARY}")
if(WIN32)
    target_link_libraries(awj_libplacebo INTERFACE shlwapi)
else()
    target_link_libraries(awj_libplacebo INTERFACE Threads::Threads m)
endif()

function(awj_force_single_config_vcpkg_library cache_var release_var debug_var)
    set(release_library "${${release_var}}")
    set(debug_library "${${debug_var}}")
    set(selected_library "${release_library}")
    if(AWJ_VCPKG_LIBRARY_CONFIG STREQUAL "Debug" AND debug_library)
        set(selected_library "${debug_library}")
    endif()
    if(selected_library AND NOT selected_library MATCHES "-NOTFOUND$")
        set(${cache_var} "${selected_library}" CACHE FILEPATH
            "AWJ-selected ${cache_var} for ${AWJ_VCPKG_LIBRARY_CONFIG}"
            FORCE)
    endif()
endfunction()

find_path(JXL_INCLUDE_DIR
    NAMES jxl/encode.h
    PATHS "${AVIF_VCPKG_INSTALLED_DIR}/include"
    REQUIRED)
avif_find_vcpkg_library(JXL_LIBRARY jxl)
avif_find_vcpkg_library(JXL_THREADS_LIBRARY jxl_threads)
avif_find_vcpkg_library(JXL_CMS_LIBRARY jxl_cms)
avif_find_vcpkg_library(JXL_HIGHWAY_LIBRARY hwy)
avif_find_vcpkg_library(JXL_BROTLI_ENC_LIBRARY brotlienc)
avif_find_vcpkg_library(JXL_BROTLI_DEC_LIBRARY brotlidec)
avif_find_vcpkg_library(JXL_BROTLI_COMMON_LIBRARY brotlicommon)
avif_find_vcpkg_library(JXL_LCMS_LIBRARY lcms2)
awj_force_single_config_vcpkg_library(LCMS2_LIBRARY
    JXL_LCMS_LIBRARY_RELEASE
    JXL_LCMS_LIBRARY_DEBUG)
avif_find_vcpkg_library(AWJ_AOM_LIBRARY aom)
awj_force_single_config_vcpkg_library(AOM_LIBRARY
    AWJ_AOM_LIBRARY_RELEASE
    AWJ_AOM_LIBRARY_DEBUG)
avif_find_vcpkg_library(AWJ_DAV1D_LIBRARY dav1d)
awj_force_single_config_vcpkg_library(DAV1D_LIBRARY
    AWJ_DAV1D_LIBRARY_RELEASE
    AWJ_DAV1D_LIBRARY_DEBUG)
if(WIN32)
avif_find_vcpkg_library(AWJ_LIBYUV_LIBRARY yuv)
awj_force_single_config_vcpkg_library(LIBYUV_LIBRARY
    AWJ_LIBYUV_LIBRARY_RELEASE
    AWJ_LIBYUV_LIBRARY_DEBUG)
endif()
set(HWY_INCLUDE_DIR "${JXL_INCLUDE_DIR}" CACHE PATH
    "AWJ-selected Highway include directory"
    FORCE)
set(HWY_LIBRARY "${JXL_HIGHWAY_LIBRARY_RELEASE}" CACHE FILEPATH
    "AWJ-selected Highway release library"
    FORCE)
if(JXL_HIGHWAY_LIBRARY_DEBUG)
    set(HWY_DEBUG_LIBRARY "${JXL_HIGHWAY_LIBRARY_DEBUG}" CACHE FILEPATH
        "AWJ-selected Highway debug library"
        FORCE)
endif()
add_library(JXL::jxl INTERFACE IMPORTED)
target_include_directories(JXL::jxl INTERFACE "${JXL_INCLUDE_DIR}")
target_compile_definitions(JXL::jxl INTERFACE
    JXL_STATIC_DEFINE
    JXL_THREADS_STATIC_DEFINE
    JXL_CMS_STATIC_DEFINE)
target_link_libraries(JXL::jxl INTERFACE
    "${JXL_LIBRARY}"
    "${JXL_THREADS_LIBRARY}"
    "${JXL_CMS_LIBRARY}"
    "${JXL_HIGHWAY_LIBRARY}"
    "${JXL_BROTLI_ENC_LIBRARY}"
    "${JXL_BROTLI_DEC_LIBRARY}"
    "${JXL_BROTLI_COMMON_LIBRARY}"
    "${JXL_LCMS_LIBRARY}")

function(avif_enable_common_options target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(WIN32)
        target_compile_definitions(${target} PUBLIC
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PUBLIC
            /utf-8
            /permissive-
            /EHsc
            $<$<BOOL:${AWJ_ENABLE_STRICT_MSVC_CHECKS}>:/W4>
            $<$<BOOL:${AWJ_ENABLE_STRICT_MSVC_CHECKS}>:/sdl>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/Ot>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/GF>
            $<$<AND:$<CONFIG:Release>,$<BOOL:${AWJ_ENABLE_X64_V3}>>:/arch:AVX2>)
        get_target_property(target_type ${target} TYPE)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:/OPT:REF>
                $<$<CONFIG:Release>:/OPT:ICF>
                $<$<CONFIG:Release>:/INCREMENTAL:NO>)
        endif()
    else()
        target_compile_options(${target} PUBLIC
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
            $<$<AND:$<CONFIG:Release>,$<BOOL:${AWJ_ENABLE_X64_V3}>>:-march=x86-64-v3>)
        get_target_property(target_type ${target} TYPE)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:-static-libstdc++>
                $<$<CONFIG:Release>:-static-libgcc>
                $<$<CONFIG:Release>:LINKER:--gc-sections>
                $<$<CONFIG:Release>:LINKER:--strip-all>)
        endif()
    endif()
    if(AVIF_RELEASE_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endfunction()

function(awj_set_runtime_directory target base_dir)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${base_dir}/${config}")
    endforeach()
endfunction()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cmake")
set(AWJ_WRITE_SHA256_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake/write_sha256.cmake")
file(WRITE "${AWJ_WRITE_SHA256_SCRIPT}" [=[
if(NOT DEFINED AWJ_SHA256_INPUT)
    message(FATAL_ERROR "AWJ_SHA256_INPUT is required.")
endif()
if(NOT DEFINED AWJ_SHA256_OUTPUT)
    message(FATAL_ERROR "AWJ_SHA256_OUTPUT is required.")
endif()
if(NOT DEFINED AWJ_SHA256_NAME)
    get_filename_component(AWJ_SHA256_NAME "${AWJ_SHA256_INPUT}" NAME)
endif()
file(SHA256 "${AWJ_SHA256_INPUT}" AWJ_SHA256_HASH)
file(WRITE "${AWJ_SHA256_OUTPUT}" "${AWJ_SHA256_HASH}  ${AWJ_SHA256_NAME}\n")
]=])

function(awj_add_sha256_post_build target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DAWJ_SHA256_INPUT=$<TARGET_FILE:${target}>"
                "-DAWJ_SHA256_OUTPUT=$<TARGET_FILE:${target}>.sha256"
                "-DAWJ_SHA256_NAME=$<TARGET_FILE_NAME:${target}>"
                -P "${AWJ_WRITE_SHA256_SCRIPT}"
        VERBATIM)
endfunction()

function(awj_suppress_msvc_warnings target)
    if(MSVC AND TARGET ${target})
        target_compile_options(${target} PRIVATE ${ARGN})
    endif()
endfunction()

function(avif_copy_slint_runtime target)
    if(TARGET slint_cpp-shared)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:slint_cpp-shared>"
                    "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    elseif(AVIF_STATIC_SLINT)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f
                    "$<TARGET_FILE_DIR:${target}>/slint_cpp.dll"
            VERBATIM)
    endif()
    # 这里曾经在 POST_BUILD 删除 slint-compiler.exe / slint_compiler.pdb。那是错的：
    # corrosion 把 slint-compiler 作为 byproduct 拷进运行时输出目录，而
    # Slint::slint-compiler 的 TARGET_FILE 就解析到那里。AWJ 与 awj_ui_smoke_tests
    # 共用同一个输出目录，并行构建时 AWJ 的删除会把另一个目标正要调用的编译器抹掉，
    # 表现为 awj_studio_ui_smoke.h 的自定义生成随机失败（MSB8066）；串行构建因为会
    # 重新拷贝而恰好看不出问题。发布打包本身用 release.ps1 的 $ReleaseFiles 白名单
    # 拷到独立目录，编译器不可能被打进包里，所以这个清理没有任何收益。
endfunction()

set(AWJ_VISUAL_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/core/shaders")
set(AWJ_VISUAL_SHADER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/visual_shaders")
set(AWJ_VISUAL_SHADER_HEADER "${AWJ_VISUAL_SHADER_GENERATED_DIR}/awj_visual_metric_shaders.hpp")
file(MAKE_DIRECTORY "${AWJ_VISUAL_SHADER_GENERATED_DIR}")

if(WIN32)
    set(AWJ_SHADER_COMPILER_DIRS)
    if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
        list(APPEND AWJ_SHADER_COMPILER_DIRS "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64")
    endif()
    set(AWJ_WINDOWS_KIT_ROOT_CANDIDATES)
    if(DEFINED ENV{ProgramFiles})
        list(APPEND AWJ_WINDOWS_KIT_ROOT_CANDIDATES "$ENV{ProgramFiles}/Windows Kits/10")
    endif()
    list(APPEND AWJ_WINDOWS_KIT_ROOT_CANDIDATES
        "C:/Program Files (x86)/Windows Kits/10"
        "C:/Program Files/Windows Kits/10")
    foreach(AWJ_WINDOWS_KIT_ROOT IN LISTS AWJ_WINDOWS_KIT_ROOT_CANDIDATES)
        file(GLOB AWJ_WINDOWS_KIT_SHADER_DIRS "${AWJ_WINDOWS_KIT_ROOT}/bin/*/x64")
        list(SORT AWJ_WINDOWS_KIT_SHADER_DIRS)
        list(REVERSE AWJ_WINDOWS_KIT_SHADER_DIRS)
        list(APPEND AWJ_SHADER_COMPILER_DIRS ${AWJ_WINDOWS_KIT_SHADER_DIRS})
    endforeach()
    find_program(AWJ_FXC_EXECUTABLE NAMES fxc.exe fxc PATHS ${AWJ_SHADER_COMPILER_DIRS} NO_DEFAULT_PATH)
    if(NOT AWJ_FXC_EXECUTABLE)
        find_program(AWJ_FXC_EXECUTABLE NAMES fxc.exe fxc)
    endif()
    if(NOT AWJ_FXC_EXECUTABLE)
        message(FATAL_ERROR "找不到 fxc.exe，无法预编译 visual_quality Direct3D 11 shader。请安装 Windows SDK；最终用户运行时不需要该工具。")
    endif()
    message(STATUS "AWJimage visual metric shader compiler: ${AWJ_FXC_EXECUTABLE}")

    function(awj_compile_visual_shader out_var shader_name hlsl_name)
        set(output_path "${AWJ_VISUAL_SHADER_GENERATED_DIR}/${shader_name}.cso")
        add_custom_command(
            OUTPUT "${output_path}"
            COMMAND "${AWJ_FXC_EXECUTABLE}" /nologo /T cs_5_0 /E main /Ges /O3
                    /Fo "${output_path}"
                    "${AWJ_VISUAL_SHADER_DIR}/${hlsl_name}"
            DEPENDS "${AWJ_VISUAL_SHADER_DIR}/${hlsl_name}"
            COMMENT "Precompiling visual metric shader ${hlsl_name}"
            VERBATIM)
        set(${out_var} "${output_path}" PARENT_SCOPE)
    endfunction()
else()
    find_package(Vulkan REQUIRED)
    find_program(AWJ_DXC_EXECUTABLE NAMES dxc
        PATHS "${AVIF_VCPKG_INSTALLED_DIR}/tools/directx-dxc" NO_DEFAULT_PATH)
    if(NOT AWJ_DXC_EXECUTABLE)
        find_program(AWJ_DXC_EXECUTABLE NAMES dxc REQUIRED)
    endif()
    message(STATUS "AWJimage visual metric shader compiler: ${AWJ_DXC_EXECUTABLE}")

    function(awj_compile_visual_shader out_var shader_name hlsl_name)
        set(output_path "${AWJ_VISUAL_SHADER_GENERATED_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${output_path}"
            COMMAND "${AWJ_DXC_EXECUTABLE}" -spirv -T cs_6_0 -E main -O3
                    -fspv-target-env=vulkan1.1 -fvk-use-dx-layout -fvk-t-shift 8 all -fvk-u-shift 16 all
                    -Fo "${output_path}"
                    "${AWJ_VISUAL_SHADER_DIR}/${hlsl_name}"
            DEPENDS "${AWJ_VISUAL_SHADER_DIR}/${hlsl_name}"
            COMMENT "Precompiling visual metric shader ${hlsl_name} to SPIR-V"
            VERBATIM)
        set(${out_var} "${output_path}" PARENT_SCOPE)
    endfunction()
endif()

awj_compile_visual_shader(AWJ_LUMA_CSO luma visual_luma.hlsl)
awj_compile_visual_shader(AWJ_GMSD_CSO gmsd visual_gmsd.hlsl)
awj_compile_visual_shader(AWJ_DOWNSAMPLE_CSO downsample visual_downsample.hlsl)
awj_compile_visual_shader(AWJ_MS_SSIM_CSO ms_ssim visual_ms_ssim.hlsl)

add_custom_command(
    OUTPUT "${AWJ_VISUAL_SHADER_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            "-DAWJ_SHADER_HEADER=${AWJ_VISUAL_SHADER_HEADER}"
            "-DAWJ_LUMA_CSO=${AWJ_LUMA_CSO}"
            "-DAWJ_GMSD_CSO=${AWJ_GMSD_CSO}"
            "-DAWJ_DOWNSAMPLE_CSO=${AWJ_DOWNSAMPLE_CSO}"
            "-DAWJ_MS_SSIM_CSO=${AWJ_MS_SSIM_CSO}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_visual_shader_header.cmake"
    DEPENDS "${AWJ_LUMA_CSO}" "${AWJ_GMSD_CSO}" "${AWJ_DOWNSAMPLE_CSO}" "${AWJ_MS_SSIM_CSO}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_visual_shader_header.cmake"
    COMMENT "Embedding visual metric shader bytecode"
    VERBATIM)
add_custom_target(awj_visual_metric_shaders DEPENDS "${AWJ_VISUAL_SHADER_HEADER}")

include(FetchContent)

if(AWJ_ENABLE_SVTAV1HDR)
    set(AWJ_PARENT_BUILD_TESTING ${BUILD_TESTING})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_APPS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF)
    set(BUILD_DEC OFF CACHE BOOL "" FORCE)
    set(COMPILE_C_ONLY ON CACHE BOOL "" FORCE)
    set(ENABLE_AVX512 OFF CACHE BOOL "" FORCE)
    set(SVT_AV1_BUILD_APPS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SvtAv1Hdr
        URL "https://codeload.github.com/juliobbv-p/svt-av1-hdr/tar.gz/${AWJ_SVTAV1HDR_GIT_TAG}"
        URL_HASH "SHA256=35454847f6635c793c996f5a1bda824b827b14de7fa04dd58d9c985aa795801f")
    FetchContent_MakeAvailable(SvtAv1Hdr)
    # Upstream main assigns the Cr point twice and leaves the matching Cb point
    # uninitialized. Patch the fetched source after population so this also works
    # when release.ps1 reuses it through FETCHCONTENT_SOURCE_DIR_SVTAV1HDR.
    set(AWJ_SVTAV1HDR_NOISE_SOURCE
        "${svtav1hdr_SOURCE_DIR}/Source/Lib/Codec/noise_generation.c")
    file(READ "${AWJ_SVTAV1HDR_NOISE_SOURCE}" AWJ_SVTAV1HDR_NOISE_TEXT)
    set(AWJ_SVTAV1HDR_BROKEN_SCALING
        "film_grain->scaling_points_cr[1][1] = film_grain->scaling_points_cr[1][1] =")
    set(AWJ_SVTAV1HDR_FIXED_SCALING
        "film_grain->scaling_points_cr[1][1] = film_grain->scaling_points_cb[1][1] =")
    string(FIND "${AWJ_SVTAV1HDR_NOISE_TEXT}" "${AWJ_SVTAV1HDR_BROKEN_SCALING}"
           AWJ_SVTAV1HDR_BROKEN_SCALING_OFFSET)
    if(NOT AWJ_SVTAV1HDR_BROKEN_SCALING_OFFSET EQUAL -1)
        string(REPLACE "${AWJ_SVTAV1HDR_BROKEN_SCALING}" "${AWJ_SVTAV1HDR_FIXED_SCALING}"
               AWJ_SVTAV1HDR_NOISE_TEXT "${AWJ_SVTAV1HDR_NOISE_TEXT}")
        file(WRITE "${AWJ_SVTAV1HDR_NOISE_SOURCE}" "${AWJ_SVTAV1HDR_NOISE_TEXT}")
    else()
        string(FIND "${AWJ_SVTAV1HDR_NOISE_TEXT}" "${AWJ_SVTAV1HDR_FIXED_SCALING}"
               AWJ_SVTAV1HDR_FIXED_SCALING_OFFSET)
        if(AWJ_SVTAV1HDR_FIXED_SCALING_OFFSET EQUAL -1)
            message(FATAL_ERROR "svt-av1-hdr noise scaling source no longer matches the audited fix.")
        endif()
    endif()
    file(WRITE "${svtav1hdr_SOURCE_DIR}/.awj-source-commit" "${AWJ_SVTAV1HDR_GIT_TAG}\n")
    awj_suppress_msvc_warnings(CODEC /wd4244)
    if(TARGET SvtAv1Enc)
        set(AWJ_SVTAV1HDR_AVAILABLE ON)
        set(AWJ_SVTAV1HDR_STATIC_DEFINE 1)
        set(AWJ_SVTAV1HDR_INCLUDE_COMPAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/svtav1hdr-include/svt-av1")
        file(MAKE_DIRECTORY "${AWJ_SVTAV1HDR_INCLUDE_COMPAT_DIR}")
        file(GLOB AWJ_SVTAV1HDR_API_HEADERS "${svtav1hdr_SOURCE_DIR}/Source/API/*.h")
        file(COPY ${AWJ_SVTAV1HDR_API_HEADERS} DESTINATION "${AWJ_SVTAV1HDR_INCLUDE_COMPAT_DIR}")
        target_include_directories(SvtAv1Enc INTERFACE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/svtav1hdr-include>")
    else()
        message(FATAL_ERROR "svt-av1-hdr FetchContent did not provide SvtAv1Enc.")
    endif()
    set(BUILD_TESTING ${AWJ_PARENT_BUILD_TESTING})
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AVIF_CODEC_AOM SYSTEM CACHE STRING "" FORCE)
set(AVIF_CODEC_AOM_DECODE ON CACHE BOOL "" FORCE)
set(AVIF_CODEC_AOM_ENCODE ON CACHE BOOL "" FORCE)
set(AVIF_CODEC_DAV1D SYSTEM CACHE STRING "" FORCE)
if(AWJ_SVTAV1HDR_AVAILABLE)
    set(AVIF_CODEC_SVT SYSTEM CACHE STRING "" FORCE)
else()
    set(AVIF_CODEC_SVT OFF CACHE STRING "" FORCE)
endif()
if(WIN32)
    set(AVIF_LIBYUV SYSTEM CACHE STRING "" FORCE)
else()
    set(AVIF_LIBYUV OFF CACHE STRING "" FORCE)
endif()
set(AVIF_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(AVIF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AVIF_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(AWJ_LIBAVIF_PATCH_COMMAND)
if(AWJ_PATCH_LIBAVIF_PARALLEL_GRID)
    set(AWJ_LIBAVIF_PATCH_COMMAND
        PATCH_COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libavif_parallel_grid.cmake")
endif()
FetchContent_Declare(libavif
    GIT_REPOSITORY ${AWJ_LIBAVIF_GIT_REPOSITORY}
    GIT_TAG ${AWJ_LIBAVIF_GIT_TAG}
    UPDATE_DISCONNECTED TRUE
    ${AWJ_LIBAVIF_PATCH_COMMAND})
FetchContent_MakeAvailable(libavif)

if(AWJ_ENABLE_JPEGLI)
    set(AWJ_PARENT_BUILD_TESTING ${BUILD_TESTING})
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    foreach(AWJ_JPEGLI_PREFIX IN ITEMS JPEGXL JPEGLI)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_JPEGLI_LIBJPEG OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_INSTALL_JPEGLI_LIBJPEG OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_FUZZERS OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_JNI OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_ENABLE_WASM_THREADS OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_FORCE_SYSTEM_HWY ON CACHE BOOL "" FORCE)
        set(${AWJ_JPEGLI_PREFIX}_FORCE_SYSTEM_LCMS2 ON CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(Jpegli
        GIT_REPOSITORY ${AWJ_JPEGLI_GIT_REPOSITORY}
        GIT_TAG ${AWJ_JPEGLI_GIT_TAG}
        GIT_SUBMODULES third_party/libjpeg-turbo
        UPDATE_DISCONNECTED TRUE
        PATCH_COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_jpegli_allocator.cmake")
    FetchContent_MakeAvailable(Jpegli)
    if(TARGET jpegli-static)
        set(AWJ_JPEGLI_AVAILABLE ON)
        if(MSVC)
            target_compile_options(jpegli-static PRIVATE /utf-8 /wd4267 /wd4244 /wd4100)
        endif()
    else()
        message(FATAL_ERROR "google/jpegli FetchContent did not provide jpegli-static.")
    endif()
    set(BUILD_TESTING ${AWJ_PARENT_BUILD_TESTING})
else()
    message(STATUS "JPGLI/Jpegli codec support disabled by AWJ_ENABLE_JPEGLI=OFF.")
endif()

if(AVIF_STATIC_SLINT)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
else()
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
endif()
set(SLINT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SLINT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_INTERPRETER OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_FEMTOVG ON CACHE BOOL "" FORCE)
FetchContent_Declare(Slint
    GIT_REPOSITORY ${AWJ_SLINT_GIT_REPOSITORY}
    GIT_TAG ${AWJ_SLINT_GIT_TAG}
    UPDATE_DISCONNECTED TRUE
    SOURCE_SUBDIR api/cpp
    PATCH_COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_slint_static_import.cmake")
FetchContent_MakeAvailable(Slint)
if(AVIF_STATIC_SLINT)
    target_compile_definitions(Slint INTERFACE SLINT_STATIC=1)
endif()
if(NOT WIN32)
    foreach(slint_target Slint slint_cpp slint_cpp-static)
        if(TARGET ${slint_target})
            foreach(link_property INTERFACE_LINK_LIBRARIES IMPORTED_LINK_INTERFACE_LIBRARIES IMPORTED_LINK_INTERFACE_LIBRARIES_RELEASE)
                get_target_property(awj_slint_link_libraries ${slint_target} ${link_property})
                if(awj_slint_link_libraries)
                    list(REMOVE_ITEM awj_slint_link_libraries gcc_s -lgcc_s)
                    set_target_properties(${slint_target} PROPERTIES ${link_property} "${awj_slint_link_libraries}")
                endif()
            endforeach()
        endif()
    endforeach()
    target_link_libraries(Slint INTERFACE fontconfig freetype)
endif()

function(avif_add_slint_sources target slint_file)
    set(options DEBUG_INFO)
    set(one_value_args OUTPUT_SUFFIX)
    cmake_parse_arguments(AWJ_SLINT "${options}" "${one_value_args}" "" ${ARGN})

    get_filename_component(slint_absolute "${slint_file}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(slint_base "${slint_absolute}" NAME_WE)

    set(generated_header "${CMAKE_CURRENT_BINARY_DIR}/${slint_base}${AWJ_SLINT_OUTPUT_SUFFIX}.h")
    set(generated_cpp "${CMAKE_CURRENT_BINARY_DIR}/slint_generated_${slint_base}${AWJ_SLINT_OUTPUT_SUFFIX}_1.cpp")
    set(slint_compiler_command "$<TARGET_FILE:Slint::slint-compiler>")
    if(AWJ_SLINT_DEBUG_INFO)
        set(slint_compiler_command
            "${CMAKE_COMMAND}" -E env SLINT_EMIT_DEBUG_INFO=1
            "$<TARGET_FILE:Slint::slint-compiler>")
    endif()

    # Slint's depfile currently produces noisy MSBuild warnings when the repo path
    # contains spaces. The UI has a single .slint file, so a direct dependency is
    # enough and keeps Windows paths intact.
    # 界面翻译走 Slint 的 bundled translations：目录结构是
    # <dir>/<语言>/LC_MESSAGES/<domain>.po，由 slint-compiler 直接解析 .po 编进二进制，
    # 不需要 msgfmt、不需要运行时 .mo、也不依赖 gettext（本项目的 Slint 没开 gettext）。
    # 中文是 msgid 本身，也是 bundled 默认语言；英文由 C++ 调
    # select_bundled_translation("en") 切换。该调用会写 translations_dirty 这个
    # 真实属性，所有 @tr() 绑定都会失效重算，可以不重启即时切换。
    # --no-default-translation-context：默认会拿组件名当 msgctxt，关掉之后 .po 里
    # 绝大多数条目不需要 msgctxt，只有真正歧义的（例如"关闭"既是 Off 又是 Close）
    # 才在源码里显式写 @tr("context" => "文本")。
    set(awj_translation_dir "${CMAKE_CURRENT_SOURCE_DIR}/ui/translations")
    set(awj_translation_po "${awj_translation_dir}/en/LC_MESSAGES/awj.po")
    add_custom_command(
        OUTPUT "${generated_header}" "${generated_cpp}"
        COMMAND ${slint_compiler_command} "${slint_absolute}"
                -f cpp
                --embed-resources embed-files
                --translation-domain awj
                --no-default-translation-context
                --bundle-translations "${awj_translation_dir}"
                -o "${generated_header}"
                "--cpp-file=${generated_cpp}"
        DEPENDS
            Slint::slint-compiler
            "${slint_absolute}"
            "${awj_translation_po}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/logo.png"
        COMMENT "Generating ${slint_base}.h"
        VERBATIM)

    target_sources(${target} PRIVATE "${generated_header}" "${generated_cpp}")
    if(MSVC)
        set_source_files_properties("${generated_cpp}" PROPERTIES
            COMPILE_OPTIONS "/wd4060;/wd4099;/wd4201;/wd4244;/wd4267;/wd4305;/wd4458;/wd4624")
    endif()
    target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>")
endfunction()

add_library(awj_base STATIC)
avif_enable_common_options(awj_base)
target_compile_definitions(awj_base PRIVATE
    AWJ_BUILD_VERSION="${PROJECT_VERSION}")
add_dependencies(awj_base awj_visual_metric_shaders)
target_link_libraries(awj_base PUBLIC
    scn::scn
    nlohmann_json::nlohmann_json
    awj_libplacebo)
if(WIN32)
    target_link_libraries(awj_base PUBLIC bcrypt d3d11)
    target_compile_definitions(awj_base PUBLIC AWJ_HAS_D3D11=1 AWJ_HAS_VULKAN=0)
else()
    target_link_libraries(awj_base PUBLIC Vulkan::Vulkan Threads::Threads)
    target_compile_definitions(awj_base PUBLIC AWJ_HAS_D3D11=0 AWJ_HAS_VULKAN=1)
endif()
target_sources(awj_base PRIVATE "${AWJ_VISUAL_SHADER_HEADER}")
target_include_directories(awj_base PUBLIC "$<BUILD_INTERFACE:${AWJ_VISUAL_SHADER_GENERATED_DIR}>")
target_sources(awj_base
    PUBLIC
        FILE_SET awj_base_modules TYPE CXX_MODULES FILES
            src/core/encoding_defaults.ixx
            src/core/large_image_plan.ixx
            src/app/config.ixx
            src/app/preset.ixx
            src/core/visual_quality.ixx
            src/core/visual_metrics.ixx
            src/core/image.ixx
            src/core/hdr_tonemap.ixx
            src/core/resource_planner.ixx
            src/core/codec.ixx
            src/core/process.ixx)

if(WIN32)
    target_sources(awj_base
        PUBLIC
            FILE_SET awj_base_modules TYPE CXX_MODULES FILES
                src/core/visual_metrics_gpu.ixx)
else()
    target_sources(awj_base
        PUBLIC
            FILE_SET awj_base_modules TYPE CXX_MODULES FILES
                src/core/visual_metrics_gpu_vulkan.ixx)
endif()

add_library(awj_raw_image_io STATIC)
avif_enable_common_options(awj_raw_image_io)
target_link_libraries(awj_raw_image_io PUBLIC awj_base)
target_sources(awj_raw_image_io
    PUBLIC
        FILE_SET awj_raw_image_io_modules TYPE CXX_MODULES FILES
            src/core/raw_image_io.ixx)

add_library(awj_native_visual_search STATIC)
avif_enable_common_options(awj_native_visual_search)
target_link_libraries(awj_native_visual_search PUBLIC awj_base)
target_sources(awj_native_visual_search
    PUBLIC
        FILE_SET awj_native_visual_search_modules TYPE CXX_MODULES FILES
            src/core/native_visual_search.ixx)

add_library(awj_native_codecs STATIC)
avif_enable_common_options(awj_native_codecs)
target_link_libraries(awj_native_codecs PUBLIC
    awj_base
    awj_raw_image_io
    WebP::webp
    WebP::webpdemux
    WebP::libwebpmux
    JXL::jxl
    PNG::PNG
    JPEG::JPEG
    GIF::GIF
    TIFF::TIFF
    libraw::raw_r)
if(WIN32)
    target_link_libraries(awj_native_codecs PUBLIC ole32 windowscodecs)
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_WINDOWS_CODECS=1)
else()
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_WINDOWS_CODECS=0)
endif()
target_sources(awj_native_codecs
    PUBLIC
        FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_registry.ixx
            src/codecs/decoder_common.ixx
            src/codecs/decoder_registry.ixx
            src/codecs/webp_codec.ixx
            src/codecs/jxl_codec.ixx
            src/codecs/png_codec.ixx
            src/codecs/bmp_codec.ixx
            src/codecs/jpeg_codec.ixx
            src/codecs/gif_codec.ixx
            src/codecs/tiff_codec.ixx
            src/codecs/libraw_codec.ixx)
if(WIN32)
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_AWJ_RAW_CODEC=1)
    target_sources(awj_native_codecs
        PUBLIC
            FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
                src/codecs/raw_codec.ixx
                src/codecs/jxr_codec.ixx
                src/codecs/wic_codec.ixx)
else()
    # ponytail: GCC16 ICEs importing awj.raw_image_io from raw_codec; LibRaw camera RAW still builds.
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_AWJ_RAW_CODEC=0)
endif()
target_compile_definitions(awj_native_codecs PUBLIC
    AWJ_HAS_JPEGLI=$<BOOL:${AWJ_JPEGLI_AVAILABLE}>)
if(AWJ_JPEGLI_AVAILABLE)
    target_link_libraries(awj_native_codecs PUBLIC jpegli-static)
    target_include_directories(awj_native_codecs PRIVATE "${jpegli_SOURCE_DIR}")
    target_sources(awj_native_codecs
        PUBLIC
            FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
                src/codecs/jpegli_codec.ixx)
endif()

set(ZENRAVIF_BRIDGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zenravif-bridge/Cargo.toml")
set(ZENRAVIF_BRIDGE_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zenravif-bridge/Cargo.lock")
set(ZENRAVIF_BRIDGE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zenravif-bridge/src/lib.rs")
set(AWJ_ZENRAVIF_AVAILABLE OFF)
if(AWJ_ENABLE_ZENRAVIF AND EXISTS "${ZENRAVIF_BRIDGE_MANIFEST}" AND EXISTS "${ZENRAVIF_BRIDGE_SOURCE}")
    set(AWJ_ZENRAVIF_AVAILABLE ON)
    set(ZENRAVIF_BRIDGE_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zenravif-bridge/target")
    if(WIN32)
        set(ZENRAVIF_BRIDGE_LIB "${ZENRAVIF_BRIDGE_TARGET_DIR}/release/zenravif_bridge.lib")
    else()
        set(ZENRAVIF_BRIDGE_LIB "${ZENRAVIF_BRIDGE_TARGET_DIR}/release/libzenravif_bridge.a")
    endif()
    add_custom_command(
        OUTPUT "${ZENRAVIF_BRIDGE_LIB}"
        COMMAND cargo build --manifest-path "${ZENRAVIF_BRIDGE_MANIFEST}" --release
        DEPENDS
            "${ZENRAVIF_BRIDGE_MANIFEST}"
            "${ZENRAVIF_BRIDGE_LOCK}"
            "${ZENRAVIF_BRIDGE_SOURCE}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building zenravif Rust static library"
        VERBATIM)
    add_custom_target(zenravif_bridge_static DEPENDS "${ZENRAVIF_BRIDGE_LIB}")
    add_library(zenravif_bridge STATIC IMPORTED GLOBAL)
    set_target_properties(zenravif_bridge PROPERTIES IMPORTED_LOCATION "${ZENRAVIF_BRIDGE_LIB}")
    if(WIN32)
        set_property(TARGET zenravif_bridge PROPERTY INTERFACE_LINK_LIBRARIES
            "ws2_32;userenv;ntdll;bcrypt;advapi32")
    else()
        set_property(TARGET zenravif_bridge PROPERTY INTERFACE_LINK_LIBRARIES "dl;pthread;m")
    endif()
    add_dependencies(zenravif_bridge zenravif_bridge_static)
elseif(AWJ_ENABLE_ZENRAVIF)
    message(STATUS "zenravif bridge sources were not found; experimental zenrav1e AVIF encoder will be unavailable in this build.")
else()
    message(STATUS "Experimental zenrav1e AVIF encoder disabled by AWJ_ENABLE_ZENRAVIF=OFF.")
endif()

# ponytail: keep avif module+impl on awj_native_codecs so decoder_registry can import it without a cycle.
target_link_libraries(awj_native_codecs PUBLIC avif)
target_compile_definitions(awj_native_codecs PUBLIC
    AWJ_HAS_SVTAV1HDR_STATIC=${AWJ_SVTAV1HDR_STATIC_DEFINE})
if(AWJ_ZENRAVIF_AVAILABLE)
    target_link_libraries(awj_native_codecs PUBLIC zenravif_bridge)
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_ZENRAVIF=1)
    add_dependencies(awj_native_codecs zenravif_bridge_static)
else()
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_ZENRAVIF=0)
endif()
target_sources(awj_native_codecs
    PRIVATE
        src/codecs/avif_aom_codec.cpp
    PUBLIC
        FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_aom_codec.ixx)
add_library(awj_avif_base_codec INTERFACE)
target_link_libraries(awj_avif_base_codec INTERFACE awj_native_codecs)

add_library(awj_svtav1hdr_codec STATIC)
avif_enable_common_options(awj_svtav1hdr_codec)
target_link_libraries(awj_svtav1hdr_codec PUBLIC
    awj_base
    awj_native_codecs
    avif)
target_compile_definitions(awj_svtav1hdr_codec PUBLIC
    AWJ_HAS_SVTAV1HDR_STATIC=${AWJ_SVTAV1HDR_STATIC_DEFINE})
if(AWJ_SVTAV1HDR_AVAILABLE)
    target_link_libraries(awj_svtav1hdr_codec PUBLIC SvtAv1Enc)
endif()
target_sources(awj_svtav1hdr_codec
    PUBLIC
        FILE_SET awj_svtav1hdr_codec_modules TYPE CXX_MODULES FILES
            src/codecs/svtav1hdr_codec.ixx)

add_library(awj_native_backend STATIC)
avif_enable_common_options(awj_native_backend)
target_link_libraries(awj_native_backend PUBLIC
    awj_base
    awj_raw_image_io
    awj_native_visual_search
    awj_native_codecs
    awj_avif_base_codec
    awj_svtav1hdr_codec)
add_dependencies(awj_native_backend
    awj_native_visual_search
    awj_native_codecs
    awj_avif_base_codec
    awj_svtav1hdr_codec)
target_sources(awj_native_backend
    PUBLIC
        FILE_SET awj_native_backend_modules TYPE CXX_MODULES FILES
            src/backends/native_backend.ixx)

add_library(awj_pipeline STATIC)
avif_enable_common_options(awj_pipeline)
target_link_libraries(awj_pipeline PUBLIC
    awj_base
    awj_native_backend)
target_sources(awj_pipeline
    PUBLIC
        FILE_SET awj_pipeline_modules TYPE CXX_MODULES FILES
            src/app/pipeline.ixx)

add_library(awj_core INTERFACE)
target_link_libraries(awj_core INTERFACE awj_pipeline)

# 更新系统的纯逻辑核心：版本比较、渠道过滤、候选判定和检查调度。
# 刻意不依赖 awj_base —— 这一层没有 I/O 也没有平台 API，独立成库才能让
# 「stable 用户不该看到 prerelease」这类安全规则用最轻的测试目标覆盖。
add_library(awj_update STATIC)
avif_enable_common_options(awj_update)
target_compile_definitions(awj_update PRIVATE
    AWJ_BUILD_VERSION="${PROJECT_VERSION}"
    AWJ_UPDATE_PUBLIC_KEY_HEX="${AWJ_UPDATE_PUBLIC_KEY_HEX}"
    AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX="${AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX}"
    AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX="${AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX}")
target_link_libraries(awj_update PUBLIC
    nlohmann_json::nlohmann_json
    unofficial-sodium::sodium
    LibArchive::LibArchive)
if(WIN32)
    target_link_libraries(awj_update PUBLIC winhttp bcrypt)
else()
    # update_linux imports awj.core for executable-path discovery.  Declare
    # the module provider here so GCC receives its CXX module mapper entry.
    target_link_libraries(awj_update PUBLIC awj_base CURL::libcurl)
endif()
target_sources(awj_update
    PUBLIC
        FILE_SET awj_update_modules TYPE CXX_MODULES FILES
            src/update/update_model.ixx
            src/update/update_manifest.ixx
            src/update/update_keyring.ixx
            src/update/update_manifest_v2.ixx
            src/update/update_security_state.ixx
            src/update/update_archive.ixx
            src/update/update_runtime.ixx)
if(WIN32)
    target_sources(awj_update
        PUBLIC
            FILE_SET awj_update_windows_modules TYPE CXX_MODULES FILES
                src/update/update_windows.ixx)
else()
    target_sources(awj_update
        PUBLIC
            FILE_SET awj_update_linux_modules TYPE CXX_MODULES FILES
                src/update/update_linux.ixx)
endif()

if(BUILD_TESTING)
    add_executable(awj_update_model_tests tests/update_model_tests.cpp)
    avif_enable_common_options(awj_update_model_tests)
    target_link_libraries(awj_update_model_tests PRIVATE awj_update)

    add_executable(awj_update_manifest_tests tests/update_manifest_tests.cpp)
    avif_enable_common_options(awj_update_manifest_tests)
    target_link_libraries(awj_update_manifest_tests PRIVATE awj_update)

    add_executable(awj_update_archive_tests tests/update_archive_tests.cpp)
    avif_enable_common_options(awj_update_archive_tests)
    target_link_libraries(awj_update_archive_tests PRIVATE awj_update)

    add_executable(awj_update_security_state_tests tests/update_security_state_tests.cpp)
    avif_enable_common_options(awj_update_security_state_tests)
    target_link_libraries(awj_update_security_state_tests PRIVATE awj_update)

    add_executable(awj_update_keyring_tests tests/update_keyring_tests.cpp)
    avif_enable_common_options(awj_update_keyring_tests)
    target_link_libraries(awj_update_keyring_tests PRIVATE awj_update)

    add_executable(awj_changelog_history_tests tests/changelog_history_tests.cpp)
    avif_enable_common_options(awj_changelog_history_tests)
    add_dependencies(awj_changelog_history_tests awj_changelog_data)
    target_include_directories(awj_changelog_history_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")

    add_executable(awj_visual_quality_tests tests/visual_quality_tests.cpp)
    avif_enable_common_options(awj_visual_quality_tests)
    target_link_libraries(awj_visual_quality_tests PRIVATE awj_base)

    add_executable(awj_hdr_tonemap_tests tests/hdr_tonemap_tests.cpp)
    avif_enable_common_options(awj_hdr_tonemap_tests)
    target_link_libraries(awj_hdr_tonemap_tests PRIVATE awj_base)

    add_executable(awj_config_defaults_tests tests/config_defaults_tests.cpp)
    avif_enable_common_options(awj_config_defaults_tests)
    target_link_libraries(awj_config_defaults_tests PRIVATE awj_base)

    add_executable(awj_process_tests tests/process_tests.cpp)
    avif_enable_common_options(awj_process_tests)
    target_link_libraries(awj_process_tests PRIVATE awj_base)

    add_executable(awj_resource_planner_tests tests/resource_planner_tests.cpp)
    avif_enable_common_options(awj_resource_planner_tests)
    target_link_libraries(awj_resource_planner_tests PRIVATE awj_base)

    add_executable(awj_visual_metrics_tests tests/visual_metrics_tests.cpp)
    avif_enable_common_options(awj_visual_metrics_tests)
    target_link_libraries(awj_visual_metrics_tests PRIVATE awj_base)

    add_executable(awj_webp_codec_tests tests/webp_codec_tests.cpp)
    avif_enable_common_options(awj_webp_codec_tests)
    target_link_libraries(awj_webp_codec_tests PRIVATE awj_native_codecs)

    add_executable(awj_png_codec_tests tests/png_codec_tests.cpp)
    avif_enable_common_options(awj_png_codec_tests)
    target_link_libraries(awj_png_codec_tests PRIVATE awj_native_codecs)

    add_executable(awj_avif_aom_codec_tests tests/avif_aom_codec_tests.cpp)
    avif_enable_common_options(awj_avif_aom_codec_tests)
    target_link_libraries(awj_avif_aom_codec_tests PRIVATE awj_avif_base_codec)

    add_executable(awj_avif_registry_tests tests/avif_registry_tests.cpp)
    avif_enable_common_options(awj_avif_registry_tests)
    target_link_libraries(awj_avif_registry_tests PRIVATE awj_native_codecs)

    add_executable(awj_jxl_codec_tests tests/jxl_codec_tests.cpp)
    avif_enable_common_options(awj_jxl_codec_tests)
    target_link_libraries(awj_jxl_codec_tests PRIVATE awj_native_codecs)

    add_executable(awj_bmp_codec_tests tests/bmp_codec_tests.cpp)
    avif_enable_common_options(awj_bmp_codec_tests)
    target_link_libraries(awj_bmp_codec_tests PRIVATE awj_native_codecs)

    if(WIN32)
        add_executable(awj_jxr_codec_tests tests/jxr_codec_tests.cpp)
        avif_enable_common_options(awj_jxr_codec_tests)
        target_link_libraries(awj_jxr_codec_tests PRIVATE awj_native_codecs)
    endif()

    if(AWJ_JPEGLI_AVAILABLE)
        add_executable(awj_jpegli_codec_tests tests/jpegli_codec_tests.cpp)
        avif_enable_common_options(awj_jpegli_codec_tests)
        target_link_libraries(awj_jpegli_codec_tests PRIVATE awj_native_codecs)
    endif()

    add_executable(awj_native_visual_search_tests tests/native_visual_search_tests.cpp)
    avif_enable_common_options(awj_native_visual_search_tests)
    target_link_libraries(awj_native_visual_search_tests PRIVATE awj_native_visual_search awj_native_codecs)

    add_executable(awj_decoder_registry_tests tests/decoder_registry_tests.cpp)
    avif_enable_common_options(awj_decoder_registry_tests)
    target_link_libraries(awj_decoder_registry_tests PRIVATE awj_native_backend)

    if(WIN32)
        add_executable(awj_native_pipeline_tests tests/native_pipeline_tests.cpp)
        avif_enable_common_options(awj_native_pipeline_tests)
        target_link_libraries(awj_native_pipeline_tests PRIVATE awj_native_backend)

        add_executable(awj_pipeline_security_tests tests/pipeline_security_tests.cpp)
        avif_enable_common_options(awj_pipeline_security_tests)
        target_link_libraries(awj_pipeline_security_tests PRIVATE awj_pipeline awj_native_codecs)
    endif()

    add_executable(awj_memory_cancel_tests tests/memory_cancel_tests.cpp)
    avif_enable_common_options(awj_memory_cancel_tests)
    target_link_libraries(awj_memory_cancel_tests PRIVATE awj_native_visual_search awj_native_codecs awj_raw_image_io)

    add_executable(awj_raw_wgc_stdin_tests tests/raw_wgc_stdin_tests.cpp)
    avif_enable_common_options(awj_raw_wgc_stdin_tests)
    target_link_libraries(awj_raw_wgc_stdin_tests PRIVATE awj_raw_image_io)

    if(WIN32)
        add_executable(awj_cli_config_entry tests/cli_config_entry.cpp)
        avif_enable_common_options(awj_cli_config_entry)
        target_link_libraries(awj_cli_config_entry PRIVATE awj_base)

        add_executable(awj_shell_context_menu_tests
            tests/shell_context_menu_tests.cpp)
        avif_enable_common_options(awj_shell_context_menu_tests)
        target_include_directories(awj_shell_context_menu_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")

        add_executable(awj_cli_native_avif_aom_tests tests/cli_native_avif_aom_tests.cpp)
        avif_enable_common_options(awj_cli_native_avif_aom_tests)
        target_link_libraries(awj_cli_native_avif_aom_tests PRIVATE awj_native_backend)
    endif()

    set(AWJ_TEST_TARGETS
        awj_update_model_tests
        awj_update_manifest_tests
        awj_update_archive_tests
        awj_update_security_state_tests
        awj_update_keyring_tests
        awj_changelog_history_tests
        awj_visual_quality_tests
        awj_hdr_tonemap_tests
        awj_config_defaults_tests
        awj_process_tests
        awj_resource_planner_tests
        awj_visual_metrics_tests
        awj_webp_codec_tests
        awj_png_codec_tests
        awj_avif_aom_codec_tests
        awj_avif_registry_tests
        awj_jxl_codec_tests
        awj_bmp_codec_tests
        awj_native_visual_search_tests
        awj_decoder_registry_tests
        awj_memory_cancel_tests
        awj_raw_wgc_stdin_tests)
    if(WIN32)
        list(APPEND AWJ_TEST_TARGETS
            awj_jxr_codec_tests
            awj_native_pipeline_tests
            awj_pipeline_security_tests
            awj_cli_config_entry
            awj_shell_context_menu_tests
            awj_cli_native_avif_aom_tests)
    endif()
    if(AWJ_JPEGLI_AVAILABLE)
        list(APPEND AWJ_TEST_TARGETS awj_jpegli_codec_tests)
    endif()
    foreach(test_target IN LISTS AWJ_TEST_TARGETS)
        awj_set_runtime_directory(${test_target} "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/tests")
    endforeach()

    add_test(NAME visual_quality_core COMMAND awj_visual_quality_tests)
    add_test(NAME update_model_core COMMAND awj_update_model_tests)
    add_test(NAME update_manifest_security COMMAND awj_update_manifest_tests)
    add_test(NAME update_archive_security COMMAND awj_update_archive_tests)
    add_test(NAME update_security_state COMMAND awj_update_security_state_tests)
    add_test(NAME update_keyring_security COMMAND awj_update_keyring_tests)
    add_test(NAME changelog_history_complete COMMAND awj_changelog_history_tests)
    add_test(NAME hdr_tonemap_cpu COMMAND awj_hdr_tonemap_tests)
    add_test(NAME config_defaults_core COMMAND awj_config_defaults_tests)
    add_test(NAME process_core COMMAND awj_process_tests)
    add_test(NAME resource_planner_core COMMAND awj_resource_planner_tests)
    add_test(NAME visual_metrics_core COMMAND awj_visual_metrics_tests)
    add_test(NAME webp_codec_core COMMAND awj_webp_codec_tests)
    add_test(NAME png_codec_core COMMAND awj_png_codec_tests)
    add_test(NAME avif_aom_codec_core COMMAND awj_avif_aom_codec_tests)
    add_test(NAME avif_registry_core COMMAND awj_avif_registry_tests)
    add_test(NAME jxl_codec_core COMMAND awj_jxl_codec_tests)
    add_test(NAME bmp_codec_core COMMAND awj_bmp_codec_tests)
    if(WIN32)
        add_test(NAME jxr_codec_core COMMAND awj_jxr_codec_tests)
        add_test(NAME shell_context_menu_model COMMAND awj_shell_context_menu_tests)
    endif()
    if(AWJ_JPEGLI_AVAILABLE)
        add_test(NAME jpegli_codec_core COMMAND awj_jpegli_codec_tests)
    endif()
    add_test(NAME native_visual_search_core COMMAND awj_native_visual_search_tests)
    add_test(NAME decoder_registry_core COMMAND awj_decoder_registry_tests)
    if(WIN32)
        add_test(NAME native_pipeline_core COMMAND awj_native_pipeline_tests)
        add_test(NAME pipeline_security_core COMMAND awj_pipeline_security_tests)
    endif()
    add_test(NAME memory_cancel_core COMMAND awj_memory_cancel_tests)
    add_test(NAME raw_wgc_stdin_core COMMAND awj_raw_wgc_stdin_tests)

    if(WIN32)
        add_test(NAME cli_help COMMAND awj_cli_config_entry --help)
    set_tests_properties(cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "AWJimage C[+][+]23")

    add_test(NAME cli_unknown_argument
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--definitely-unknown-option' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('未知参数')) { exit 1 }")

    add_test(NAME cli_rejects_nan_quality
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--quality' 'nan' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('质量参数必须是有限数字')) { exit 1 }")

    add_test(NAME cli_rejects_low_visual_quality
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--visual-quality' '0' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('visual-quality 范围必须在 1 到 100 之间')) { exit 1 }")

    add_test(NAME cli_rejects_high_visual_quality
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--visual-quality' '101' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('visual-quality 范围必须在 1 到 100 之间')) { exit 1 }")

    add_test(NAME cli_accepts_visual_quality_priority
        COMMAND awj_cli_config_entry --visual-quality 90 --quality 20 --help)

    add_test(NAME cli_native_avif_aom_converts_webp
        COMMAND awj_cli_native_avif_aom_tests)
    set_tests_properties(cli_native_avif_aom_converts_webp PROPERTIES TIMEOUT 30)

    add_test(NAME cli_rejects_svt_444
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--avif-encoder' 'svt' '--chroma' '444' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('420 chroma')) { exit 1 }")

    add_test(NAME cli_rejects_rav1e
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--avif-encoder' 'rav1e' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('AVIF encoder 不支持')) { exit 1 }")

    add_test(NAME cli_rejects_tiny_memory_limit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--memory-limit' '1MiB' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('memory-limit 不能低于 64MiB')) { exit 1 }")

    add_test(NAME cli_accepts_auto_memory_limit
        COMMAND awj_cli_config_entry --memory-limit auto --help)

    add_test(NAME cli_rejects_empty_option_key
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--option' '=x' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('--option 的 key 不能为空')) { exit 1 }")
    endif()
endif()

add_executable(awj_update_manifest_sign scripts/update_manifest_sign.cpp)
avif_enable_common_options(awj_update_manifest_sign)
target_link_libraries(awj_update_manifest_sign PRIVATE unofficial-sodium::sodium)

if(WIN32)
    add_executable(AWJ WIN32
        src/app/main.cpp
        src/cli/main.cpp
        src/ui/main.cpp
        src/ui/AWJ.manifest
        src/ui/AWJ.rc)
else()
    add_executable(AWJ
        src/app/main.cpp
        src/cli/main.cpp
        src/ui/main.cpp)
    set_target_properties(AWJ PROPERTIES SKIP_BUILD_RPATH TRUE)
endif()

set(AWJ_CHANGELOG_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/awj_changelog_data.h")
add_custom_command(
    OUTPUT "${AWJ_CHANGELOG_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT_ZH=${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
        "-DINPUT_EN=${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.en.md"
        "-DOUTPUT=${AWJ_CHANGELOG_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_changelog.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.en.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_changelog.cmake"
    COMMENT "Embedding bilingual changelog history"
    VERBATIM)
add_custom_target(awj_changelog_data DEPENDS "${AWJ_CHANGELOG_HEADER}")
add_dependencies(AWJ awj_changelog_data)
target_sources(AWJ PRIVATE "${AWJ_CHANGELOG_HEADER}")
target_include_directories(AWJ PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_sources(AWJ
    PRIVATE
        FILE_SET awj_studio_modules TYPE CXX_MODULES FILES
            src/ui/studio_defaults.ixx)
if(MSVC)
    set_source_files_properties(src/ui/main.cpp PROPERTIES
        COMPILE_OPTIONS "/wd4099;/wd4201;/wd4624")
endif()
avif_enable_common_options(AWJ)
target_compile_definitions(AWJ PRIVATE
    AWJ_UNIFIED_EXE
    AWJ_BUILD_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_link_options(AWJ PRIVATE "/ENTRY:wmainCRTStartup")
endif()
target_link_libraries(AWJ PRIVATE
    awj_core
    awj_update
    Slint::Slint)
if(TARGET slint_cpp)
    add_dependencies(AWJ slint_cpp)
endif()
if(WIN32)
    target_link_libraries(AWJ PRIVATE
        dwmapi
        advapi32
        gdi32
        ole32
        shell32
        uxtheme
        imm32
        opengl32)
endif()
if(MSVC AND AVIF_STATIC_SLINT AND AWJ_ZENRAVIF_AVAILABLE)
    # Both Slint's static C++ runtime and zenravif_bridge are Rust static libraries.
    # MSVC otherwise reports duplicate Rust std runtime symbols even when built by
    # the same toolchain for one final executable.
    target_link_options(AWJ PRIVATE "/FORCE:MULTIPLE")
endif()
avif_add_slint_sources(AWJ ui/awj_studio.slint)
avif_copy_slint_runtime(AWJ)
awj_add_sha256_post_build(AWJ)

if(BUILD_TESTING)
    add_executable(awj_ui_smoke_tests
        tests/ui_smoke_tests.cpp)
    avif_enable_common_options(awj_ui_smoke_tests)
    avif_add_slint_sources(awj_ui_smoke_tests ui/awj_studio.slint
        DEBUG_INFO
        OUTPUT_SUFFIX _ui_smoke)
    target_include_directories(awj_ui_smoke_tests PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(awj_ui_smoke_tests PRIVATE
        SLINT_FEATURE_EXPERIMENTAL=1)
    target_link_libraries(awj_ui_smoke_tests PRIVATE Slint::Slint)
    if(TARGET slint_cpp)
        add_dependencies(awj_ui_smoke_tests slint_cpp)
    endif()
    if(WIN32)
        target_link_libraries(awj_ui_smoke_tests PRIVATE
            dwmapi
            gdi32
            ole32
            shell32
            uxtheme
            imm32
            opengl32)
    endif()
    if(MSVC)
        set_source_files_properties(
            "${CMAKE_CURRENT_BINARY_DIR}/slint_generated_awj_studio_ui_smoke_1.cpp"
            PROPERTIES COMPILE_OPTIONS
                "/wd4060;/wd4099;/wd4201;/wd4244;/wd4267;/wd4305;/wd4458;/wd4624")
    endif()
    awj_set_runtime_directory(awj_ui_smoke_tests "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/tests")
    add_test(NAME ui_smoke COMMAND awj_ui_smoke_tests)
    set_tests_properties(ui_smoke PROPERTIES TIMEOUT 30)
endif()

if(WIN32)
    add_executable(AWJ-com src/cli/shim_main.cpp)
    avif_enable_common_options(AWJ-com)
    set_target_properties(AWJ-com PROPERTIES
        OUTPUT_NAME "AWJ"
        SUFFIX ".com"
        PDB_NAME "AWJ-com")
    awj_add_sha256_post_build(AWJ-com)
endif()
