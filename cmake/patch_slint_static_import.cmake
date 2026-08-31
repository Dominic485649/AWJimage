if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required.")
endif()

set(CONFIG_H "${SOURCE_DIR}/api/cpp/include/private/slint_config.h")
if(NOT EXISTS "${CONFIG_H}")
    message(FATAL_ERROR "Slint config header not found: ${CONFIG_H}")
endif()

file(READ "${CONFIG_H}" CONTENT)
set(NEEDLE "#if !defined(DOXYGEN)\n#    if defined(_MSC_VER)")
set(REPLACEMENT "#if !defined(DOXYGEN)\n#    if defined(SLINT_STATIC)\n#        define SLINT_DLL_IMPORT\n#    elif defined(_MSC_VER)")

if(NOT CONTENT MATCHES "defined\\(SLINT_STATIC\\)")
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" PATCHED "${CONTENT}")
    if(PATCHED STREQUAL CONTENT)
        message(FATAL_ERROR "Could not patch Slint static import declarations.")
    endif()
    file(WRITE "${CONFIG_H}" "${PATCHED}")
endif()

file(TOUCH "${SOURCE_DIR}/api/cpp/build.rs")

# Slint 1.17 exposes DropArea/DataTransfer, while its Winit backend currently
# leaves Winit's native DroppedFile events untranslated.  AWJ needs the public
# DropArea path on both desktop platforms, not a Win32 WM_DROPFILES shim.
set(WINIT_EVENT_LOOP "${SOURCE_DIR}/internal/backends/winit/event_loop.rs")
if(NOT EXISTS "${WINIT_EVENT_LOOP}")
    message(FATAL_ERROR "Slint Winit event loop source not found.")
