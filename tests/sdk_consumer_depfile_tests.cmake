cmake_minimum_required(VERSION 4.0)
# Real installed C99/C++11/C++17 cross-link regression, not a new SDK build,
# package, provenance proof or target execution. Never run the linked programs.
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Run this regression in the project's native WSL/Linux workspace")
endif()
if(NOT DEFINED SOURCE_ROOT)
    get_filename_component(SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
endif()
file(REAL_PATH "${SOURCE_ROOT}" repo)
get_filename_component(work "${repo}" DIRECTORY)
if(NOT "${work}/src" STREQUAL repo)
    message(FATAL_ERROR "SOURCE_ROOT must be the project's native WorkRoot/src")
endif()
include("${repo}/cmake/sdk-hmos-cross-build.cmake")
foreach(required IN ITEMS TEST_ROOT INSTALL_DIRECTORY TARGET_NATIVE_SDK_DIRECTORY TARGET_RESOURCE_DIRECTORY LLVM_DIRECTORY)
    if(NOT IS_ABSOLUTE "${${required}}" OR "${${required}}" MATCHES "^/mnt/[A-Za-z](/|$)")
        message(FATAL_ERROR "Missing native absolute regression input: ${required}")
    endif()
endforeach()
get_filename_component(parent "${TEST_ROOT}" DIRECTORY)
get_filename_component(leaf "${TEST_ROOT}" NAME)
if(NOT IS_DIRECTORY "${parent}" OR leaf STREQUAL "" OR leaf MATCHES "^[.][.]?$" OR EXISTS "${TEST_ROOT}")
    message(FATAL_ERROR "TEST_ROOT must be a fresh child of an existing project test/results directory")
endif()
file(REAL_PATH "${parent}" parent)
cmake_path(SET TEST_ROOT NORMALIZE "${parent}/${leaf}")
set(allowed FALSE)
foreach(relative IN ITEMS build/release-contract-tests results/release-1.3.5-prep-20261111)
    set(base "${work}/${relative}")
    cmake_path(IS_PREFIX base "${TEST_ROOT}" NORMALIZE inside)
    if(inside AND NOT TEST_ROOT STREQUAL base)
        set(allowed TRUE)
    endif()
endforeach()
if(NOT allowed)
    message(FATAL_ERROR "TEST_ROOT is outside the project's release regression roots")
endif()
if(NOT DEFINED PACKAGER_SCRIPT)
    set(PACKAGER_SCRIPT "${repo}/scripts/package-sdk-release.cmake")
endif()
foreach(path IN ITEMS "${repo}" "${INSTALL_DIRECTORY}" "${TARGET_NATIVE_SDK_DIRECTORY}"
        "${TARGET_RESOURCE_DIRECTORY}" "${LLVM_DIRECTORY}" "${PACKAGER_SCRIPT}" "${TEST_ROOT}")
    if(NOT path MATCHES "^/[A-Za-z0-9_./+-]+$")
        message(FATAL_ERROR "Regression paths must be unambiguous native paths: ${path}")
    endif()
endforeach()
awj_sdk_hmos_reject_output_overlap("${TEST_ROOT}" "${repo}" "${INSTALL_DIRECTORY}"
    "${TARGET_NATIVE_SDK_DIRECTORY}" "${TARGET_RESOURCE_DIRECTORY}" "${LLVM_DIRECTORY}")

# Extract both literal generated files from the packager under test. In WSL the
# caller can provide only the changed packager as an isolated regression input;
# no synchronization or modification of the native source checkout is required.
awj_sdk_read_file("${PACKAGER_SCRIPT}" 262144 packager_text)
string(REPLACE "\r\n" "\n" packager_text "${packager_text}")
function(extract_consumer_file name out)
    set(opening "file(WRITE \"\${output}/consumer-project/${name}\" [==[\n")
    string(FIND "${packager_text}" "${opening}" start)
    if(start LESS 0)
        message(FATAL_ERROR "Cannot extract the production generated consumer file: ${name}")
    endif()
    string(LENGTH "${opening}" length)
    math(EXPR start "${start} + ${length}")
    string(SUBSTRING "${packager_text}" ${start} -1 tail)
    string(FIND "${tail}" "\n]==])" end)
    string(FIND "${tail}" "${opening}" duplicate)
    if(end LESS 0 OR NOT duplicate LESS 0)
        message(FATAL_ERROR "Ambiguous production generated consumer file: ${name}")
    endif()
    string(SUBSTRING "${tail}" 0 ${end} content)
    set(${out} "${content}\n" PARENT_SCOPE)
endfunction()
file(MAKE_DIRECTORY "${TEST_ROOT}/consumer-project")
foreach(name IN ITEMS CMakeLists.txt profile-probe.cpp)
    extract_consumer_file("${name}" content)
    file(WRITE "${TEST_ROOT}/consumer-project/${name}" "${content}")
endforeach()

file(READ "${repo}/VERSION" version)
string(STRIP "${version}" version)
awj_sdk_select_profile("${version}" hmos-arm64-api26-clang23 Linux x86_64 profile)
awj_sdk_check_install("${INSTALL_DIRECTORY}" profile)
awj_sdk_hmos_check_installed_sources("${INSTALL_DIRECTORY}" "${repo}")
awj_sdk_compiler_identity(profile identity)
awj_sdk_find_llvm_tool(consumer_c clang 23)
awj_sdk_find_llvm_tool(readobj llvm-readobj 23)
set(toolchain "${repo}/cmake/toolchains/ohos-clang23.cmake")
file(REAL_PATH "${INSTALL_DIRECTORY}/${profile_library}" expected_library)
file(SHA256 "${expected_library}" library_before)

# Force the CMake 4.x linker-depfile default ON at entry. Only the production
# AWJ_HMOS_CROSS_BUILD branch may disable it before installed targets are added.
set(build "${TEST_ROOT}/build")
file(MAKE_DIRECTORY "${build}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}/consumer-project" -B "${build}" -G Ninja
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_LINK_DEPENDS_USE_LINKER=ON
    "-DCMAKE_PREFIX_PATH=${INSTALL_DIRECTORY}" "-DAWJimage_DIR=${INSTALL_DIRECTORY}/lib/cmake/AWJimage"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
    "-DAWJ_CONSUMER_SOURCE_DIRECTORY=${INSTALL_DIRECTORY}/share/AWJimage/examples/basic"
    -DAWJ_HMOS_PROFILE=ON -DAWJ_HMOS_CROSS_BUILD=ON -DAWJ_MODERN_PROFILE=ON
    "-DCMAKE_TOOLCHAIN_FILE=${toolchain}" "-DCMAKE_C_COMPILER=${consumer_c}"
    "-DCMAKE_CXX_COMPILER=${identity_compiler}" -DCMAKE_SKIP_RPATH=ON
    "-DAWJ_TRUSTED_CLANG=${identity_compiler}" "-DAWJ_CLANG_VERSION=${identity_compiler_version}"
    "-DAWJ_NATIVE_SDK_DIRECTORY=${TARGET_NATIVE_SDK_DIRECTORY}"
    "-DAWJ_HMOS_COMPILER_TARGET_PATTERN=${AWJ_SDK_HMOS_COMPILER_TARGET_PATTERN}"
    "-DAWJ_OHOS_NATIVE_ROOT=${TARGET_NATIVE_SDK_DIRECTORY}" "-DAWJ_OHOS_RESOURCE_DIR=${TARGET_RESOURCE_DIRECTORY}"
    "-DAWJ_OHOS_LLVM_ROOT=${LLVM_DIRECTORY}"
    OUTPUT_FILE "${build}/configure.log" ERROR_FILE "${build}/configure.log" RESULT_VARIABLE configured)
if(NOT configured EQUAL 0)
    message(FATAL_ERROR "Production generated consumer configure failed; see ${build}/configure.log")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel 2 --verbose
    OUTPUT_FILE "${build}/build.log" ERROR_FILE "${build}/build.log" RESULT_VARIABLE linked)
