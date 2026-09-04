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
# leaves Winit's native DroppedFile events untranslated. AWJ needs the public
# DropArea path on both desktop platforms, not a Win32 WM_DROPFILES shim.
set(WINIT_EVENT_LOOP "${SOURCE_DIR}/internal/backends/winit/event_loop.rs")
if(NOT EXISTS "${WINIT_EVENT_LOOP}")
    message(FATAL_ERROR "Slint Winit event loop source not found.")
endif()

# The native-drop position helper uses POINT on Windows. Keep the dependency
# explicit instead of relying on feature unification from another Slint crate.
set(WINIT_CARGO_TOML "${SOURCE_DIR}/internal/backends/winit/Cargo.toml")
if(NOT EXISTS "${WINIT_CARGO_TOML}")
    message(FATAL_ERROR "Slint Winit Cargo.toml not found.")
endif()
file(READ "${WINIT_CARGO_TOML}" WINIT_CARGO_CONTENT)
if(NOT WINIT_CARGO_CONTENT MATCHES "Win32_Foundation")
    string(REPLACE
        "windows = { workspace = true, features = [\"Win32_UI_WindowsAndMessaging\", \"Win32_UI_HiDpi\", \"Win32_Graphics_Gdi\", \"Win32_Graphics_Dwm\"] }"
        "windows = { workspace = true, features = [\"Win32_Foundation\", \"Win32_UI_WindowsAndMessaging\", \"Win32_UI_HiDpi\", \"Win32_Graphics_Gdi\", \"Win32_Graphics_Dwm\"] }"
        WINIT_CARGO_PATCHED "${WINIT_CARGO_CONTENT}")
    if(WINIT_CARGO_PATCHED STREQUAL WINIT_CARGO_CONTENT)
        message(FATAL_ERROR "Could not add the Slint Winit Windows foundation feature.")
    endif()
    file(WRITE "${WINIT_CARGO_TOML}" "${WINIT_CARGO_PATCHED}")
endif()

