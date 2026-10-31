cmake_minimum_required(VERSION 3.30)

# Read version from the single source-of-truth file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
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

option(AWJ_BUILD_APPLICATION "构建桌面程序、CLI 和更新器" ON)
option(AWJ_BUILD_SDK "构建仅公开 C ABI 的帧处理库" ON)
option(AWJ_SDK_NATIVE_CODECS "SDK 包含项目原生编码器" ON)
option(AWJ_ENABLE_GPU_METRICS "构建平台 GPU 视觉质量后端" ON)
set(AWJ_SDK_VERSION "${PROJECT_VERSION}")
if(AWJ_BUILD_SDK)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/awj-stdlib-compat.cmake")

option(AVIF_STATIC_SLINT "Link Slint statically so AWJ does not need slint_cpp.dll" ON)
option(AVIF_STATIC_MSVC_RUNTIME "Use /MT or /MTd for smaller redistribution bundles when all dependencies allow it" ON)
option(AVIF_ENABLE_RELEASE_IPO "Enable interprocedural optimization for Release builds when the compiler supports it" OFF)
option(AWJ_ENABLE_STRICT_MSVC_CHECKS "Enable MSVC /W4 and /sdl for project targets" ON)
option(AWJ_ENABLE_NATIVE_RENDERER_TESTS "注册需要桌面与 OpenGL 的真实 Winit/FemtoVG 回归" OFF)
option(AWJ_ENABLE_X64_V3 "Enable x64-v3/AVX2 code generation for project targets in Release builds" OFF)
option(AWJ_ENABLE_JPEGLI "Build Google Jpegli JPGLI codec support into AWJ" ON)
option(AWJ_PATCH_LIBAVIF_PARALLEL_GRID "Patch pinned libavif to encode simple still color grids in parallel" ON)
set(AWJ_UPDATE_PUBLIC_KEY_HEX "ebdb1a49b8686f34a5791f7c0f2a0215e4cdc03ad99281e5bf8606cb62fcf1b1" CACHE STRING
    "Ed25519 update-manifest public key as 64 lowercase hexadecimal characters")
set(AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX "652b506c2fbd71d75ae48cca3d0196cd5914605dd811c1a2b786c6bbd259f05f" CACHE STRING
    "Offline recovery root A public key for the signed updater keyring")
set(AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX "65a49e06e12dfc48bf26dbbcc5380d5fbd61198c6e7cd65af4f860dfe5b6f65a" CACHE STRING
    "Offline recovery root B public key for the signed updater keyring")
set(AWJ_LIBAVIF_GIT_REPOSITORY "https://github.com/AOMediaCodec/libavif.git" CACHE STRING "libavif repository used for the static AOM build")
set(AWJ_JPEGLI_GIT_REPOSITORY "https://github.com/google/jpegli.git" CACHE STRING "google/jpegli repository used for JPGLI support")
set(AWJ_SLINT_GIT_REPOSITORY "https://github.com/slint-ui/slint.git" CACHE STRING "Slint repository")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/codec-dependencies.json" AWJ_CODEC_DEPENDENCIES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/codec-dependencies.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-configuration.json")
string(JSON AWJ_LOCK_VERSION GET "${AWJ_CODEC_DEPENDENCIES}" awj_version)
if(NOT AWJ_LOCK_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "Review codec-dependencies.json for AWJ ${PROJECT_VERSION} before building")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-configuration.json" AWJ_VCPKG_CONFIGURATION)
string(JSON AWJ_LOCK_BASELINE GET "${AWJ_CODEC_DEPENDENCIES}" vcpkg_baseline)
string(JSON AWJ_CONFIG_BASELINE GET "${AWJ_VCPKG_CONFIGURATION}" default-registry baseline)
if(NOT AWJ_LOCK_BASELINE STREQUAL AWJ_CONFIG_BASELINE)
    message(FATAL_ERROR "The codec lock and vcpkg baseline disagree")
endif()
string(JSON AWJ_LOCAL_BUILD_COUNT LENGTH "${AWJ_CODEC_DEPENDENCIES}" local_build_sha256)
if(AWJ_LOCAL_BUILD_COUNT GREATER 0)
    math(EXPR AWJ_LOCAL_BUILD_LAST "${AWJ_LOCAL_BUILD_COUNT} - 1")
    foreach(AWJ_LOCAL_INDEX RANGE ${AWJ_LOCAL_BUILD_LAST})
        string(JSON AWJ_LOCAL_PATH MEMBER "${AWJ_CODEC_DEPENDENCIES}" local_build_sha256 ${AWJ_LOCAL_INDEX})
        string(JSON AWJ_LOCAL_EXPECTED GET "${AWJ_CODEC_DEPENDENCIES}" local_build_sha256 "${AWJ_LOCAL_PATH}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${AWJ_LOCAL_PATH}")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${AWJ_LOCAL_PATH}" AWJ_LOCAL_TEXT)
        string(REPLACE "\r\n" "\n" AWJ_LOCAL_TEXT "${AWJ_LOCAL_TEXT}")
        string(SHA256 AWJ_LOCAL_ACTUAL "${AWJ_LOCAL_TEXT}")
        if(NOT AWJ_LOCAL_ACTUAL STREQUAL AWJ_LOCAL_EXPECTED)
            message(FATAL_ERROR "${AWJ_LOCAL_PATH} differs from the version lock; review and update its hash")
        endif()
    endforeach()
endif()
foreach(AWJ_DEP IN ITEMS LIBAVIF JPEGLI SLINT)
    string(TOLOWER "${AWJ_DEP}" AWJ_DEP_KEY)
    string(JSON AWJ_LOCK_COMMIT GET "${AWJ_CODEC_DEPENDENCIES}" "${AWJ_DEP_KEY}" commit)
    set(AWJ_${AWJ_DEP}_GIT_TAG "${AWJ_LOCK_COMMIT}" CACHE STRING "Locked ${AWJ_DEP} commit")
    if(NOT AWJ_${AWJ_DEP}_GIT_TAG STREQUAL AWJ_LOCK_COMMIT)
        message(FATAL_ERROR "${AWJ_DEP} cached revision differs from the version lock; review/update the lock and cache together")
    endif()
    if(FETCHCONTENT_SOURCE_DIR_${AWJ_DEP})
        find_package(Git REQUIRED)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FETCHCONTENT_SOURCE_DIR_${AWJ_DEP}}" rev-parse HEAD
            OUTPUT_VARIABLE AWJ_OVERRIDE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE AWJ_OVERRIDE_RESULT)
        if(NOT AWJ_OVERRIDE_RESULT EQUAL 0 OR NOT AWJ_OVERRIDE_COMMIT STREQUAL AWJ_LOCK_COMMIT)
            message(FATAL_ERROR "${AWJ_DEP} source override must be a checkout of the locked commit")
        endif()
    endif()
endforeach()

set(AVIF_RELEASE_IPO_SUPPORTED OFF)
if(AVIF_ENABLE_RELEASE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AVIF_RELEASE_IPO_SUPPORTED
        OUTPUT AVIF_RELEASE_IPO_ERROR
        LANGUAGES CXX)
    if(NOT AVIF_RELEASE_IPO_SUPPORTED)
        message(FATAL_ERROR "Requested Release IPO/LTO is not available: ${AVIF_RELEASE_IPO_ERROR}")
    endif()
endif()

set(AWJ_RUNTIME_OUTPUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64" CACHE PATH "Application and test executable output root")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper}
        "${AWJ_RUNTIME_OUTPUT_ROOT}/${config}")
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
if(AWJ_BUILD_APPLICATION)
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
endif()
if(WIN32)
    find_package(libyuv CONFIG REQUIRED)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    if(AWJ_BUILD_APPLICATION)
        find_package(CURL REQUIRED)
    endif()
endif()

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
    target_link_libraries(${target} PUBLIC awj_stdlib)
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
                $<$<AND:$<CONFIG:Release>,$<NOT:$<PLATFORM_ID:OHOS>>>:-static-libstdc++>
                $<$<AND:$<CONFIG:Release>,$<NOT:$<PLATFORM_ID:OHOS>>>:-static-libgcc>
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

if(AWJ_ENABLE_GPU_METRICS)
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
else()
    add_custom_target(awj_visual_metric_shaders)
endif()

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# HEIC decode is deliberately self-contained: libheif + the libde265 backend only.
# Release tarballs and hashes are pinned so configure fails closed on source drift.
foreach(AWJ_DEP IN ITEMS LIBHEIF LIBDE265 LIBULTRAHDR)
    string(TOLOWER "${AWJ_DEP}" AWJ_DEP_KEY)
    string(JSON AWJ_${AWJ_DEP}_URL GET "${AWJ_CODEC_DEPENDENCIES}" "${AWJ_DEP_KEY}" url)
    string(JSON AWJ_${AWJ_DEP}_SHA256 GET "${AWJ_CODEC_DEPENDENCIES}" "${AWJ_DEP_KEY}" sha256)