if(NOT linked EQUAL 0)
    message(FATAL_ERROR "Real installed consumer cross-link failed; see ${build}/build.log")
endif()
awj_sdk_read_file("${build}/consumer-paths.json" 65536 paths)
set(names installed_c_consumer installed_cxx_consumer installed_cxx17_consumer)
set(standards c99 c++11 c++17)
set(records "{}")
foreach(name standard IN ZIP_LISTS names standards)
    awj_sdk_json_get(path "${paths}" STRING "${name}" path)
    assert_under("${build}" "${path}")
    awj_sdk_hmos_check_program("${path}" sdk-consumer "${readobj}" "${build}/${name}-identity.txt")
    awj_sdk_json_expect("${paths}" STRING "${standard}" "${name}" standard)
    awj_sdk_json_get(imported "${paths}" STRING "${name}" sdk_library)
    file(REAL_PATH "${imported}" imported)
    file(SHA256 "${imported}" imported_hash)
    if(NOT imported STREQUAL expected_library OR NOT imported_hash STREQUAL library_before)
        message(FATAL_ERROR "Installed consumer imported a different SDK path or byte identity: ${name}")
    endif()
    foreach(field IN ITEMS link_map link_inputs)
        awj_sdk_json_get("${field}" "${paths}" STRING "${name}" "${field}")
        assert_under("${build}" "${${field}}")
        if(IS_SYMLINK "${${field}}")
            message(FATAL_ERROR "Consumer audit attachment must not be a symlink")
        endif()
        awj_sdk_read_file("${${field}}" 16777216 "${field}_text")
    endforeach()
    if(NOT link_inputs STREQUAL "${build}/${name}.d" OR NOT link_map STREQUAL "${build}/${name}.map")
        message(FATAL_ERROR "Production explicit consumer audit attachment paths changed")
    endif()
    awj_sdk_hmos_link_dependencies("${link_inputs_text}" "${path}" "${build}" inputs)
    set(cxx TRUE)
    if(standard STREQUAL "c99")
        set(cxx FALSE)
    endif()
    awj_sdk_hmos_check_link_inputs("${inputs}" "${expected_library}" "${TARGET_NATIVE_SDK_DIRECTORY}"
        "${TARGET_RESOURCE_DIRECTORY}" "${cxx}" sdk-consumer "${build}" "${INSTALL_DIRECTORY}")
    set(record "{\"standard\":\"${standard}\",\"sdk_library_sha256\":\"${imported_hash}\",\"execution\":\"not-run\"}")
    foreach(field IN ITEMS path link_map link_inputs)
        file(SHA256 "${${field}}" hash)
        string(JSON record SET "${record}" "${field}_sha256" "\"${hash}\"")
    endforeach()
    string(JSON records SET "${records}" "${name}" "${record}")
