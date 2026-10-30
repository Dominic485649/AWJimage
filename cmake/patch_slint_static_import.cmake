# Slint 1.18.1 / 372cf0ee5577c3dfec309a45e7b778ba4e81b734 only.
# Optional AWJ_WINIT_SOURCE_DIR selects an already-patched, project-owned winit
# 0.30.13. Omit it for the existing Slint-only contract; never run cargo update.
if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "SOURCE_DIR is required.")
endif()
file(REAL_PATH "${SOURCE_DIR}" SOURCE_DIR)
set(_awj_files api/cpp/Cargo.toml api/cpp/include/private/slint_config.h internal/core/window.rs
    internal/backends/winit/winitwindowadapter.rs internal/backends/winit/event_loop.rs
    api/cpp/include/private/slint_models.h internal/renderers/femtovg/itemrenderer.rs)
set(_awj_hashes
    1fd9d8b2dd96e134e196f0c104e08d1d26bf06ce2dd615ae29d88797e2179911
    db1b1ef7ba9ddc870883772adb0f5ae9cffa2a8e14318a8de51f243ae18c1019
    6055b5e819a8e087a70b2310387033bb82ea49501b6b2c4f35d47da97658bea2
    29357860c3bc564c01f5b89ca0900e2cb091fd7590d8ba675cbee38f0178d61c
    07d94cfca3b5174082ff033a3f376a1b9f93ba2a6f8c8b5f628af55ad6c4bf79
    dbca9ee4a78953eddd37f3098691762f75b6912280c299efeabe47ab7f04a985
    30ee92fa6c8ce12c21fb7135f3696677131bb252d9ba34b7a5e0079687e3d82b)
foreach(_awj_file IN LISTS _awj_files)
    file(READ "${SOURCE_DIR}/${_awj_file}" _awj_text)
    string(REPLACE "\r\n" "\n" _awj_text "${_awj_text}")
    set("_awj_input_${_awj_file}" "${_awj_text}")
    set("_awj_base_${_awj_file}" "${_awj_text}")
    set("_awj_work_${_awj_file}" "${_awj_text}")
endforeach()

# Stage changes in memory. Reverse recognized replacements to verify the entire
# pristine file, not just a marker. No writes occur until every file passes.
function(awj_slint_replace relative before after)
    set(content "${_awj_work_${relative}}")
    string(FIND "${content}" "${after}" applied)
    if(NOT applied EQUAL -1)
        set(anchor "${after}")
        string(REPLACE "${after}" "${before}" base "${_awj_base_${relative}}")
        set("_awj_base_${relative}" "${base}" PARENT_SCOPE)
    else()
        set(anchor "${before}")
    endif()
    string(FIND "${content}" "${anchor}" position)
    string(REPLACE "${anchor}" "" stripped "${content}")
    string(LENGTH "${content}" length)
    string(LENGTH "${stripped}" stripped_length)
    string(LENGTH "${anchor}" anchor_length)
    math(EXPR removed "${length} - ${stripped_length}")
    if(position EQUAL -1 OR NOT removed EQUAL anchor_length)
        message(FATAL_ERROR "Slint 1.18.1 patch anchor missing or duplicated in ${relative}")
    endif()
    string(REPLACE "${anchor}" "${after}" content "${content}")
    set("_awj_work_${relative}" "${content}" PARENT_SCOPE)
endfunction()

awj_slint_replace(api/cpp/include/private/slint_config.h
    "#    if defined(_MSC_VER)"
    "#    if defined(SLINT_STATIC)\n#        define SLINT_DLL_IMPORT\n#    elif defined(_MSC_VER)")

# Release the RefCell borrow before callbacks that can synchronously change focus.
awj_slint_replace(internal/core/window.rs
    "        if let Some(focus_item) = self.focus_item.borrow().upgrade() {\n            focus_item.borrow().as_ref().focus_event(&event, &self.window_adapter(), &focus_item);"
    "        // AWJ_SLINT_FOCUS_REENTRANCY_FIX\n        let focus_item = self.focus_item.borrow().upgrade();\n        if let Some(focus_item) = focus_item {\n            focus_item.borrow().as_ref().focus_event(&event, &self.window_adapter(), &focus_item);")

