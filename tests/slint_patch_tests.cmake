# Run against the pinned git checkout; exercise pristine, repeated and broken inputs.
if(NOT DEFINED SLINT_SOURCE_DIR OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "SLINT_SOURCE_DIR and TEST_DIR are required")
endif()
find_package(Git REQUIRED)
set(files api/cpp/Cargo.toml api/cpp/include/private/slint_config.h internal/core/window.rs
    internal/backends/winit/winitwindowadapter.rs internal/backends/winit/event_loop.rs
    api/cpp/include/private/slint_models.h)
file(MAKE_DIRECTORY "${TEST_DIR}")
foreach(path IN LISTS files)
    get_filename_component(parent "${TEST_DIR}/${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SLINT_SOURCE_DIR}" show "HEAD:${path}"
        OUTPUT_FILE "${TEST_DIR}/${path}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
set(patches "${CMAKE_CURRENT_LIST_DIR}/../cmake/patch_slint_static_import.cmake")
foreach(patch IN LISTS patches)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${TEST_DIR}" -P "${patch}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
foreach(path IN LISTS files)
    file(SHA256 "${TEST_DIR}/${path}" hash)
    list(APPEND before "${hash}")
endforeach()
foreach(patch IN LISTS patches)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${TEST_DIR}" -P "${patch}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
foreach(path IN LISTS files)
    file(SHA256 "${TEST_DIR}/${path}" hash)
    list(APPEND after "${hash}")
endforeach()
if(NOT before STREQUAL after)
    message(FATAL_ERROR "Slint patches are not idempotent")
endif()
# An incompatible Winit source must fail closed, including on an otherwise patched tree.
file(WRITE "${TEST_DIR}/internal/backends/winit/event_loop.rs" "// incompatible source\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${TEST_DIR}" -P "${CMAKE_CURRENT_LIST_DIR}/../cmake/patch_slint_static_import.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT error MATCHES "patch anchor missing")
    message(FATAL_ERROR "Slint patch accepted a missing Winit anchor: ${output}${error}")
endif()
message(STATUS "Slint patches: pristine, idempotent, mismatched-anchor rejection passed")