file(READ "${WINIT_EVENT_LOOP}" WINIT_CONTENT)
# V3 was briefly generated before native file drops retained their exact
# position. Upgrade those already-patched build trees before deciding whether a
# full patch is needed.
if(WINIT_CONTENT MATCHES "AWJ_NATIVE_FILE_DND_V3" AND
   NOT WINIT_CONTENT MATCHES "fallback_position = self.cursor_pos")
    set(WINIT_V3_OLD_DROP_EVENT
        "            WindowEvent::DroppedFile(path) => {\n                let fallback_position = self.cursor_pos;\n                let position = window\n                    .winit_window()\n                    .map(|winit_window| {\n                        native_file_drop_position(\n                            &winit_window,\n                            winit_window.scale_factor(),\n                            fallback_position,\n                        )\n                    })\n                    .unwrap_or(fallback_position);\n                self.cursor_pos = position;\n                self.pending_native_file_drops.push((window_id, path, position));\n            }")
    set(WINIT_V3_NEW_DROP_EVENT
        "            WindowEvent::DroppedFile(path) => {\n                let fallback_position = self.cursor_pos;\n                let position = window\n                    .winit_window()\n                    .map(|winit_window| {\n                        native_file_drop_position(\n                            &winit_window,\n                            runtime_window.scale_factor() as f64,\n                            fallback_position,\n                        )\n                    })\n                    .unwrap_or(fallback_position);\n                self.cursor_pos = position;\n                self.pending_native_file_drops.push((window_id, path, position));\n            }")
    string(REPLACE "${WINIT_V3_OLD_DROP_EVENT}" "${WINIT_V3_NEW_DROP_EVENT}"
        WINIT_PATCHED "${WINIT_CONTENT}")
    string(REPLACE
        "let logical_position = physical_position.to_logical(scale_factor);"
        "let logical_position = physical_position.to_logical::<corelib::Coord>(scale_factor);"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {\n        self.flush_pending_native_file_drops();\n        self.flush_pending_mouse_move();"
        "    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {\n        self.flush_pending_mouse_move();\n        self.flush_pending_native_file_drops();"
        WINIT_PATCHED "${WINIT_PATCHED}")
    if(WINIT_PATCHED STREQUAL WINIT_CONTENT)
        message(FATAL_ERROR "Could not upgrade the existing Slint native-drop patch.")
    endif()
    file(WRITE "${WINIT_EVENT_LOOP}" "${WINIT_PATCHED}")
    set(WINIT_CONTENT "${WINIT_PATCHED}")
endif()
if(WINIT_CONTENT MATCHES "AWJ_NATIVE_FILE_DND_V3")
    # Idempotent DPI migration: if the old scale source is present, replace it.
    # If absent, the tree is already migrated and the replace is a no-op.
    string(REPLACE
        "                            runtime_window.scale_factor() as f64,"
        "                            winit_window.scale_factor(),"
        WINIT_PATCHED "${WINIT_CONTENT}")
    if(NOT WINIT_PATCHED STREQUAL WINIT_CONTENT)
        file(WRITE "${WINIT_EVENT_LOOP}" "${WINIT_PATCHED}")
        set(WINIT_CONTENT "${WINIT_PATCHED}")
    endif()
endif()
if(NOT WINIT_CONTENT MATCHES "AWJ_NATIVE_FILE_DND_V3")
    set(WINIT_NATIVE_DROP_POSITION "\nfn native_file_drop_position(\n    window: &winit::window::Window,\n    scale_factor: f64,\n    fallback: LogicalPoint,\n) -> LogicalPoint {\n    #[cfg(target_os = \"windows\")]\n    {\n        use windows::Win32::Foundation::POINT;\n        use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;\n\n        let mut screen_position = POINT::default();\n        if unsafe { GetCursorPos(&mut screen_position) }.is_ok() {\n            if let Ok(client_origin) = window.inner_position() {\n                let physical_position = winit::dpi::PhysicalPosition::new(\n                    screen_position.x - client_origin.x,\n                    screen_position.y - client_origin.y,\n                );\n                let logical_position = physical_position.to_logical::<corelib::Coord>(scale_factor);\n                return euclid::point2(logical_position.x, logical_position.y);\n            }\n        }\n    }\n    #[cfg(not(target_os = \"windows\"))]\n    let _ = (window, scale_factor);\n    fallback\n}\n")
    set(WINIT_DROPPED_FILE_EVENT "            WindowEvent::DroppedFile(path) => {\n                let fallback_position = self.cursor_pos;\n                let position = window\n                    .winit_window()\n                    .map(|winit_window| {\n                        native_file_drop_position(\n                            &winit_window,\n                            winit_window.scale_factor(),\n                            fallback_position,\n                        )\n                    })\n                    .unwrap_or(fallback_position);\n                self.cursor_pos = position;\n                self.pending_native_file_drops.push((window_id, path, position));\n            }\n")

    # Support reconfiguration of an existing build tree that contains the
    # previous V2 patch, as well as pristine Slint sources in a fresh build.
    if(WINIT_CONTENT MATCHES "AWJ_NATIVE_FILE_DND_V2")
        set(WINIT_PATCHED "${WINIT_CONTENT}")
        string(REPLACE "AWJ_NATIVE_FILE_DND_V2" "AWJ_NATIVE_FILE_DND_V3"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "pending_native_file_drops: Vec<(winit::window::WindowId, std::path::PathBuf)>,"
            "pending_native_file_drops: Vec<(winit::window::WindowId, std::path::PathBuf, LogicalPoint)>,"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "use winit::keyboard::Key;\n"
            "use winit::keyboard::Key;${WINIT_NATIVE_DROP_POSITION}\n"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "        for (window_id, path) in pending {"
            "        for (window_id, path, position) in pending {"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "        let mut grouped: Vec<(winit::window::WindowId, String)> = Vec::new();"
            "        let mut grouped: Vec<(winit::window::WindowId, LogicalPoint, String)> = Vec::new();"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "if let Some((_, paths)) = grouped.iter_mut().find(|(id, _)| *id == window_id)"
            "if let Some((_, _, paths)) = grouped.iter_mut().find(|(id, _, _)| *id == window_id)"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "                grouped.push((window_id, path.to_string_lossy().into_owned()));"
            "                grouped.push((window_id, position, path.to_string_lossy().into_owned()));"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "        for (window_id, paths) in grouped {"
            "        for (window_id, position, paths) in grouped {"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "event.position = corelib::api::LogicalPosition::new(self.cursor_pos.x, self.cursor_pos.y);"
            "event.position = corelib::api::LogicalPosition::new(position.x, position.y);"
            WINIT_PATCHED "${WINIT_PATCHED}")
        string(REPLACE
            "            WindowEvent::DroppedFile(path) => {\n                self.pending_native_file_drops.push((window_id, path));\n            }\n"
            "${WINIT_DROPPED_FILE_EVENT}"
            WINIT_PATCHED "${WINIT_PATCHED}")
    else()
    string(REPLACE
        "use corelib::SharedString;\nuse corelib::graphics::euclid;\nuse corelib::input::{InternalKeyEvent, KeyEvent, KeyEventType, MouseEvent, TouchPhase};\nuse corelib::items::{ColorScheme, PointerEventButton};"
        "use corelib::SharedString;\nuse corelib::DataTransfer;\nuse corelib::graphics::euclid;\nuse corelib::input::{InternalKeyEvent, KeyEvent, KeyEventType, MouseEvent, TouchPhase};\nuse corelib::items::{AllowedDragActions, ColorScheme, DragAction, DropEvent, PointerEventButton};"
        WINIT_PATCHED "${WINIT_CONTENT}")
    if(WINIT_PATCHED STREQUAL WINIT_CONTENT)
        message(FATAL_ERROR "Could not add Slint Winit native-drop imports.")
    endif()
    string(REPLACE
        "    pending_mouse_move: Option<(winit::window::WindowId, LogicalPoint)>,"
        "    pending_mouse_move: Option<(winit::window::WindowId, LogicalPoint)>,\n\n    // AWJ_NATIVE_FILE_DND_V3: batch paths and the native drop position.\n    pending_native_file_drops: Vec<(winit::window::WindowId, std::path::PathBuf, LogicalPoint)>,"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "            pending_mouse_move: Default::default(),"
        "            pending_mouse_move: Default::default(),\n            pending_native_file_drops: Default::default(),"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "use winit::keyboard::Key;\n"
        "use winit::keyboard::Key;${WINIT_NATIVE_DROP_POSITION}\n"
        WINIT_PATCHED "${WINIT_PATCHED}")
    set(WINIT_FLUSH_NATIVE "\n    fn flush_pending_native_file_drops(&mut self) {\n        let pending = std::mem::take(&mut self.pending_native_file_drops);\n        let mut grouped: Vec<(winit::window::WindowId, LogicalPoint, String)> = Vec::new();\n        for (window_id, path, position) in pending {\n            if let Some((_, _, paths)) = grouped.iter_mut().find(|(id, _, _)| *id == window_id) {\n                if !paths.is_empty() { paths.push('\\n'); }\n                paths.push_str(&path.to_string_lossy());\n            } else {\n                grouped.push((window_id, position, path.to_string_lossy().into_owned()));\n            }\n        }\n        for (window_id, position, paths) in grouped {\n            let Some(window) = self.shared_backend_data.window_by_id(window_id) else { continue; };\n            let runtime_window = WindowInner::from_pub(window.window());\n            let mut data = DataTransfer::default();\n            data.set_plain_text(paths.into());\n            let mut event = DropEvent::default();\n            event.data = data;\n            event.position = corelib::api::LogicalPosition::new(position.x, position.y);\n            event.proposed_action = DragAction::Copy;\n            let allowed = AllowedDragActions { copy: true, move_: false, link: false };\n            runtime_window.process_mouse_input(MouseEvent::DragMove { event: event.clone(), allowed });\n            runtime_window.process_mouse_input(MouseEvent::Drop { event, allowed });\n        }\n    }\n")
    string(REPLACE
        "    }\n}\n\nimpl winit::application::ApplicationHandler<SlintEvent> for EventLoopState {"
        "    }${WINIT_FLUSH_NATIVE}}\n\nimpl winit::application::ApplicationHandler<SlintEvent> for EventLoopState {"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "            WindowEvent::CursorMoved { position, .. } => {"
        "${WINIT_DROPPED_FILE_EVENT}            WindowEvent::CursorMoved { position, .. } => {"
        WINIT_PATCHED "${WINIT_PATCHED}")
    string(REPLACE
        "    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {\n        self.flush_pending_mouse_move();"
        "    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {\n        self.flush_pending_mouse_move();\n        self.flush_pending_native_file_drops();"
        WINIT_PATCHED "${WINIT_PATCHED}")
    endif()

    if(NOT WINIT_PATCHED MATCHES "AWJ_NATIVE_FILE_DND_V3" OR
       NOT WINIT_PATCHED MATCHES "native_file_drop_position" OR
       NOT WINIT_PATCHED MATCHES "pending_native_file_drops.push\\(\\(window_id, path, position\\)\\)")
        message(FATAL_ERROR "Could not upgrade Slint Winit native-drop event handling.")
    endif()
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