endforeach()

set(AWJ_PARENT_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)
set(ENABLE_DECODER OFF CACHE BOOL "" FORCE)
set(ENABLE_ENCODER OFF CACHE BOOL "" FORCE)
set(ENABLE_SHERLOCK265 OFF CACHE BOOL "" FORCE)
set(ENABLE_INTERNAL_DEVELOPMENT_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL OFF CACHE BOOL "" FORCE)
set(WITH_FUZZERS OFF CACHE BOOL "" FORCE)
set(DE265_LOG_LEVEL none CACHE STRING "" FORCE)
FetchContent_Declare(libde265
    URL ${AWJ_LIBDE265_URL}
    URL_HASH SHA256=${AWJ_LIBDE265_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(libde265)
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND TARGET x86_sse)
    # 上游把 clang-cl 归入 MSVC 分支，漏掉仅 SSE4.1 dispatch 对象所需的 ISA。
    target_compile_options(x86_sse PRIVATE /clang:-msse4.1)
endif()

set(LIBDE265_INCLUDE_DIR "${libde265_SOURCE_DIR}" CACHE PATH "" FORCE)
set(LIBDE265_LIBRARY de265 CACHE STRING "" FORCE)
set(ENABLE_PLUGIN_LOADING OFF CACHE BOOL "" FORCE)
set(WITH_LIBDE265 ON CACHE BOOL "" FORCE)
set(WITH_LIBDE265_PLUGIN OFF CACHE BOOL "" FORCE)
foreach(AWJ_LIBHEIF_CODEC IN ITEMS
        X265 KVAZAAR UVG266 VVDEC VVENC X264 OpenH264_DECODER DAV1D
        AOM_DECODER AOM_ENCODER SvtEnc RAV1E JPEG_DECODER JPEG_ENCODER
        OpenJPEG_ENCODER OpenJPEG_DECODER FFMPEG_DECODER OPENJPH_ENCODER)
    set(WITH_${AWJ_LIBHEIF_CODEC} OFF CACHE BOOL "" FORCE)
    set(WITH_${AWJ_LIBHEIF_CODEC}_PLUGIN OFF CACHE BOOL "" FORCE)
endforeach()
set(WITH_UNCOMPRESSED_CODEC OFF CACHE BOOL "" FORCE)
set(WITH_WEBCODECS OFF CACHE BOOL "" FORCE)
set(WITH_LIBSHARPYUV OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLE_HEIF_THUMB OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLE_HEIF_VIEW OFF CACHE BOOL "" FORCE)
set(WITH_GDK_PIXBUF OFF CACHE BOOL "" FORCE)
set(BUILD_DEVELOPMENT_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPERIMENTAL_FEATURES OFF CACHE BOOL "" FORCE)
set(ENABLE_MULTITHREADING_SUPPORT ON CACHE BOOL "" FORCE)
set(ENABLE_PARALLEL_TILE_DECODING ON CACHE BOOL "" FORCE)
FetchContent_Declare(libheif
    URL ${AWJ_LIBHEIF_URL}
    URL_HASH SHA256=${AWJ_LIBHEIF_SHA256}
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=<SOURCE_DIR>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libheif_decoder_only.cmake"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(libheif)
execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${libheif_SOURCE_DIR}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libheif_gain_map.cmake"
    RESULT_VARIABLE AWJ_HEIF_GAIN_MAP_PATCH_RESULT)
if(NOT AWJ_HEIF_GAIN_MAP_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply libheif gain map container adapter")
endif()
# FetchContent does not rerun PATCH_COMMAND for an already-populated dependency.
# Apply the same idempotent decoder-only patch here as well so an incremental
# configure cannot silently retain libheif's built-in mask encoder.
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${libheif_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libheif_decoder_only.cmake"
    RESULT_VARIABLE AWJ_LIBHEIF_DECODER_ONLY_PATCH_RESULT)
if(NOT AWJ_LIBHEIF_DECODER_ONLY_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to enforce decoder-only libheif sources.")
endif()
# If the source tree was already populated, libheif's CMakeLists.txt was read
# before the patch above. Remove the encoder source from the already-created
# target too; a clean configure has nothing to remove here.
get_target_property(AWJ_LIBHEIF_SOURCES heif SOURCES)
if(AWJ_LIBHEIF_SOURCES)
    list(FILTER AWJ_LIBHEIF_SOURCES EXCLUDE REGEX "encoder_mask\\.(cc|h)$")
    set_property(TARGET heif PROPERTY SOURCES "${AWJ_LIBHEIF_SOURCES}")
endif()
# libheif's build-interface include path assumes its installed include layout.
# When embedded through FetchContent, expose the real API tree and the generated
# heif_version.h location to every in-tree consumer.
target_include_directories(heif PUBLIC
    $<BUILD_INTERFACE:${libheif_SOURCE_DIR}/libheif/api>
    $<BUILD_INTERFACE:${libheif_BINARY_DIR}>)
target_compile_definitions(heif PRIVATE LIBDE265_STATIC_BUILD)
# libheif 的上游 target 即使静态链接仍传播 LIBHEIF_EXPORTS，必须关闭 DLL 导出标记。
target_compile_definitions(heif PUBLIC LIBHEIF_STATIC_BUILD)
target_include_directories(heif PRIVATE "${libde265_BINARY_DIR}")
set(BUILD_TESTING ${AWJ_PARENT_BUILD_TESTING})

set(AVIF_CODEC_AOM SYSTEM CACHE STRING "" FORCE)
set(AVIF_CODEC_AOM_DECODE ON CACHE BOOL "" FORCE)
set(AVIF_CODEC_AOM_ENCODE ON CACHE BOOL "" FORCE)
set(AVIF_CODEC_DAV1D SYSTEM CACHE STRING "" FORCE)
set(AVIF_CODEC_SVT OFF CACHE STRING "" FORCE)
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
execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${libavif_SOURCE_DIR}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libavif_animation_budget.cmake"
    RESULT_VARIABLE AWJ_AVIF_ANIMATION_PATCH_RESULT)
if(NOT AWJ_AVIF_ANIMATION_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply libavif animation memory accounting patch")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${libavif_SOURCE_DIR}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libavif_primary_item.cmake"
    RESULT_VARIABLE AWJ_AVIF_PRIMARY_PATCH_RESULT)
if(NOT AWJ_AVIF_PRIMARY_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply pinned libavif independent-primary adapter")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libavif_gain_map_threads.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/avif_gain_map_thread_adapter.h")
execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${libavif_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libavif_gain_map_threads.cmake"
    RESULT_VARIABLE AWJ_AVIF_GAIN_THREADS_PATCH_RESULT)
if(NOT AWJ_AVIF_GAIN_THREADS_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply pinned libavif gain-map CPU/cancellation adapter")
endif()

# ISO 21496-1 JPEG, using the pinned libultrahdr numerical/container engine.
# Write switches alone do not disable the upstream decoder's XMP fallback.
set(UHDR_WRITE_XMP OFF CACHE BOOL "" FORCE)
set(UHDR_WRITE_ISO ON CACHE BOOL "" FORCE)
foreach(AWJ_UHDR_OPTION IN ITEMS BUILD_EXAMPLES BUILD_TESTS BUILD_BENCHMARK BUILD_FUZZERS
        BUILD_DEPS BUILD_JAVA BUILD_PACKAGING ENABLE_INSTALL ENABLE_HEIF ENABLE_GLES ENABLE_WERROR)
    set(UHDR_${AWJ_UHDR_OPTION} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(libultrahdr
    URL ${AWJ_LIBULTRAHDR_URL}
    URL_HASH SHA256=${AWJ_LIBULTRAHDR_SHA256}
    PATCH_COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_iso_only.cmake"
        COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_threads.cmake"
        COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=<SOURCE_DIR>" "-DAWJ_TARGET_SYSTEM=${CMAKE_SYSTEM_NAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_ohos.cmake"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS" AND FETCHCONTENT_SOURCE_DIR_LIBULTRAHDR)
    execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${FETCHCONTENT_SOURCE_DIR_LIBULTRAHDR}"
        "-DAWJ_TARGET_SYSTEM=OHOS" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_ohos.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
FetchContent_MakeAvailable(libultrahdr)
# Source overrides and populated FetchContent caches bypass PATCH_COMMAND.
# Recheck the exact patched function on every configure on both platforms.
execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${libultrahdr_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_iso_only.cmake"
    RESULT_VARIABLE AWJ_UHDR_ISO_PATCH_RESULT)
if(NOT AWJ_UHDR_ISO_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to enforce ISO 21496-1-only libultrahdr decoding")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${libultrahdr_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_threads.cmake"
    RESULT_VARIABLE AWJ_UHDR_THREADS_PATCH_RESULT)
if(NOT AWJ_UHDR_THREADS_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply libultrahdr CPU budget adapter")
endif()
if(TARGET uhdr-static)
    set(AWJ_UHDR_TARGET uhdr-static)
else()
    set(AWJ_UHDR_TARGET uhdr)
endif()

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

if(AWJ_BUILD_APPLICATION)
if(AVIF_STATIC_SLINT)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
else()
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
endif()
set(SLINT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SLINT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_TESTING ${BUILD_TESTING} CACHE BOOL "" FORCE)
set(SLINT_FEATURE_INTERPRETER OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_LIVE_PREVIEW OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_SYSTEM_TRAY OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_ACCESSIBILITY ON CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_VELLO OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_FEMTOVG ON CACHE BOOL "" FORCE)
string(JSON AWJ_WINIT_URL GET "${AWJ_CODEC_DEPENDENCIES}" winit url)
string(JSON AWJ_WINIT_SHA256 GET "${AWJ_CODEC_DEPENDENCIES}" winit sha256)
set(AWJ_WINIT_ARCHIVE "" CACHE FILEPATH "已校验的官方 winit 归档，可离线复用")
set(AWJ_WINIT_DOWNLOAD_DIR "${FETCHCONTENT_BASE_DIR}/awj-winit-download")
set(AWJ_WINIT_TEST_ARCHIVE "${AWJ_WINIT_DOWNLOAD_DIR}/winit-0.30.13.tar.gz")
if(AWJ_WINIT_ARCHIVE)
    file(SHA256 "${AWJ_WINIT_ARCHIVE}" AWJ_WINIT_ARCHIVE_SHA256)
    if(NOT AWJ_WINIT_ARCHIVE_SHA256 STREQUAL AWJ_WINIT_SHA256)
        message(FATAL_ERROR "winit source archive differs from the version lock")
    endif()
    set(AWJ_WINIT_URL "${AWJ_WINIT_ARCHIVE}")
    set(AWJ_WINIT_TEST_ARCHIVE "${AWJ_WINIT_ARCHIVE}")
endif()
FetchContent_Declare(awj_winit_sources
    URL "${AWJ_WINIT_URL}"
    DOWNLOAD_DIR "${AWJ_WINIT_DOWNLOAD_DIR}"
    URL_HASH "SHA256=${AWJ_WINIT_SHA256}"
    DOWNLOAD_NAME winit-0.30.13.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR awj-source-only)
FetchContent_MakeAvailable(awj_winit_sources)
execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${awj_winit_sources_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_winit_dpi.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${awj_winit_sources_SOURCE_DIR}"
    "-DEXPECTED_ARCHIVE_SHA256=${AWJ_WINIT_SHA256}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_winit_source.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
FetchContent_Declare(Slint
    GIT_REPOSITORY ${AWJ_SLINT_GIT_REPOSITORY}
    GIT_TAG ${AWJ_SLINT_GIT_TAG}
    UPDATE_DISCONNECTED TRUE
    SOURCE_SUBDIR api/cpp
    PATCH_COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=<SOURCE_DIR>"
        "-DAWJ_WINIT_SOURCE_DIR=${awj_winit_sources_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_slint_static_import.cmake")
# FetchContent source overrides bypass PATCH_COMMAND; apply the same contract.
if(FETCHCONTENT_SOURCE_DIR_SLINT)
    execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${FETCHCONTENT_SOURCE_DIR_SLINT}"
        "-DAWJ_WINIT_SOURCE_DIR=${awj_winit_sources_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_slint_static_import.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
FetchContent_MakeAvailable(Slint)
# 已缓存的 FetchContent 源码不会因脚本内容变化而重跑 PATCH_COMMAND。
execute_process(COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${slint_SOURCE_DIR}"
    "-DAWJ_WINIT_SOURCE_DIR=${awj_winit_sources_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_slint_static_import.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
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
endif()

function(avif_add_slint_sources target slint_file)
    set(options DEBUG_INFO)
    set(one_value_args OUTPUT_SUFFIX CPP_NAMESPACE)
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

    if(AWJ_SLINT_CPP_NAMESPACE)
        list(APPEND slint_compiler_command --cpp-namespace "${AWJ_SLINT_CPP_NAMESPACE}")
    endif()

    # Slint's depfile currently produces noisy MSBuild warnings when the repo path
    # contains spaces. List the page/component dependencies explicitly to keep
    # incremental UI builds correct without the depfile path ambiguity.
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
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/studio_components.slint"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/studio_theme.slint"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/parameter_page.slint"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/menu_parameter_page.slint"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/queue_page.slint"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/settings_page.slint"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/preset_editor.slint"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/shell_convert_window.slint"
            "${awj_translation_po}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/logo-light.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/logo-dark.png"
        COMMENT "Generating ${slint_base}.h"
        VERBATIM)

    target_sources(${target} PRIVATE "${generated_header}" "${generated_cpp}")
    if(MSVC)
        set_source_files_properties("${generated_cpp}" PROPERTIES
            COMPILE_OPTIONS "/wd4060;/wd4099;/wd4201;/wd4244;/wd4267;/wd4305;/wd4458;/wd4624")
    endif()
    target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>")
endfunction()

add_library(awj_shared STATIC)
avif_enable_common_options(awj_shared)
target_compile_definitions(awj_shared PRIVATE AWJ_BUILD_VERSION="${PROJECT_VERSION}")
target_link_libraries(awj_shared PUBLIC scn::scn nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(awj_shared PUBLIC bcrypt)
else()
    target_link_libraries(awj_shared PUBLIC Threads::Threads)
endif()
target_sources(awj_shared PUBLIC
    FILE_SET awj_shared_modules TYPE CXX_MODULES FILES
        src/core/encoding_defaults.ixx
        src/core/large_image_plan.ixx
        src/app/config.ixx
        src/core/visual_quality.ixx
        src/core/image.ixx
        src/core/animation.ixx
        src/core/resource_planner.ixx
        src/core/codec.ixx
        src/core/process.ixx
        src/codecs/avif_registry.ixx
)

if(AWJ_BUILD_APPLICATION)
    target_sources(awj_shared PUBLIC FILE_SET awj_shared_modules TYPE CXX_MODULES FILES
        src/app/preset.ixx src/app/batch_protocol.ixx)
elseif(WIN32 AND BUILD_TESTING AND AWJ_BUILD_SDK)
    # 内存 CLI 回归复用预设查询；HMOS/Linux SDK 没有此 importer。
    target_sources(awj_shared PUBLIC FILE_SET awj_shared_modules TYPE CXX_MODULES FILES
        src/app/preset.ixx)
endif()

add_library(awj_base STATIC)
avif_enable_common_options(awj_base)
add_dependencies(awj_base awj_visual_metric_shaders)
target_link_libraries(awj_base PUBLIC awj_shared awj_libplacebo)
if(NOT AWJ_ENABLE_GPU_METRICS)
    target_compile_definitions(awj_base PUBLIC AWJ_HAS_D3D11=0 AWJ_HAS_VULKAN=0)
elseif(WIN32)
    target_compile_definitions(awj_base PUBLIC AWJ_HAS_D3D11=1 AWJ_HAS_VULKAN=0)
else()
    target_link_libraries(awj_base PUBLIC Vulkan::Headers Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_definitions(awj_base PUBLIC AWJ_HAS_D3D11=0 AWJ_HAS_VULKAN=1)
endif()
if(AWJ_ENABLE_GPU_METRICS)
    target_sources(awj_base PRIVATE "${AWJ_VISUAL_SHADER_HEADER}")
    target_include_directories(awj_base PUBLIC "$<BUILD_INTERFACE:${AWJ_VISUAL_SHADER_GENERATED_DIR}>")
endif()
if(NOT WIN32)
    target_link_libraries(awj_base PUBLIC Threads::Threads)
endif()
target_sources(awj_base PUBLIC
    FILE_SET awj_base_modules TYPE CXX_MODULES FILES
        src/core/visual_metrics.ixx
        src/core/hdr_tonemap.ixx)

if(NOT AWJ_ENABLE_GPU_METRICS)
    target_sources(awj_base PUBLIC FILE_SET awj_base_modules TYPE CXX_MODULES FILES
        src/core/visual_metrics_gpu_disabled.ixx)
elseif(WIN32)
    target_sources(awj_base PUBLIC FILE_SET awj_base_modules TYPE CXX_MODULES FILES
        src/core/visual_metrics_gpu.ixx)
else()
    target_sources(awj_base PUBLIC FILE_SET awj_base_modules TYPE CXX_MODULES FILES
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

# gain_map/compressed_icc 是同一固定 libjxl 版本的官方 extras API，vcpkg 的
# tools=OFF 不安装其实现。仅编译这两个原始 utility TU，不重建或替换核心 codec。
string(JSON AWJ_JXL_SUPPORT_SHA256 GET "${AWJ_CODEC_DEPENDENCIES}" libjxl sha256)
set(AWJ_JXL_GAIN_MAP_ARCHIVE "" CACHE FILEPATH "Fixed libjxl source archive already downloaded by vcpkg")
if(NOT AWJ_JXL_GAIN_MAP_ARCHIVE)
    foreach(AWJ_JXL_DOWNLOAD_DIR IN ITEMS "${VCPKG_DOWNLOADS}" "$ENV{VCPKG_DOWNLOADS}" "${VCPKG_ROOT}/downloads")
        if(EXISTS "${AWJ_JXL_DOWNLOAD_DIR}/libjxl-libjxl-v0.12.0.tar.gz")
            set(AWJ_JXL_GAIN_MAP_ARCHIVE "${AWJ_JXL_DOWNLOAD_DIR}/libjxl-libjxl-v0.12.0.tar.gz" CACHE FILEPATH "Fixed libjxl source archive already downloaded by vcpkg" FORCE)
            break()
        endif()
    endforeach()
endif()
if(AWJ_JXL_GAIN_MAP_ARCHIVE)
    if(NOT EXISTS "${AWJ_JXL_GAIN_MAP_ARCHIVE}")
        message(FATAL_ERROR "Explicit AWJ_JXL_GAIN_MAP_ARCHIVE does not exist")
    endif()
    file(SHA256 "${AWJ_JXL_GAIN_MAP_ARCHIVE}" AWJ_JXL_ARCHIVE_ACTUAL)
    if(NOT AWJ_JXL_ARCHIVE_ACTUAL STREQUAL AWJ_JXL_SUPPORT_SHA256)
        message(FATAL_ERROR "Local libjxl archive differs from the codec version lock")
    endif()
    set(AWJ_JXL_SUPPORT_URL "${AWJ_JXL_GAIN_MAP_ARCHIVE}")
else()
    # 全新构建仍可从既有版本锁取归档；本次受限环境显式使用已核对的本地缓存。
    string(JSON AWJ_JXL_SUPPORT_URL GET "${AWJ_CODEC_DEPENDENCIES}" libjxl url)
endif()
FetchContent_Declare(awj_jxl_gainmap_sources
    URL "${AWJ_JXL_SUPPORT_URL}"
    URL_HASH "SHA256=${AWJ_JXL_SUPPORT_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR awj-source-only-no-cmake-entrypoint)
FetchContent_MakeAvailable(awj_jxl_gainmap_sources)
foreach(AWJ_JXL_UTILITY IN ITEMS lib/extras/gain_map.cc lib/extras/compressed_icc.cc)
    string(JSON AWJ_JXL_UTILITY_EXPECTED GET "${AWJ_CODEC_DEPENDENCIES}" libjxl gain_map_utility_sha256 "${AWJ_JXL_UTILITY}")
    file(SHA256 "${awj_jxl_gainmap_sources_SOURCE_DIR}/${AWJ_JXL_UTILITY}" AWJ_JXL_UTILITY_ACTUAL)
    if(NOT AWJ_JXL_UTILITY_ACTUAL STREQUAL AWJ_JXL_UTILITY_EXPECTED)
        message(FATAL_ERROR "libjxl utility ${AWJ_JXL_UTILITY} differs from pinned source")
    endif()
endforeach()
add_library(awj_jxl_gainmap_support STATIC
    "${awj_jxl_gainmap_sources_SOURCE_DIR}/lib/extras/gain_map.cc"
    "${awj_jxl_gainmap_sources_SOURCE_DIR}/lib/extras/compressed_icc.cc")
avif_enable_common_options(awj_jxl_gainmap_support)
target_include_directories(awj_jxl_gainmap_support PRIVATE "${awj_jxl_gainmap_sources_SOURCE_DIR}")
target_link_libraries(awj_jxl_gainmap_support PUBLIC JXL::jxl)

add_library(awj_native_codecs STATIC)
avif_enable_common_options(awj_native_codecs)
target_link_libraries(awj_native_codecs PUBLIC
    awj_base
    awj_raw_image_io
    WebP::webp
    WebP::webpdemux
    WebP::libwebpmux
    JXL::jxl
    awj_jxl_gainmap_support
    PNG::PNG
    JPEG::JPEG
    GIF::GIF
    TIFF::TIFF
    libraw::raw_r
    heif
    ${AWJ_UHDR_TARGET})
target_include_directories(awj_native_codecs PUBLIC
    "${libheif_SOURCE_DIR}/libheif/api"
    "${libheif_BINARY_DIR}")
target_include_directories(awj_native_codecs PRIVATE "${libultrahdr_SOURCE_DIR}/lib/include")
# JPEGli 导出 ABI-62 兼容头；普通 JPEG decoder 必须使用所链接的 libjpeg-turbo 头。
target_compile_definitions(awj_native_codecs PRIVATE
    AWJ_TURBO_JPEG_HEADER="${JPEG_INCLUDE_DIR}/jpeglib.h")
target_link_libraries(awj_native_codecs PUBLIC "${JXL_LCMS_LIBRARY}")
if(WIN32)
    target_link_libraries(awj_native_codecs PUBLIC ole32 windowscodecs)
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_WINDOWS_CODECS=1)
else()
    target_compile_definitions(awj_native_codecs PUBLIC AWJ_HAS_WINDOWS_CODECS=0)
endif()
target_sources(awj_native_codecs
    PUBLIC
        FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
            src/codecs/decoder_common.ixx
            src/core/pixel_conversion.ixx
            src/codecs/gain_map.ixx
            src/codecs/animation_decoder.ixx
            src/codecs/decoder_registry.ixx
            src/codecs/webp_codec.ixx
            src/codecs/jxl_codec.ixx
            src/codecs/png_codec.ixx
            src/codecs/bmp_codec.ixx
            src/codecs/jpeg_codec.ixx
            src/codecs/gif_codec.ixx
            src/codecs/tiff_codec.ixx
            src/codecs/libraw_codec.ixx
            src/codecs/heif_codec.ixx)
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
                src/codecs/jpegli_codec.ixx
                src/codecs/jpeg_hdr_codec.ixx)
endif()

# ponytail: keep avif module+impl on awj_native_codecs so decoder_registry can import it without a cycle.
target_link_libraries(awj_native_codecs PUBLIC avif)
target_sources(awj_native_codecs
    PRIVATE
        src/codecs/avif_aom_codec.cpp
    PUBLIC
        FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_aom_codec.ixx)
add_library(awj_avif_base_codec INTERFACE)
target_link_libraries(awj_avif_base_codec INTERFACE awj_native_codecs)

add_library(awj_native_backend STATIC)
avif_enable_common_options(awj_native_backend)
target_link_libraries(awj_native_backend PUBLIC
    awj_base
    awj_raw_image_io
    awj_native_visual_search
    awj_native_codecs
    awj_avif_base_codec)
add_dependencies(awj_native_backend
    awj_native_visual_search
    awj_native_codecs
    awj_avif_base_codec)
target_sources(awj_native_backend
    PUBLIC
        FILE_SET awj_native_backend_modules TYPE CXX_MODULES FILES
            src/backends/native_backend.ixx)

if(AWJ_BUILD_APPLICATION)
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
endif()

if(AWJ_BUILD_SDK)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk-targets.cmake")
endif()
if(NOT AWJ_BUILD_APPLICATION)
    return()
endif()

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
    target_link_libraries(awj_update PUBLIC awj_shared CURL::libcurl)
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

    add_executable(awj_user_preset_tests tests/user_preset_tests.cpp)
    avif_enable_common_options(awj_user_preset_tests)
    target_link_libraries(awj_user_preset_tests PRIVATE awj_base)
    awj_set_runtime_directory(awj_user_preset_tests "${CMAKE_CURRENT_BINARY_DIR}/preset-contract")
    add_test(NAME user_preset_contract COMMAND awj_user_preset_tests)

    add_executable(awj_process_tests tests/process_tests.cpp)
    avif_enable_common_options(awj_process_tests)
    target_link_libraries(awj_process_tests PRIVATE awj_base)

    add_executable(awj_resource_planner_tests tests/resource_planner_tests.cpp)
    avif_enable_common_options(awj_resource_planner_tests)
    target_link_libraries(awj_resource_planner_tests PRIVATE awj_base)

    add_executable(awj_optional_graphics_tests tests/optional_graphics_tests.cpp)
    avif_enable_common_options(awj_optional_graphics_tests)
    target_link_libraries(awj_optional_graphics_tests PRIVATE awj_base)
    add_executable(awj_visual_metrics_tests tests/visual_metrics_tests.cpp)
    avif_enable_common_options(awj_visual_metrics_tests)
    target_link_libraries(awj_visual_metrics_tests PRIVATE awj_base)

    add_executable(awj_webp_codec_tests tests/webp_codec_tests.cpp)
    avif_enable_common_options(awj_webp_codec_tests)
    target_link_libraries(awj_webp_codec_tests PRIVATE awj_native_codecs)

    add_executable(awj_png_codec_tests tests/png_codec_tests.cpp)
    avif_enable_common_options(awj_png_codec_tests)
    target_link_libraries(awj_png_codec_tests PRIVATE awj_native_codecs)

    add_executable(awj_pixel_conversion_tests tests/pixel_conversion_tests.cpp)
    avif_enable_common_options(awj_pixel_conversion_tests)
    target_link_libraries(awj_pixel_conversion_tests PRIVATE awj_native_codecs)
    target_compile_definitions(awj_pixel_conversion_tests PRIVATE AWJ_HEIF_TEST_DATA="${libheif_SOURCE_DIR}/tests/data")
    add_test(NAME awj_pixel_conversion_tests COMMAND awj_pixel_conversion_tests)

    add_executable(awj_avif_aom_codec_tests tests/avif_aom_codec_tests.cpp)
    avif_enable_common_options(awj_avif_aom_codec_tests)
    target_link_libraries(awj_avif_aom_codec_tests PRIVATE awj_avif_base_codec)

    add_executable(awj_avif_hybrid_tests tests/avif_hybrid_tests.cpp)
    avif_enable_common_options(awj_avif_hybrid_tests)
    target_link_libraries(awj_avif_hybrid_tests PRIVATE awj_avif_base_codec)

    add_executable(awj_avif_gain_map_tests tests/avif_gain_map_tests.cpp)
    avif_enable_common_options(awj_avif_gain_map_tests)
    target_link_libraries(awj_avif_gain_map_tests PRIVATE awj_avif_base_codec)

    # Isolated libavif math tests: no application modules/encoders are required.
    # A second, symbol-renamed C translation unit permits deterministic faults
    # without exposing any failure-injection switches in the production library.
    set(AWJ_GAIN_FAULT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/awj_gain_map_fault_injection.c")
    file(GENERATE OUTPUT "${AWJ_GAIN_FAULT_SOURCE}" CONTENT
"#define AWJ_AVIF_GAIN_MAP_TESTING 1
#define avifRGBImageApplyGainMap awjTestOfficialRGBApply
#define avifImageApplyGainMap awjTestOfficialApply
#define avifFindMinMaxWithoutOutliers awjTestOfficialRange
#define avifGainMapValidateMetadata awjTestOfficialValidate
#define avifSameGainMapMetadata awjTestOfficialSameMetadata
#define avifSameGainMapAltMetadata awjTestOfficialSameAltMetadata
#define avifRGBImageComputeGainMap awjTestOfficialRGBCompute
#define avifImageComputeGainMap awjTestOfficialCompute
#define awjAvifRGBImageApplyGainMap awjTestRGBApply
#define awjAvifRGBImageComputeGainMap awjTestRGBCompute
#include \"${libavif_SOURCE_DIR}/src/gainmap.c\"
")
    # 根测试目标必须加载 C 编译器配置，不能仅依赖子目录启用的 C。
    enable_language(C)
    if(AVIF_RELEASE_IPO_SUPPORTED)
        check_ipo_supported(RESULT AWJ_GAIN_MAP_C_IPO_SUPPORTED
            OUTPUT AWJ_GAIN_MAP_C_IPO_ERROR LANGUAGES C)
        if(NOT AWJ_GAIN_MAP_C_IPO_SUPPORTED)
            message(FATAL_ERROR "Gain Map C test IPO is unavailable: ${AWJ_GAIN_MAP_C_IPO_ERROR}")
        endif()
    endif()
    add_executable(awj_gain_map_parallel_tests tests/gain_map_parallel_tests.cpp "${AWJ_GAIN_FAULT_SOURCE}")
    avif_enable_common_options(awj_gain_map_parallel_tests)
    set_target_properties(awj_gain_map_parallel_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_link_libraries(awj_gain_map_parallel_tests PRIVATE avif)
    if(WIN32)
        target_link_libraries(awj_gain_map_parallel_tests PRIVATE psapi)
    endif()
    add_test(NAME gain_map_parallel_core COMMAND awj_gain_map_parallel_tests)
    add_test(NAME avif_gain_map_threads_patch_contract COMMAND "${CMAKE_COMMAND}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/gain-map-threads-patch-contract"
        "-DLIBAVIF_SOURCE_DIR=${libavif_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/avif_gain_map_threads_patch_tests.cmake")
    set_tests_properties(gain_map_parallel_core avif_gain_map_threads_patch_contract PROPERTIES TIMEOUT 180)

    add_executable(awj_iso_gain_map_tests tests/iso_gain_map_tests.cpp)
    avif_enable_common_options(awj_iso_gain_map_tests)
    target_link_libraries(awj_iso_gain_map_tests PRIVATE awj_native_codecs)
    target_compile_definitions(awj_iso_gain_map_tests PRIVATE
        AWJ_TEST_TURBO_HEADER="${JPEG_INCLUDE_DIR}/jpeglib.h"
        AWJ_ISO_TEST_HEIF_DATA="${libheif_SOURCE_DIR}/tests/data"
        AWJ_ISO_TEST_AVIF_DATA="${libavif_SOURCE_DIR}/tests/data")

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
        add_executable(awj_jpegli_codec_tests tests/jpegli_codec_tests.cpp tests/jpeg_hdr_compat.cpp)
        target_compile_definitions(awj_jpegli_codec_tests PRIVATE AWJ_TEST_TURBO_HEADER="${JPEG_INCLUDE_DIR}/jpeglib.h")
        avif_enable_common_options(awj_jpegli_codec_tests)
        target_link_libraries(awj_jpegli_codec_tests PRIVATE awj_native_codecs)

        add_executable(awj_jpeg_hdr_iso_tests tests/jpeg_hdr_iso_tests.cpp)
        avif_enable_common_options(awj_jpeg_hdr_iso_tests)
        target_link_libraries(awj_jpeg_hdr_iso_tests PRIVATE awj_native_codecs)
        target_include_directories(awj_jpeg_hdr_iso_tests PRIVATE "${libultrahdr_SOURCE_DIR}/lib/include")
    endif()

    add_executable(awj_native_visual_search_tests tests/native_visual_search_tests.cpp)
    avif_enable_common_options(awj_native_visual_search_tests)
    target_link_libraries(awj_native_visual_search_tests PRIVATE awj_native_visual_search awj_native_codecs)

    add_executable(awj_decoder_registry_tests tests/decoder_registry_tests.cpp)
    avif_enable_common_options(awj_decoder_registry_tests)
    target_link_libraries(awj_decoder_registry_tests PRIVATE awj_native_backend)

    add_executable(awj_heif_backend_tests tests/heif_backend_tests.cpp)
    avif_enable_common_options(awj_heif_backend_tests)
    target_link_libraries(awj_heif_backend_tests PRIVATE heif)

    if(AWJ_JPEGLI_AVAILABLE)
        add_executable(awj_rendition_policy_tests tests/rendition_policy_tests.cpp)
        avif_enable_common_options(awj_rendition_policy_tests)
        target_link_libraries(awj_rendition_policy_tests PRIVATE awj_native_backend)
    endif()

    if(WIN32)
        add_executable(awj_native_pipeline_tests tests/native_pipeline_tests.cpp)
        avif_enable_common_options(awj_native_pipeline_tests)
        target_link_libraries(awj_native_pipeline_tests PRIVATE awj_native_backend)

    endif()

    add_executable(awj_pipeline_security_tests tests/pipeline_security_tests.cpp)
    avif_enable_common_options(awj_pipeline_security_tests)
    target_link_libraries(awj_pipeline_security_tests PRIVATE awj_pipeline awj_native_codecs)

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

        add_executable(awj_cli_native_avif_aom_tests tests/cli_native_avif_aom_tests.cpp)
        avif_enable_common_options(awj_cli_native_avif_aom_tests)
        target_link_libraries(awj_cli_native_avif_aom_tests PRIVATE awj_native_backend)
    endif()

    add_executable(awj_animation_decoder_tests tests/animation_decoder_tests.cpp)
    avif_enable_common_options(awj_animation_decoder_tests)
    target_link_libraries(awj_animation_decoder_tests PRIVATE awj_native_codecs ZLIB::ZLIB)
    set(AWJ_TEST_TARGETS
        awj_animation_decoder_tests
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
        awj_optional_graphics_tests
        awj_visual_metrics_tests
        awj_webp_codec_tests
        awj_png_codec_tests
        awj_avif_aom_codec_tests
        awj_avif_gain_map_tests
        awj_gain_map_parallel_tests
        awj_avif_hybrid_tests
        awj_iso_gain_map_tests
        awj_avif_registry_tests
        awj_jxl_codec_tests
        awj_bmp_codec_tests
        awj_native_visual_search_tests
        awj_decoder_registry_tests
        awj_heif_backend_tests
        awj_memory_cancel_tests
        awj_raw_wgc_stdin_tests
        awj_pipeline_security_tests)
    if(WIN32)
        list(APPEND AWJ_TEST_TARGETS
            awj_jxr_codec_tests
            awj_native_pipeline_tests
            awj_cli_config_entry
            awj_cli_native_avif_aom_tests)
    endif()
    if(AWJ_JPEGLI_AVAILABLE)
        list(APPEND AWJ_TEST_TARGETS awj_jpegli_codec_tests awj_jpeg_hdr_iso_tests)
    endif()
    foreach(test_target IN LISTS AWJ_TEST_TARGETS)
        awj_set_runtime_directory(${test_target} "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    endforeach()

    add_test(NAME visual_quality_core COMMAND awj_visual_quality_tests)
    add_test(NAME animation_decoder_core COMMAND awj_animation_decoder_tests)
    add_test(NAME update_model_core COMMAND awj_update_model_tests)
    add_test(NAME update_manifest_security COMMAND awj_update_manifest_tests)
    add_test(NAME update_archive_security COMMAND awj_update_archive_tests)
    add_test(NAME update_security_state COMMAND awj_update_security_state_tests)
    add_test(NAME update_keyring_security COMMAND awj_update_keyring_tests)
    add_test(NAME changelog_history_complete
        COMMAND awj_changelog_history_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.en.md")
    add_test(NAME changelog_chunk_boundary
        COMMAND ${CMAKE_COMMAND}
            "-DEMBED_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_changelog.cmake"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/changelog-chunk-boundary-test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/changelog_chunk_boundary_test.cmake")
    add_test(NAME hdr_tonemap_cpu COMMAND awj_hdr_tonemap_tests)
    add_test(NAME config_defaults_core COMMAND awj_config_defaults_tests)
    add_test(NAME process_core COMMAND awj_process_tests)
    add_test(NAME resource_planner_core COMMAND awj_resource_planner_tests)
    add_test(NAME visual_metrics_core COMMAND awj_visual_metrics_tests)
    add_test(NAME optional_graphics_core COMMAND awj_optional_graphics_tests)
    set_tests_properties(optional_graphics_core PROPERTIES TIMEOUT 60)
    add_test(NAME webp_codec_core COMMAND awj_webp_codec_tests)
    add_test(NAME png_codec_core COMMAND awj_png_codec_tests)
    add_test(NAME avif_aom_codec_core COMMAND awj_avif_aom_codec_tests)
    add_test(NAME avif_gain_map_core COMMAND awj_avif_gain_map_tests)
    set_tests_properties(avif_gain_map_core PROPERTIES TIMEOUT 120)
    add_test(NAME avif_hybrid_core COMMAND awj_avif_hybrid_tests "${CMAKE_CURRENT_BINARY_DIR}/hybrid-results")
    set_tests_properties(avif_hybrid_core PROPERTIES TIMEOUT 120)
    add_test(NAME iso_gain_map_core COMMAND awj_iso_gain_map_tests)
    add_test(NAME heif_grid_12_4 COMMAND awj_iso_gain_map_tests --grid-12-4-only)
    add_test(NAME ultrahdr_iso_patch_contract COMMAND "${CMAKE_COMMAND}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/ultrahdr-iso-patch-test"
        "-DPATCH_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_iso_only.cmake"
        "-DTHREAD_PATCH_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libultrahdr_threads.cmake"
        "-DLIBULTRAHDR_SOURCE_DIR=${libultrahdr_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ultrahdr_iso_patch_tests.cmake")
    add_test(NAME avif_primary_item_patch_contract COMMAND "${CMAKE_COMMAND}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/avif-primary-item-patch-test"
        "-DPATCH_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_libavif_primary_item.cmake"
        "-DLIBAVIF_SOURCE_DIR=${libavif_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/avif_primary_item_patch_tests.cmake")
    set_tests_properties(iso_gain_map_core ultrahdr_iso_patch_contract avif_primary_item_patch_contract PROPERTIES TIMEOUT 120)
    add_test(NAME avif_registry_core COMMAND awj_avif_registry_tests)
    add_test(NAME jxl_codec_core COMMAND awj_jxl_codec_tests)
    add_test(NAME bmp_codec_core COMMAND awj_bmp_codec_tests)
    if(WIN32)
        add_test(NAME jxr_codec_core COMMAND awj_jxr_codec_tests)
    endif()
    if(AWJ_JPEGLI_AVAILABLE)
        add_test(NAME jpegli_codec_core COMMAND awj_jpegli_codec_tests)
        add_test(NAME jpeg_hdr_iso_core COMMAND awj_jpeg_hdr_iso_tests)
        add_test(NAME rendition_policy_core COMMAND awj_rendition_policy_tests
            "${CMAKE_CURRENT_BINARY_DIR}/rendition-policy-tests")
        set_tests_properties(jpeg_hdr_iso_core PROPERTIES TIMEOUT 120)
        set_tests_properties(rendition_policy_core PROPERTIES TIMEOUT 180)
    endif()
    add_test(NAME native_visual_search_core COMMAND awj_native_visual_search_tests)
    add_test(NAME decoder_registry_core COMMAND awj_decoder_registry_tests)
    add_test(NAME heif_backend_contract COMMAND awj_heif_backend_tests)
    if(WIN32)
        add_test(NAME native_pipeline_core COMMAND awj_native_pipeline_tests)
    endif()
    add_test(NAME pipeline_security_core COMMAND awj_pipeline_security_tests)
    add_test(NAME memory_cancel_core COMMAND awj_memory_cancel_tests)
    add_test(NAME raw_wgc_stdin_core COMMAND awj_raw_wgc_stdin_tests)

    if(WIN32)
        add_test(NAME cli_help COMMAND awj_cli_config_entry --help)
    set_tests_properties(cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "AWJimage C[+][+]23")

    add_test(NAME cli_unknown_argument
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--definitely-unknown-option' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('未知参数')) { exit 1 }")

    add_test(NAME cli_rejects_nan_quality
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--quality' 'nan' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('质量参数必须是有限数字')) { exit 1 }")

    add_test(NAME cli_rejects_low_visual_quality
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--visual-quality' '0' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('visual-quality 范围必须在 1 到 100 之间')) { exit 1 }")

    add_test(NAME cli_rejects_high_visual_quality
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--visual-quality' '101' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('visual-quality 范围必须在 1 到 100 之间')) { exit 1 }")

    add_test(NAME cli_accepts_visual_quality_priority
        COMMAND awj_cli_config_entry --visual-quality 90 --quality 20 --help)

    add_test(NAME cli_native_avif_aom_converts_webp
        COMMAND awj_cli_native_avif_aom_tests)
    set_tests_properties(cli_native_avif_aom_converts_webp PROPERTIES TIMEOUT 30)

    add_test(NAME cli_rejects_removed_encoder
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--avif-encoder' 'svt' '--chroma' '444' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('已移除')) { exit 1 }")

    add_test(NAME cli_rejects_rav1e
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--avif-encoder' 'rav1e' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('AVIF encoder 不支持')) { exit 1 }")

    add_test(NAME cli_rejects_tiny_memory_limit
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--memory-limit' '1MiB' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('memory-limit 不能低于 64MiB')) { exit 1 }")

    add_test(NAME cli_accepts_auto_memory_limit
        COMMAND awj_cli_config_entry --memory-limit auto --help)

    add_test(NAME cli_rejects_empty_option_key
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--option' '=x' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('--option 的 key 不能为空')) { exit 1 }")
    endif()
endif()

add_executable(awj_update_manifest_sign scripts/update_manifest_sign.cpp)
avif_enable_common_options(awj_update_manifest_sign)
target_link_libraries(awj_update_manifest_sign PRIVATE unofficial-sodium::sodium)

if(WIN32)
    add_library(awj_shell_context_menu STATIC
        src/ui/menu_config_journal.cpp
        src/ui/menu_transaction_state.cpp
        src/ui/shell_context_menu.cpp
        src/ui/shell_elevation.cpp)
    avif_enable_common_options(awj_shell_context_menu)
    target_include_directories(awj_shell_context_menu PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
    target_link_libraries(awj_shell_context_menu PUBLIC shlwapi shell32 ole32)
endif()

if(WIN32)
    add_executable(AWJ WIN32
        src/app/main.cpp
        src/ui/import_service.cpp
        src/ui/file_drop_win32.cpp
        src/ui/path_picker_win32.cpp
        src/ui/studio_config.cpp
        src/ui/studio_json.cpp
        src/ui/studio_worker_events.cpp
        src/ui/studio_queue_format.cpp
        src/ui/studio_fonts.cpp
        src/ui/studio_fields.cpp
        src/ui/studio_menu_params.cpp
        src/ui/studio_parameter_page.cpp
        src/ui/studio_ui_util.cpp
        src/ui/studio_worker_control.cpp
        src/ui/studio_shell_cli.cpp
        src/ui/studio_shell_menu.cpp
        src/ui/studio_encode_dispatch.cpp
        src/ui/studio_queue_rows.cpp
        src/ui/studio_update_ui.cpp
        src/ui/studio_config_io.cpp
        src/ui/studio_import.cpp
        src/ui/studio_queue_import.cpp
        src/ui/studio_template_theme.cpp
        src/ui/studio_import_dispatch.cpp
        src/ui/studio_presets.cpp
        src/ui/studio_run_windows.cpp
        src/ui/studio_run_linux.cpp
        src/ui/main.cpp
        src/ui/AWJ.manifest
        src/ui/AWJ.rc)
else()
    add_executable(AWJ
        src/app/main.cpp
        src/cli/main.cpp
        src/ui/import_service.cpp
        src/ui/studio_run_linux.cpp
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
if(MSVC)
    target_link_options(AWJ PRIVATE "$<$<CONFIG:Release>:/MAP:${CMAKE_CURRENT_BINARY_DIR}/AWJ.map>")
else()
    target_link_options(AWJ PRIVATE "$<$<CONFIG:Release>:-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/AWJ.map>")
endif()
target_compile_definitions(AWJ PRIVATE
    AWJ_UNIFIED_EXE
    AWJ_BUILD_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_link_options(AWJ PRIVATE "/ENTRY:wmainCRTStartup")
    target_link_options(AWJ PRIVATE "/DELAYLOAD:opengl32.dll" "/DELAYLOAD:dwmapi.dll")
    target_link_libraries(AWJ PRIVATE delayimp)
endif()
target_link_libraries(AWJ PRIVATE awj_update Slint::Slint)
if(WIN32)
    target_link_libraries(AWJ PRIVATE awj_shared)
else()
    target_link_libraries(AWJ PRIVATE awj_core)
endif()
if(TARGET slint_cpp)
    add_dependencies(AWJ slint_cpp)
endif()
if(WIN32)
    target_link_libraries(AWJ PRIVATE
        awj_shell_context_menu
        dwmapi
        gdi32
        ole32
        shell32
        uxtheme
        imm32
        opengl32)
endif()
avif_add_slint_sources(AWJ ui/awj_studio.slint)
avif_copy_slint_runtime(AWJ)
awj_add_sha256_post_build(AWJ)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(awj_linux_queue_tests tests/linux_queue_tests.cpp src/ui/import_service.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/slint_generated_awj_studio_1.cpp")
    target_sources(awj_linux_queue_tests PRIVATE
        FILE_SET linux_queue_test_modules TYPE CXX_MODULES FILES src/ui/studio_defaults.ixx)
    avif_enable_common_options(awj_linux_queue_tests)
    target_compile_definitions(awj_linux_queue_tests PRIVATE SLINT_FEATURE_EXPERIMENTAL=1 AWJ_BUILD_VERSION="${PROJECT_VERSION}")
    target_include_directories(awj_linux_queue_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(awj_linux_queue_tests PRIVATE awj_core awj_update Slint::Slint)
    add_dependencies(awj_linux_queue_tests AWJ)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/queue-contract-temp")
    add_test(NAME linux_release_path_contract COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_release_paths_tests.sh")
    add_test(NAME linux_queue_contract COMMAND awj_linux_queue_tests
        "${CMAKE_CURRENT_BINARY_DIR}/queue-contract-fixtures")
    set_tests_properties(linux_queue_contract PROPERTIES TIMEOUT 120
        ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/queue-contract-temp")
endif()

if(BUILD_TESTING AND WIN32)
    add_executable(awj_update_windows_cleanup_tests tests/update_windows_cleanup_tests.cpp)
    avif_enable_common_options(awj_update_windows_cleanup_tests)
    target_link_libraries(awj_update_windows_cleanup_tests PRIVATE awj_update)
    target_compile_definitions(awj_update_windows_cleanup_tests PRIVATE AWJ_BUILD_VERSION="${PROJECT_VERSION}")
    awj_set_runtime_directory(awj_update_windows_cleanup_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    add_test(NAME update_windows_cleanup COMMAND awj_update_windows_cleanup_tests)

    add_executable(awj_shell_context_menu_logic_tests
        tests/shell_context_menu_logic_tests.cpp)
    avif_enable_common_options(awj_shell_context_menu_logic_tests)
    target_link_libraries(awj_shell_context_menu_logic_tests PRIVATE awj_shell_context_menu)
    awj_set_runtime_directory(awj_shell_context_menu_logic_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    add_test(NAME shell_context_menu_logic COMMAND awj_shell_context_menu_logic_tests)
    add_executable(awj_menu_config_journal_tests tests/menu_config_journal_tests.cpp)
    avif_enable_common_options(awj_menu_config_journal_tests)
    target_link_libraries(awj_menu_config_journal_tests PRIVATE awj_shell_context_menu)
    awj_set_runtime_directory(awj_menu_config_journal_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    add_test(NAME menu_config_journal COMMAND awj_menu_config_journal_tests)
    set_tests_properties(menu_config_journal PROPERTIES TIMEOUT 30)
    add_executable(awj_shell_machine_menu_tests tests/shell_machine_menu_tests.cpp)
    avif_enable_common_options(awj_shell_machine_menu_tests)
    target_link_libraries(awj_shell_machine_menu_tests PRIVATE awj_shell_context_menu)
    awj_set_runtime_directory(awj_shell_machine_menu_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    add_test(NAME shell_machine_snapshot_rejection COMMAND awj_shell_machine_menu_tests)

    add_executable(awj_shell_context_menu_registry_tests
        tests/shell_context_menu_registry_tests.cpp)
    avif_enable_common_options(awj_shell_context_menu_registry_tests)
    target_link_libraries(awj_shell_context_menu_registry_tests PRIVATE awj_shell_context_menu)
    add_dependencies(awj_shell_context_menu_registry_tests AWJ)
    awj_set_runtime_directory(awj_shell_context_menu_registry_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    add_test(NAME shell_context_menu_registry
        COMMAND awj_shell_context_menu_registry_tests "$<TARGET_FILE:AWJ>")
    set_tests_properties(shell_context_menu_registry PROPERTIES TIMEOUT 45)

    add_executable(awj_shell_context_menu_explorer_tests
        tests/shell_context_menu_explorer_tests.cpp)
    avif_enable_common_options(awj_shell_context_menu_explorer_tests)
    target_link_libraries(awj_shell_context_menu_explorer_tests PRIVATE
        awj_shell_context_menu
        awj_native_codecs
        ole32)
    add_dependencies(awj_shell_context_menu_explorer_tests AWJ)
    awj_set_runtime_directory(awj_shell_context_menu_explorer_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    add_test(NAME shell_context_menu_explorer
        COMMAND awj_shell_context_menu_explorer_tests "$<TARGET_FILE:AWJ>")
    set_tests_properties(shell_context_menu_registry shell_context_menu_explorer
        PROPERTIES RESOURCE_LOCK awj_shell_registry)
    set_tests_properties(shell_context_menu_explorer PROPERTIES TIMEOUT 1200)
endif()

if(BUILD_TESTING)
    if(WIN32)
        add_executable(awj_studio_state_tests
            tests/studio_state_tests.cpp
            src/ui/studio_config.cpp
            src/ui/studio_json.cpp
            src/ui/studio_queue_format.cpp
            src/ui/studio_queue_import.cpp
            src/ui/import_service.cpp
            src/ui/file_drop_win32.cpp)
        target_sources(awj_studio_state_tests PRIVATE
            FILE_SET studio_state_modules TYPE CXX_MODULES FILES src/ui/studio_defaults.ixx)
        avif_enable_common_options(awj_studio_state_tests)
        target_include_directories(awj_studio_state_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(awj_studio_state_tests PRIVATE awj_shared Slint::Slint psapi ole32 shell32)
        add_dependencies(awj_studio_state_tests AWJ)
        awj_set_runtime_directory(awj_studio_state_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/studio-state-contract")
        add_test(NAME studio_state_contract COMMAND awj_studio_state_tests)
        set_tests_properties(studio_state_contract PROPERTIES TIMEOUT 30)
        find_program(AWJ_PWSH_EXECUTABLE pwsh)
        if(AWJ_PWSH_EXECUTABLE)
            add_test(NAME package_manifest_contract COMMAND "${AWJ_PWSH_EXECUTABLE}"
                -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_manifest_tests.ps1")
            add_test(NAME release_safety_contract COMMAND "${AWJ_PWSH_EXECUTABLE}"
                -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_safety_tests.ps1")
            set_tests_properties(release_safety_contract PROPERTIES TIMEOUT 120)
            add_test(NAME release_layout_contract COMMAND "${AWJ_PWSH_EXECUTABLE}"
                -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_layout_tests.ps1")
            set_tests_properties(release_layout_contract PROPERTIES TIMEOUT 120)
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(awj_native_path_tests tests/native_path_tests.cpp)
        avif_enable_common_options(awj_native_path_tests)
        target_link_libraries(awj_native_path_tests PRIVATE awj_base)
        add_dependencies(awj_native_path_tests AWJ)
        add_test(NAME native_path_contract COMMAND awj_native_path_tests
            $<TARGET_FILE:AWJ> "${CMAKE_CURRENT_SOURCE_DIR}/tests/slint_runtime_images/pixel.jpg")
        add_executable(awj_linux_process_tests tests/linux_process_tests.cpp)
        avif_enable_common_options(awj_linux_process_tests)
        target_link_libraries(awj_linux_process_tests PRIVATE Threads::Threads)
        add_test(NAME linux_process COMMAND awj_linux_process_tests)
        set_tests_properties(linux_process PROPERTIES TIMEOUT 30)
    endif()
    add_executable(awj_queue_merge_tests tests/queue_merge_tests.cpp)
    avif_enable_common_options(awj_queue_merge_tests)
    add_test(NAME queue_merge_contract COMMAND awj_queue_merge_tests)
    set_tests_properties(queue_merge_contract PROPERTIES TIMEOUT 30)

    add_executable(awj_import_service_tests
        tests/import_service_tests.cpp
        src/ui/import_service.cpp)
    avif_enable_common_options(awj_import_service_tests)
    add_test(NAME import_service_core COMMAND awj_import_service_tests)

    if(WIN32)
        add_executable(awj_file_drop_win32_tests
            tests/file_drop_win32_tests.cpp
            src/ui/file_drop_win32.cpp)
        avif_enable_common_options(awj_file_drop_win32_tests)
        target_link_libraries(awj_file_drop_win32_tests PRIVATE ole32 shell32)
        add_test(NAME file_drop_win32_core COMMAND awj_file_drop_win32_tests)
    endif()

    # 字形像素回归必须使用真实 Winit/FemtoVG，不能由仅验证布局的 testing 后端代替。
    # 运行需要可用桌面和 OpenGL，因此只在显式启用时注册到 CTest。
    add_executable(awj_text_rendering_tests tests/text_rendering_tests.cpp)
    avif_enable_common_options(awj_text_rendering_tests)
    avif_add_slint_sources(awj_text_rendering_tests tests/text_rendering.slint)
    target_link_libraries(awj_text_rendering_tests PRIVATE Slint::Slint)
    if(TARGET slint_cpp)
        add_dependencies(awj_text_rendering_tests slint_cpp)
    endif()
    if(WIN32)
        target_sources(awj_text_rendering_tests PRIVATE src/ui/AWJ.manifest)
        target_link_libraries(awj_text_rendering_tests PRIVATE
            dwmapi gdi32 ole32 shell32 uxtheme imm32 opengl32)
    endif()
    awj_set_runtime_directory(awj_text_rendering_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    if(AWJ_ENABLE_NATIVE_RENDERER_TESTS)
        foreach(awj_text_scale 1 1.25 1.5 2)
            string(REPLACE "." "_" awj_text_scale_suffix "${awj_text_scale}")
            add_test(NAME text_rendering_pixels_${awj_text_scale_suffix}
                COMMAND awj_text_rendering_tests --scale "${awj_text_scale}")
            set_tests_properties(text_rendering_pixels_${awj_text_scale_suffix} PROPERTIES
                TIMEOUT 90 LABELS "ui;renderer;gpu" RESOURCE_LOCK awj_gpu_renderer
                ENVIRONMENT "SLINT_BACKEND=winit-femtovg")
        endforeach()
    endif()

    add_executable(awj_ui_smoke_tests
        tests/ui_smoke_tests.cpp)
    avif_enable_common_options(awj_ui_smoke_tests)
    avif_add_slint_sources(awj_ui_smoke_tests ui/awj_studio.slint
        DEBUG_INFO
        OUTPUT_SUFFIX _ui_smoke)
    avif_add_slint_sources(awj_ui_smoke_tests tests/focus_reentrancy.slint DEBUG_INFO CPP_NAMESPACE awj_focus_test)
    avif_add_slint_sources(awj_ui_smoke_tests tests/font_combo_hover.slint DEBUG_INFO CPP_NAMESPACE awj_font_hover_test)
    target_include_directories(awj_ui_smoke_tests PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(awj_ui_smoke_tests PRIVATE
        SLINT_FEATURE_EXPERIMENTAL=1
        AWJ_UI_VERSION="${PROJECT_VERSION}"
        AWJ_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(awj_ui_smoke_tests PRIVATE Slint::Slint)
    if(TARGET slint_cpp)
        add_dependencies(awj_ui_smoke_tests slint_cpp)
    endif()
    if(WIN32)
        target_sources(awj_ui_smoke_tests PRIVATE src/ui/AWJ.manifest)
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
    awj_set_runtime_directory(awj_ui_smoke_tests "${AWJ_RUNTIME_OUTPUT_ROOT}/tests")
    add_test(NAME slint_patch_contract COMMAND "${CMAKE_COMMAND}"
        "-DSLINT_SOURCE_DIR=${slint_SOURCE_DIR}"
        "-DAWJ_WINIT_SOURCE_DIR=${awj_winit_sources_SOURCE_DIR}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/slint-patch-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/slint_patch_tests.cmake")
    add_test(NAME winit_dpi_patch_contract COMMAND "${CMAKE_COMMAND}"
        "-DWINIT_SOURCE_ARCHIVE=${AWJ_WINIT_TEST_ARCHIVE}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/winit-dpi-patch-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/winit_dpi_patch_tests.cmake")
    add_test(NAME ui_memory_budget_unlock COMMAND awj_ui_smoke_tests --memory-budget-unlock)
    set_tests_properties(ui_memory_budget_unlock PROPERTIES TIMEOUT 120)
    add_test(NAME ui_smoke COMMAND awj_ui_smoke_tests)
    add_test(NAME ui_queue_stress COMMAND awj_ui_smoke_tests --queue-stress)
    set_tests_properties(ui_queue_stress PROPERTIES TIMEOUT 120)
    set_tests_properties(ui_smoke PROPERTIES TIMEOUT 120)
endif()

if(WIN32)
    add_executable(AWJ-com src/cli/shim_main.cpp src/cli/main.cpp src/ui/AWJ.rc)
    avif_enable_common_options(AWJ-com)
    target_compile_definitions(AWJ-com PRIVATE AWJ_UNIFIED_EXE=1)
    target_link_libraries(AWJ-com PRIVATE awj_core)
    set_target_properties(AWJ-com PROPERTIES
        OUTPUT_NAME "AWJ"
        SUFFIX ".com"
        PDB_NAME "AWJ-com")
    awj_add_sha256_post_build(AWJ-com)
    add_dependencies(AWJ AWJ-com)
    if(BUILD_TESTING)
        find_program(AWJ_PWSH NAMES pwsh REQUIRED)
        add_test(NAME cli_independence COMMAND "${AWJ_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_independence.ps1"
            -Cli $<TARGET_FILE:AWJ-com> -Gui $<TARGET_FILE:AWJ>
            -InputImage "${CMAKE_CURRENT_SOURCE_DIR}/tests/slint_runtime_images/pixel.jpg"
            -WorkDirectory "${CMAKE_CURRENT_BINARY_DIR}/cli-independence")
        set_tests_properties(cli_independence PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
endif()
