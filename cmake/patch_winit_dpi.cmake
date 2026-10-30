# winit 0.30.13, crates.io SHA256:
# a6755fa58a9f8350bd1e472d4c3fcc25f824ec358933bba33306d0b63df5978d
# Backport of PR #4341, merge 488c036a05d418e13bbdcdc349e2db2f6b7f58e2.
# Unlike upstream, retain the final InnerSizeWriter size and restore-size policy.
# SOURCE_DIR must be a project-owned extraction, never the shared Cargo registry.
# AWJ_WINIT_VERIFY_ONLY checks an already-patched tree without writing anything.
if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "winit DPI patch: SOURCE_DIR is required")
endif()
file(REAL_PATH "${SOURCE_DIR}" SOURCE_DIR)
if(SOURCE_DIR MATCHES "/registry/(src|cache)/")
    message(FATAL_ERROR "winit DPI patch: shared Cargo registry sources are forbidden")
endif()

set(_files Cargo.toml src/platform_impl/windows/dark_mode.rs src/platform_impl/windows/event_loop.rs)
set(_hashes
    2aeb741d47b763ee198ca915c330a6bf0b1fc094dd69357e80278b7852289c61
    9fa225a2462e0a0467b7608fc126515f5138d7762f0f86688b17ef9d627f0fe1
    f99c748e189ea8ce4bde08ec186ee27306756ce93d5235371e198c07e582c52d)
foreach(_file IN LISTS _files)
    file(READ "${SOURCE_DIR}/${_file}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    set("_input_${_file}" "${_text}")
    set("_base_${_file}" "${_text}")
    set("_work_${_file}" "${_text}")
endforeach()

# Stage every replacement, then validate the complete original-file hashes.
# Reject partial or unrecognized patches before writing either source file.
function(awj_winit_replace relative before after)
    set(text "${_work_${relative}}")
    string(FIND "${text}" "${after}" found)
    if(NOT found EQUAL -1)
        set(anchor "${after}")
        string(REPLACE "${after}" "${before}" base "${_base_${relative}}")
        set("_base_${relative}" "${base}" PARENT_SCOPE)
    else()
        set(anchor "${before}")
    endif()
    string(FIND "${text}" "${anchor}" found)
    string(REPLACE "${anchor}" "" stripped "${text}")
    string(LENGTH "${text}" length)
    string(LENGTH "${stripped}" stripped_length)
    string(LENGTH "${anchor}" anchor_length)
    math(EXPR removed "${length} - ${stripped_length}")
    if(found EQUAL -1 OR NOT removed EQUAL anchor_length)
        message(FATAL_ERROR "winit DPI patch anchor missing or duplicated in ${relative}")
    endif()
    string(REPLACE "${anchor}" "${after}" text "${text}")
    set("_work_${relative}" "${text}" PARENT_SCOPE)
endfunction()

set(_dark src/platform_impl/windows/dark_mode.rs)
awj_winit_replace(${_dark}
    "static WIN10_BUILD_VERSION: Lazy<Option<u32>>"
    "pub(super) static WIN10_BUILD_VERSION: Lazy<Option<u32>>")
awj_winit_replace(${_dark}
    "                dwOSVersionInfoSize: 0,"
    "                dwOSVersionInfoSize: std::mem::size_of::<OSVERSIONINFOW>() as u32,")

set(_event src/platform_impl/windows/event_loop.rs)
awj_winit_replace(${_event}
    [=[            let new_outer_rect: RECT;
            {
                let suggested_ul =]=]
    [=[            let new_outer_rect: RECT;
            // AWJ_WIN11_DPI_POSITION: winit #4341.
            if super::dark_mode::WIN10_BUILD_VERSION.is_some_and(|build| build >= 22000) {
                // Windows 11 supplies the destination position. Keep the chosen client size,
                // including InnerSizeWriter requests and the fullscreen/maximized restore policy.
                let requested_inner_rect = RECT {
                    left: 0,
                    top: 0,
                    right: new_physical_inner_size.width as i32,
                    bottom: new_physical_inner_size.height as i32,
                };
                let requested_outer_rect = window_flags
                    .adjust_rect(window, requested_inner_rect)
                    .unwrap_or(requested_inner_rect);
                new_outer_rect = RECT {
                    left: suggested_rect.left,
                    top: suggested_rect.top,
                    right: suggested_rect.left
                        + (requested_outer_rect.right - requested_outer_rect.left),
                    bottom: suggested_rect.top
                        + (requested_outer_rect.bottom - requested_outer_rect.top),
                };
            } else {
                let suggested_ul =]=])

set(_all_pristine TRUE)
set(_all_patched TRUE)
foreach(_file _hash IN ZIP_LISTS _files _hashes)
    string(SHA256 actual "${_base_${_file}}")
    if(NOT actual STREQUAL _hash)
        message(FATAL_ERROR "winit 0.30.13 original-source SHA256 mismatch in ${_file}: ${actual}")
    endif()
    if(NOT "${_input_${_file}}" STREQUAL "${_base_${_file}}")
        set(_all_pristine FALSE)
    endif()
    if(NOT "${_input_${_file}}" STREQUAL "${_work_${_file}}")
        set(_all_patched FALSE)
    endif()
endforeach()
if(NOT _all_pristine AND NOT _all_patched)
    message(FATAL_ERROR "winit DPI patch: partially patched source tree refused")
endif()
if(AWJ_WINIT_VERIFY_ONLY)
    if(NOT _all_patched)
        message(FATAL_ERROR "winit DPI patch: verification requires an already-patched source tree")
    endif()
else()
    foreach(_file IN LISTS _files)
        if(NOT "${_input_${_file}}" STREQUAL "${_work_${_file}}")
            file(WRITE "${SOURCE_DIR}/${_file}" "${_work_${_file}}")
        endif()
    endforeach()
endif()
message(STATUS "winit 0.30.13 DPI patch: original hashes and complete patch state verified")
