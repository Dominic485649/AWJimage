#pragma once

// 导入结果的 UI 应用、后台扫描派发，以及 Windows 原生拖放（OLE IDropTarget）
// 的回调与注册。从 main.cpp 拆出。

#include <memory>

#include "awj_studio.h"
#include "file_drop_win32.h"
#include "import_service.h"
#include "studio_state.h"

namespace awj::studio {

void apply_import_result(AwjStudio& app, UiState& state,
                         awj::ui_import::Request request,
                         awj::ui_import::Result result);
bool enqueue_import(const std::shared_ptr<UiState>& state,
                    awj::ui_import::Request request);
void start_import_dispatcher(slint::ComponentWeakHandle<AwjStudio> weak,
                             const std::shared_ptr<UiState>& state);
void start_native_drop_registration(slint::ComponentWeakHandle<AwjStudio> weak,
                                    const std::shared_ptr<UiState>& state);

}  // namespace awj::studio