endif()
file(READ "${WINIT_EVENT_LOOP}" WINIT_CONTENT)
if(NOT WINIT_CONTENT MATCHES "AWJ_NATIVE_FILE_DND_V2")
    string(REPLACE
        "use corelib::SharedString;\nuse corelib::graphics::euclid;\nuse corelib::input::{InternalKeyEvent, KeyEvent, KeyEventType, MouseEvent, TouchPhase};\nuse corelib::items::{ColorScheme, PointerEventButton};"
        "use corelib::SharedString;\nuse corelib::DataTransfer;\nuse corelib::graphics::euclid;\nuse corelib::input::{InternalKeyEvent, KeyEvent, KeyEventType, MouseEvent, TouchPhase};\nuse corelib::items::{AllowedDragActions, ColorScheme, DragAction, DropEvent, PointerEventButton};"
        WINIT_PATCHED "${WINIT_CONTENT}")
    if(WINIT_PATCHED STREQUAL WINIT_CONTENT)
        message(FATAL_ERROR "Could not add Slint Winit native-drop imports.")
    endif()
    string(REPLACE
        "    pending_mouse_move: Option<(winit::window::WindowId, LogicalPoint)>,"
        "    pending_mouse_move: Option<(winit::window::WindowId, LogicalPoint)>,\n\n    // AWJ_NATIVE_FILE_DND_V2: batch paths reported in one native Winit turn.\n    pending_native_file_drops: Vec<(winit::window::WindowId, std::path::PathBuf)>,"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "            pending_mouse_move: Default::default(),"
        "            pending_mouse_move: Default::default(),\n            pending_native_file_drops: Default::default(),"
        WINIT_PATCHED "${WINIT_PATCHED}")
    set(WINIT_FLUSH_NATIVE "\n    fn flush_pending_native_file_drops(&mut self) {\n        let pending = std::mem::take(&mut self.pending_native_file_drops);\n        let mut grouped: Vec<(winit::window::WindowId, String)> = Vec::new();\n        for (window_id, path) in pending {\n            if let Some((_, paths)) = grouped.iter_mut().find(|(id, _)| *id == window_id) {\n                if !paths.is_empty() { paths.push('\\n'); }\n                paths.push_str(&path.to_string_lossy());\n            } else {\n                grouped.push((window_id, path.to_string_lossy().into_owned()));\n            }\n        }\n        for (window_id, paths) in grouped {\n            let Some(window) = self.shared_backend_data.window_by_id(window_id) else { continue; };\n            let runtime_window = WindowInner::from_pub(window.window());\n            let mut data = DataTransfer::default();\n            data.set_plain_text(paths.into());\n            let mut event = DropEvent::default();\n            event.data = data;\n            event.position = corelib::api::LogicalPosition::new(self.cursor_pos.x, self.cursor_pos.y);\n            event.proposed_action = DragAction::Copy;\n            let allowed = AllowedDragActions { copy: true, move_: false, link: false };\n            runtime_window.process_mouse_input(MouseEvent::DragMove { event: event.clone(), allowed });\n            runtime_window.process_mouse_input(MouseEvent::Drop { event, allowed });\n        }\n    }\n")
    string(REPLACE
        "    }\n}\n\nimpl winit::application::ApplicationHandler<SlintEvent> for EventLoopState {"
        "    }${WINIT_FLUSH_NATIVE}}\n\nimpl winit::application::ApplicationHandler<SlintEvent> for EventLoopState {"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "            WindowEvent::CursorMoved { position, .. } => {"
        "            WindowEvent::DroppedFile(path) => {\n                self.pending_native_file_drops.push((window_id, path));\n            }\n            WindowEvent::CursorMoved { position, .. } => {"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {\n        self.flush_pending_mouse_move();"
        "    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {\n        self.flush_pending_native_file_drops();\n        self.flush_pending_mouse_move();"
        WINIT_PATCHED "${WINIT_PATCHED}")
    if(WINIT_PATCHED STREQUAL WINIT_CONTENT)
        message(FATAL_ERROR "Could not add Slint Winit native-drop event handling.")
    endif()
    file(WRITE "${WINIT_EVENT_LOOP}" "${WINIT_PATCHED}")
endif()

# Slint 1.17.1 can schedule an AccessKit tree rebuild while a window adapter is
# still alive but its component has already been destroyed. Backport upstream
# slint-ui/slint#12938 so that teardown produces an empty accessibility update
# instead of calling WindowInner::component() and aborting on its unwrap().
set(WINIT_ACCESSKIT "${SOURCE_DIR}/internal/backends/winit/accesskit.rs")
if(NOT EXISTS "${WINIT_ACCESSKIT}")
    message(FATAL_ERROR "Slint Winit AccessKit source not found.")
endif()
file(READ "${WINIT_ACCESSKIT}" WINIT_ACCESSKIT_CONTENT)
if(NOT WINIT_ACCESSKIT_CONTENT MATCHES "AWJ_SLINT_ACCESSKIT_COMPONENT_LIFETIME_FIX")
    set(WINIT_ACCESSKIT_NEEDLE
        "        let root_item = ItemRc::new_root(window_inner.component());")
    set(WINIT_ACCESSKIT_REPLACEMENT
        "        // AWJ_SLINT_ACCESSKIT_COMPONENT_LIFETIME_FIX: backport slint-ui/slint#12938.\n        let Some(component) = window_inner.try_component() else {\n            return TreeUpdate {\n                nodes: Default::default(),\n                tree: Default::default(),\n                tree_id: TreeId::ROOT,\n                focus: self.root_node_id,\n            };\n        };\n        let root_item = ItemRc::new_root(component);")
    string(REPLACE
        "${WINIT_ACCESSKIT_NEEDLE}"
        "${WINIT_ACCESSKIT_REPLACEMENT}"
        WINIT_ACCESSKIT_PATCHED "${WINIT_ACCESSKIT_CONTENT}")
    if(WINIT_ACCESSKIT_PATCHED STREQUAL WINIT_ACCESSKIT_CONTENT)
        message(FATAL_ERROR "Could not backport Slint AccessKit component-lifetime fix.")
    endif()
    file(WRITE "${WINIT_ACCESSKIT}" "${WINIT_ACCESSKIT_PATCHED}")
endif()
