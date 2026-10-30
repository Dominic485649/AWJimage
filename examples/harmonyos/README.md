# HarmonyOS 原生接入示例（未验证）

目标是华为 HarmonyOS Native SDK，不是以 OpenHarmony 或 Linux 的通过结果替代。当前开发环境没有配置 `DEVECO_SDK_HOME`、`OHOS_SDK_HOME` 或 `HARMONYOS_SDK_HOME`；尚未用真实 SDK 编译、链接或真机运行本目录。

- `sdk/harmonyos/CMakeLists.txt` 要求调用者明确提供真实 Native SDK toolchain；检查 OHOS 目标和 Clang 21+，基础 SDK 另以实际编译/链接探针检查 C++23 标准库。
- 不能只把旧 NDK 的 clang 替换为桌面 Clang，也不能把 Windows vcpkg 库、桌面 libc++ 或私有 modules/BMI 链入 HarmonyOS。
- native codecs 默认开启，但所有依赖必须来自匹配的 SDK、架构/API/运行库。若只能构建 core-only，它只证明帧/队列/协议基础库，不提供图像编码；不能称为完整 HarmonyOS SDK。
- 构建薄绑定需设置 `AWJ_HARMONY_NODE_API=ON`。`encodeRgba(ArrayBuffer, width, height)` 借用紧凑 RGBA8，在线程池中调用同一 C API；Promise 完成前，输入及所有别名必须保持不变且不能 detach。只保持 JS 引用不等于允许并发修改。
- 完成回调回到 JS 线程后解除输入引用；编码结果使用外部 ArrayBuffer，并在其 finalizer 调用 `awj_result_release`。若运行时拒绝外部缓冲，明确失败，不悄悄增加全帧/码流复制。绑定及 SDK 必须保持加载直到最后一个 finalizer 完成。
- 此示例没有 PixelMap/NativeBuffer/GPU 的特定资源导入，也没有验证运行时对外部 ArrayBuffer 的支持。把 PixelMap 先拷贝到 ArrayBuffer 不属于零复制采集。
- 本示例的 Native API 名称、模块注册和外部缓冲行为仍须以用户实际华为 SDK 的头文件、库和真机结果确认，不能因配置文件存在就宣布支持已验收。

ArkTS 使用 `index.d.ts` 中的接口声明。SDK 的公共 ABI 仍只有 C；此处是应用侧薄绑定，并非额外 C++ ABI。