# Winit still reports one DroppedFile per path. Batch per window and event turn;
# preserve PathBuf bytes, including non-UTF-8 Unix paths and embedded newlines.
set(adapter internal/backends/winit/winitwindowadapter.rs)
awj_slint_replace(${adapter}
    "    cursor_pos: Cell<LogicalPoint>,"
    "    cursor_pos: Cell<LogicalPoint>,\n    pending_native_file_drops: RefCell<Vec<std::path::PathBuf>>,")
awj_slint_replace(${adapter}
    "            cursor_pos: Default::default(),"
    "            cursor_pos: Default::default(),\n            pending_native_file_drops: Default::default(),")
# External Windows resizers can change the native client area before queued
# resize events reach Slint. Render the current size and present each resize
# without waiting for the animation frame timer.
awj_slint_replace(${adapter}
    "            if self.pending_resize_event_after_show.take() {"
    [=[            // AWJ_WINDOWS_RESIZE_SYNC
            if self.pending_resize_event_after_show.take()
                || (cfg!(target_os = "windows")
                    && physical_size_to_slint(&winit_window.surface_size()) != self.size.get())
            {]=])
awj_slint_replace(${adapter}
    "                resized?;\n            }\n            WinitWindowEvent::CloseRequested => {"
    [=[                resized?;
                #[cfg(target_os = "windows")]
                if size.width > 0 && size.height > 0 {
                    self.draw()?; // AWJ_WINDOWS_RESIZE_SYNC
                }
            }
            WinitWindowEvent::CloseRequested => {]=])
awj_slint_replace(${adapter}
    "            WinitWindowEvent::CursorMoved { position, .. } => {"
    "            WinitWindowEvent::DroppedFile(path) => {\n                self.pending_native_file_drops.borrow_mut().push(path);\n            }\n            WinitWindowEvent::CursorMoved { position, .. } => {")
awj_slint_replace(${adapter}
    "    fn dispatch_internal_event(&self, event: impl Into<corelib::platform::InternalEvent>) {"
    [=[    // AWJ_NATIVE_FILE_DND_V3: no RefCell borrow may cross a UI callback.
    pub(crate) fn flush_pending_native_file_drops(&self) {
        let paths = std::mem::take(&mut *self.pending_native_file_drops.borrow_mut());
        if paths.is_empty() { return; }
        let mut data = corelib::DataTransfer::default();
        data.set_file_paths(paths);
        let position = self.cursor_pos.get();
        let mut event = corelib::items::DropEvent::default();
        event.data = data;
        event.position = corelib::api::LogicalPosition::new(position.x, position.y);
        event.proposed_action = corelib::items::DragAction::Copy;
        let allowed = corelib::items::AllowedDragActions { copy: true, move_: false, link: false };
        let runtime_window = WindowInner::from_pub(self.window());
        runtime_window.process_drag_event(corelib::input::BackendDragEvent::Move { event: event.clone(), allowed });
        runtime_window.process_drag_event(corelib::input::BackendDragEvent::Drop { event, allowed });
    }

    fn dispatch_internal_event(&self, event: impl Into<corelib::platform::InternalEvent>) {]=])
awj_slint_replace(internal/backends/winit/event_loop.rs
    [=[    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        self.shared_backend_data.flush_pending_mouse_move();]=]
    [=[    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        self.shared_backend_data.flush_pending_mouse_move();
        // AWJ_NATIVE_FILE_DND_V3: release the window map before dispatching callbacks.
        let windows = self.shared_backend_data.active_windows.borrow().values()
            .filter_map(|window| window.upgrade()).collect::<Vec<_>>();
        for window in windows { window.flush_pending_native_file_drops(); }]=])

# FilterModel still fails to shift rejected insertions in 1.18.1. Mapping changes
# must notify consumers that expose source row indices, even when no row matches.
set(models api/cpp/include/private/slint_models.h)
awj_slint_replace(${models}
    [=[        if (added_accepted_rows.empty()) {
            return;
        }

        auto insertion_point = std::lower_bound(accepted_rows.begin(), accepted_rows.end(), index);]=]
    [=[        // AWJ_FILTER_INDEX_FIX: rejected insertions still shift source indices.
        auto insertion_point = std::lower_bound(accepted_rows.begin(), accepted_rows.end(), index);
        if (added_accepted_rows.empty()) {
            for (auto it = insertion_point; it != accepted_rows.end(); ++it) *it += count;
            target_model.notify_reset(); // AWJ_FILTER_MAPPING_NOTIFY
            return;
        }]=])
awj_slint_replace(${models}
    "(mapped_row_start != accepted_rows.end() && *mapped_row_start == index)"
    "(mapped_removed_len > 0)")
awj_slint_replace(${models}
    [=[        if (mapped_removed_index) {
            target_model.notify_row_removed(*mapped_removed_index, mapped_removed_len);
        }]=]
    [=[        // Source indices also change when only rejected rows are removed.
        target_model.notify_reset();]=])

# 圆角裁剪的离屏纹理也需对齐物理像素，避免分数 DPI 下文字再次被线性采样。
# 裁剪路径、纹理和覆盖区同步对齐，不能只平移纹理而额外截掉贴边内容。
awj_slint_replace(internal/renderers/femtovg/itemrenderer.rs
    [=[                    canvas.intersect_scissor(
                        layer_origin.x,
                        layer_origin.y,
                        layer_image_width,
                        layer_image_height,
                    );
                    canvas.fill_path(&layer_path, &layer_image_paint);]=]
    [=[                    // AWJ_ROUNDED_CLIP_PIXEL_ALIGNMENT
                    // 对齐后的圆角仍受原矩形约束，不能越出父图层缓存的逻辑边界。
                    let clip_bounds = clip_rect * self.scale_factor;
                    canvas.intersect_scissor(
                        clip_bounds.origin.x,
                        clip_bounds.origin.y,
                        clip_bounds.size.width,
                        clip_bounds.size.height,
                    );
                    let restore_transform = Self::pixel_align_origin(canvas, layer_origin);
                    canvas.intersect_scissor(
                        layer_origin.x,
                        layer_origin.y,
                        layer_image_width,
                        layer_image_height,
                    );
                    canvas.fill_path(&layer_path, &layer_image_paint);
                    if restore_transform {
                        canvas.restore();
                    }]=])

# Runtime PNG/JPEG remain enabled by workspace image features; SVG stays explicit.
# The host slint-compiler retains image-default-formats for build-time resources.
awj_slint_replace(api/cpp/Cargo.toml
    [=[  "i-slint-core/image-default-formats",]=]
    [=[  # AWJ_RUNTIME_IMAGES: keep PNG/JPEG/SVG, omit other runtime decoders.]=])

# Slint #13568 / merge 08a297b7151e52b98b500b4424a8dee770fc7b58.
# Both software-renderer dirty-region paths already round_out() in 1.18.1.
# Read the Win32 invalid region before winit validates it, not the whole window.
set(_awj_dirty_call_before "        self.pending_redraw.set(false);\n\n        if let Some(winit_window)")
set(_awj_dirty_call_after "        self.pending_redraw.set(false);\n\n        #[cfg(target_os = \"windows\")]\n        self.mark_windows_update_region_dirty();\n\n        if let Some(winit_window)")
awj_slint_replace(${adapter} "${_awj_dirty_call_before}" "${_awj_dirty_call_after}")
set(_awj_dirty_method_before "    pub fn window_state_event(&self) {")
set(_awj_dirty_method_after [=[    /// Windows invalidates the surface on DPI changes, independently of buffer age.
    /// winit delivers the redraw before validating this region (Slint #13568).
    #[cfg(target_os = "windows")]
    fn mark_windows_update_region_dirty(&self) {
        use raw_window_handle::{HasWindowHandle, RawWindowHandle};
        use windows::Win32::Foundation::{HWND, RECT};
        use windows::Win32::Graphics::Gdi::GetUpdateRect;

        let Some(winit_window) = self.winit_window_or_none.borrow().as_window() else { return };
        let Ok(window_handle) = winit_window.window_handle() else { return };
        let RawWindowHandle::Win32(win32_handle) = window_handle.as_raw() else { return };
        let hwnd = HWND(win32_handle.hwnd.get() as *mut core::ffi::c_void);

        let mut update_rect = RECT::default();
        if !unsafe { GetUpdateRect(hwnd, Some(&mut update_rect), false) }.as_bool() {
            return;
        }

        let physical_rect = euclid::Box2D::<i32, PhysicalPx>::new(
            euclid::point2(update_rect.left, update_rect.top),
            euclid::point2(update_rect.right, update_rect.bottom),
        )
        .to_rect()
        .cast::<Coord>();
        let logical_rect: LogicalRect =
            physical_rect / ScaleFactor::new(self.window().scale_factor());
        self.renderer().as_core_renderer().mark_dirty_region(logical_rect.into());
    }

    pub fn window_state_event(&self) {]=])
awj_slint_replace(${adapter} "${_awj_dirty_method_before}" "${_awj_dirty_method_after}")

set(_awj_all_pristine TRUE)
set(_awj_all_legacy TRUE)
set(_awj_all_patched TRUE)
foreach(_awj_file _awj_hash IN ZIP_LISTS _awj_files _awj_hashes)
    string(SHA256 _awj_actual "${_awj_base_${_awj_file}}")
    if(NOT _awj_actual STREQUAL _awj_hash)
        message(FATAL_ERROR "Slint 1.18.1 original-source SHA256 mismatch in ${_awj_file}: ${_awj_actual}")
    endif()
    set(_awj_legacy "${_awj_work_${_awj_file}}")
    if(_awj_file STREQUAL adapter)
        string(REPLACE "${_awj_dirty_call_after}" "${_awj_dirty_call_before}" _awj_legacy "${_awj_legacy}")
        string(REPLACE "${_awj_dirty_method_after}" "${_awj_dirty_method_before}" _awj_legacy "${_awj_legacy}")
    endif()
    if(NOT "${_awj_input_${_awj_file}}" STREQUAL "${_awj_base_${_awj_file}}")
        set(_awj_all_pristine FALSE)
    endif()
    if(NOT "${_awj_input_${_awj_file}}" STREQUAL "${_awj_legacy}")
        set(_awj_all_legacy FALSE)
    endif()
    if(NOT "${_awj_input_${_awj_file}}" STREQUAL "${_awj_work_${_awj_file}}")
        set(_awj_all_patched FALSE)
    endif()
endforeach()
if(NOT _awj_all_pristine AND NOT _awj_all_legacy AND NOT _awj_all_patched)
    message(FATAL_ERROR "Slint patch: partially patched source tree refused")
endif()

if(DEFINED AWJ_WINIT_SOURCE_DIR AND NOT AWJ_WINIT_SOURCE_DIR STREQUAL "")
    if(NOT IS_ABSOLUTE "${AWJ_WINIT_SOURCE_DIR}" OR NOT IS_DIRECTORY "${AWJ_WINIT_SOURCE_DIR}")
        message(FATAL_ERROR "AWJ_WINIT_SOURCE_DIR must be an absolute project-owned source directory")
    endif()
    file(REAL_PATH "${AWJ_WINIT_SOURCE_DIR}" _awj_winit_source)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${_awj_winit_source}"
        -DAWJ_WINIT_VERIFY_ONLY=ON -P "${CMAKE_CURRENT_LIST_DIR}/patch_winit_dpi.cmake"
        RESULT_VARIABLE _awj_result OUTPUT_VARIABLE _awj_out ERROR_VARIABLE _awj_err)
    if(NOT _awj_result EQUAL 0)
        message(FATAL_ERROR "Slint winit override rejected: ${_awj_out}${_awj_err}")
    endif()
    file(RELATIVE_PATH _awj_relative "${SOURCE_DIR}" "${_awj_winit_source}")
    if(IS_ABSOLUTE "${_awj_relative}" OR _awj_relative MATCHES "[:;\r\n\t]")
        message(FATAL_ERROR "winit override requires a portable relative path on the same volume")
    endif()
    # TOML basic strings escape quotes; CMake emits forward-slash relative paths.
    string(REPLACE "\"" "\\\"" _awj_toml_path "${_awj_relative}")
    set(_awj_override "\n# AWJ_WINIT_DPI_OVERRIDE: winit 0.30.13 / PR #4341.\n[patch.crates-io]\nwinit = { path = \"${_awj_toml_path}\" }\n")
    foreach(_awj_file IN ITEMS Cargo.toml Cargo.lock)
        file(READ "${SOURCE_DIR}/${_awj_file}" _awj_text)
        string(REPLACE "\r\n" "\n" _awj_text "${_awj_text}")
        set("_awj_input_${_awj_file}" "${_awj_text}")
    endforeach()
    string(FIND "${_awj_input_Cargo.toml}" "# AWJ_WINIT_DPI_OVERRIDE:" _awj_marker)
    if(_awj_marker EQUAL -1)
        set(_awj_manifest_base "${_awj_input_Cargo.toml}")
        set(_awj_manifest_patched FALSE)
    else()
        string(FIND "${_awj_input_Cargo.toml}" "${_awj_override}" _awj_position)
        if(_awj_position EQUAL -1)
            message(FATAL_ERROR "Slint winit override path/state mismatch; restore the pinned manifests before changing the vendor layout")
        endif()
        string(REPLACE "${_awj_override}" "" _awj_manifest_base "${_awj_input_Cargo.toml}")
        set(_awj_manifest_patched TRUE)
    endif()
    string(SHA256 _awj_actual "${_awj_manifest_base}")
    if(NOT _awj_actual STREQUAL "4d310173b3d954f72731f4635a3c1c570d351295bb25fb364e496e5dd3e44242")
        message(FATAL_ERROR "Slint Cargo.toml original-source SHA256 mismatch: ${_awj_actual}")
    endif()
    set(_awj_lock_before [=[[[package]]
name = "winit"
version = "0.30.13"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "a6755fa58a9f8350bd1e472d4c3fcc25f824ec358933bba33306d0b63df5978d"
dependencies = []=])
    set(_awj_lock_after [=[[[package]]
name = "winit"
version = "0.30.13"
dependencies = []=])
    string(FIND "${_awj_input_Cargo.lock}" "${_awj_lock_after}" _awj_position)
    if(_awj_position EQUAL -1)
        set(_awj_lock_base "${_awj_input_Cargo.lock}")
        set(_awj_lock_patched FALSE)
    else()
        string(REPLACE "${_awj_lock_after}" "${_awj_lock_before}" _awj_lock_base "${_awj_input_Cargo.lock}")
        set(_awj_lock_patched TRUE)
    endif()
    string(SHA256 _awj_actual "${_awj_lock_base}")
    if(NOT _awj_actual STREQUAL "4ef25a0b68e724259e574fe5ba4ec19c71dbe15a85cb3d304f6c376b47f2aa7a")
        message(FATAL_ERROR "Slint Cargo.lock original-source SHA256 mismatch: ${_awj_actual}")
    endif()
    if(NOT _awj_manifest_patched STREQUAL _awj_lock_patched)
        message(FATAL_ERROR "Slint winit override: partially patched manifest/lock refused")
    endif()
    set(_awj_work_Cargo.toml "${_awj_manifest_base}${_awj_override}")
    string(REPLACE "${_awj_lock_before}" "${_awj_lock_after}" _awj_work_Cargo.lock "${_awj_lock_base}")
    list(APPEND _awj_files Cargo.toml Cargo.lock)
elseif(EXISTS "${SOURCE_DIR}/Cargo.toml")
    file(READ "${SOURCE_DIR}/Cargo.toml" _awj_manifest)
    if(_awj_manifest MATCHES "AWJ_WINIT_DPI_OVERRIDE")
        message(FATAL_ERROR "Existing winit override requires AWJ_WINIT_SOURCE_DIR for path verification")
    endif()
endif()

foreach(_awj_file IN LISTS _awj_files)
    if(NOT "${_awj_input_${_awj_file}}" STREQUAL "${_awj_work_${_awj_file}}")
        file(WRITE "${SOURCE_DIR}/${_awj_file}" "${_awj_work_${_awj_file}}")
    endif()
endforeach()
message(STATUS "Slint 1.18.1 patches: original hashes and complete patch state verified")
