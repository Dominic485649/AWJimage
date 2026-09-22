# Patches for the pinned Slint 1.18.0 only. AccessKit #12938 is upstream.
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required.")
endif()

# Each replacement validates its exact before/after anchor, including on reruns.
# A source mismatch must stop configuration rather than produce a partial build.
function(awj_slint_replace relative before after)
    set(path "${SOURCE_DIR}/${relative}")
    file(READ "${path}" content)
    string(FIND "${content}" "${after}" applied)
    if(NOT applied EQUAL -1)
        return()
    endif()
    string(FIND "${content}" "${before}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Slint 1.18.0 patch anchor missing in ${relative}: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" content "${content}")
    file(WRITE "${path}" "${content}")
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

# FilterModel still fails to shift rejected insertions in 1.18.0. Mapping changes
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

# Runtime PNG/JPEG remain enabled by workspace image features; SVG stays explicit.
# The host slint-compiler retains image-default-formats for build-time resources.
awj_slint_replace(api/cpp/Cargo.toml
    [=[  "i-slint-core/image-default-formats",]=]
    [=[  # AWJ_RUNTIME_IMAGES: keep PNG/JPEG/SVG, omit other runtime decoders.]=])