endforeach()
awj_sdk_read_file("${build}/build.log" 16777216 commands)
string(REGEX MATCHALL "--dependency-file=[^ \r\n]+" depfile_options "${commands}")
list(LENGTH depfile_options depfile_count)
if(NOT depfile_count EQUAL 3)
    message(FATAL_ERROR "Expected exactly three explicit LLD depfiles, not extra CMake depfile options")
endif()
file(SHA256 "${expected_library}" library_after)
if(NOT library_after STREQUAL library_before)
    message(FATAL_ERROR "Read-only regression SDK library changed during consumer linking")
endif()
set(result "{\"kind\":\"installed-consumer-depfile-regression\",\"regression_only\":true,\"sdk_build\":\"not-run\",\"new_build_proof\":\"not-issued\",\"packaging\":\"not-run\",\"target_execution\":\"not-run\",\"target_loader\":\"not-run\",\"cmake_version\":\"${CMAKE_VERSION}\",\"inputs_sha256\":{}}")
string(JSON result SET "${result}" consumers "${records}")
foreach(input IN ITEMS "${CMAKE_CURRENT_LIST_FILE}" "${PACKAGER_SCRIPT}" "${CMAKE_COMMAND}" "${identity_compiler}" "${readobj}"
        "${toolchain}" "${repo}/VERSION" "${repo}/cmake/sdk-release-profiles.cmake" "${repo}/cmake/sdk-hmos-cross-build.cmake"
        "${expected_library}" "${INSTALL_DIRECTORY}/include/awj/awj.h"
        "${INSTALL_DIRECTORY}/share/AWJimage/examples/basic/CMakeLists.txt"
        "${INSTALL_DIRECTORY}/share/AWJimage/examples/basic/basic.c"
        "${INSTALL_DIRECTORY}/share/AWJimage/examples/basic/basic.cpp"
        "${INSTALL_DIRECTORY}/share/AWJimage/examples/basic/basic_cpp17.cpp"
        "${TARGET_NATIVE_SDK_DIRECTORY}/oh-uni-package.json"
        "${TARGET_NATIVE_SDK_DIRECTORY}/llvm/include/libcxx-ohos/include/c++/v1/__config_site"
        "${TARGET_NATIVE_SDK_DIRECTORY}/llvm/lib/aarch64-linux-ohos/libc++_static.a"
        "${TARGET_NATIVE_SDK_DIRECTORY}/llvm/lib/aarch64-linux-ohos/libunwind.a"
        "${TEST_ROOT}/consumer-project/CMakeLists.txt" "${TEST_ROOT}/consumer-project/profile-probe.cpp"
        "${build}/configure.log" "${build}/build.log" "${build}/consumer-paths.json")
    file(SHA256 "${input}" hash)
    string(JSON result SET "${result}" inputs_sha256 "${input}" "\"${hash}\"")
endforeach()
file(WRITE "${TEST_ROOT}/verification-inputs.json" "${result}\n")
message(STATUS "Three real installed consumers cross-linked with preserved explicit depfiles and SDK/Native binding; no target execution or new SDK proof")
