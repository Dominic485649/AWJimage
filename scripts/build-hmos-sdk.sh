#!/usr/bin/env bash
# WSL/Linux 原生入口；构建与采集，不执行目标程序，不伪造 JUnit。
set -euo pipefail
export LC_ALL=C.UTF-8
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
work=${AWJ_RELEASE_ROOT:-$(dirname -- "$repo")}
native=${AWJ_OHOS_NATIVE_ROOT:-}
llvm=${AWJ_OHOS_LLVM_ROOT:-/usr/lib/llvm-23}
vcpkg=${VCPKG_ROOT:-}
cmake=${CMAKE:-cmake}
jobs=1
mode=formal
build= installed= results= resource= package=
usage() {
    printf '%s\n' '用法：bash scripts/build-hmos-sdk.sh --native-sdk DIR --vcpkg-root DIR [选项]' \
        '选项：--work-root DIR --llvm-directory DIR --cmake FILE --jobs N' \
        '      --build-directory DIR --installed-directory DIR --results-directory DIR' \
        '      --resource-directory DIR --mode formal|candidate|legacy --package-directory DIR' \
        'formal（默认，>=1.3.5）要求干净源码及精确版本 tag；candidate 不能修改 JSON 升格。' \
        'legacy 仅保留 <1.3.5 的构建方式，不签发新正式跨构建证明或放宽历史运行验收。' \
        'results-directory 和可选的正式 package-directory 必须全新；不清理第三方缓存。'
}
while (($#)); do
    case $1 in
        --help|-h) usage; exit 0 ;;
        --work-root|--native-sdk|--llvm-directory|--vcpkg-root|--cmake|--jobs|--build-directory|--installed-directory|--results-directory|--resource-directory|--mode|--package-directory)
            (($# >= 2)) || { usage >&2; exit 2; }
            case $1 in
                --work-root) work=$2 ;; --native-sdk) native=$2 ;;
                --llvm-directory) llvm=$2 ;; --vcpkg-root) vcpkg=$2 ;;
                --cmake) cmake=$2 ;; --jobs) jobs=$2 ;;
                --build-directory) build=$2 ;; --installed-directory) installed=$2 ;;
                --results-directory) results=$2 ;; --resource-directory) resource=$2 ;;
                --mode) mode=$2 ;; --package-directory) package=$2 ;;
            esac
            shift 2 ;;
        *) printf '未知参数：%s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ $(uname -s) == Linux && $jobs =~ ^[1-9][0-9]*$ && $mode =~ ^(formal|candidate|legacy)$ ]] || { usage >&2; exit 2; }
[[ -n $native && -n $vcpkg && -d $work && -d $native && -x $vcpkg/vcpkg ]] || {
    printf '%s\n' '请提供已安装 SDK、vcpkg 和存在的项目工作根。' >&2; exit 2;
}
work=$(realpath -e -- "$work")
native=$(realpath -e -- "$native")
llvm=$(realpath -e -- "$llvm")
vcpkg=$(realpath -e -- "$vcpkg")
cmake=$(command -v -- "$cmake")
ninja=$(command -v ninja)
[[ $work/src == "$repo" ]] || { printf '%s\n' '工作根必须使用本项目原生 src 布局。' >&2; exit 2; }
for path in "$repo" "$work" "$native" "$llvm" "$vcpkg" "$cmake" "$ninja"; do
    [[ ! $path =~ ^/mnt/[A-Za-z](/|$) && $path =~ ^/[[:alnum:]_./+-]+$ ]] || {
        printf '必须使用无歧义原生路径：%s\n' "$path" >&2; exit 2;
    }
done
version=$(tr -d '\r\n' < "$repo/VERSION")
[[ $version =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || exit 2
if [[ $mode == legacy ]]; then
    [[ $(printf '%s\n' "$version" 1.3.5 | sort -V | head -n 1) == "$version" && $version != 1.3.5 ]] || {
        printf '%s\n' 'legacy 仅用于历史版本；不能绕过新正式来源采集。' >&2; exit 2;
    }
else
    "$cmake" -DACTION=check-source -DMODE="$mode" -DWORK_ROOT="$work" -P "$repo/scripts/record-hmos-build.cmake"
fi
if [[ -n $package && $mode != formal ]]; then
    printf '%s\n' '只有 formal 可以调用正式 packager；历史版使用原有入口。' >&2; exit 2
fi
build=${build:-$work/build/hmos-clang23-$version}
installed=${installed:-$work/deps/hmos-$version/installed}
results=${results:-$work/results/hmos-$version-$(date -u +%Y%m%dT%H%M%S%NZ)}
resource=${resource:-$work/tools/hmos-native26-clang23-$version/clang23-resource}
build=$(realpath -m -- "$build")
installed=$(realpath -m -- "$installed")
results=$(realpath -m -- "$results")
resource=$(realpath -m -- "$resource")
paths=("$build" "$installed" "$results" "$resource")
if [[ -n $package ]]; then
    package=$(realpath -m -- "$package")
    [[ ! -e $package ]] || { printf '%s\n' '正式 package-directory 必须全新。' >&2; exit 2; }
    paths+=("$package")
fi
for path in "${paths[@]}"; do
    [[ $path == "$work/"* && $path != "$repo" && $path != "$repo/"* && $path =~ ^/[[:alnum:]_./+-]+$ ]] || {
        printf '工作路径不安全：%s\n' "$path" >&2; exit 2;
    }
    for protected in "$native" "$llvm" "$vcpkg"; do
        if [[ $path == "$protected" || $path == "$protected/"* || $protected == "$path/"* ]]; then
            printf '输出与只读工具链/原生输入重叠：%s\n' "$path" >&2; exit 2
        fi
    done
done
for ((i=0; i<${#paths[@]}; ++i)); do
    for ((j=i+1; j<${#paths[@]}; ++j)); do
        left=${paths[i]} right=${paths[j]}
        if [[ $left == "$right" || $left == "$right/"* || $right == "$left/"* ]]; then
            printf '工作目录相同或互相嵌套：%s %s\n' "$left" "$right" >&2; exit 2
        fi
    done
done
[[ ! -e $results ]] || { printf '%s\n' '结果目录已存在；拒绝覆盖验证记录。' >&2; exit 2; }
mkdir -p -- "$results" "$work/cache/vcpkg/downloads" "$work/cache/vcpkg/archives" \
    "$work/cache/vcpkg/registries" "$results/tmp"
export TMPDIR="$results/tmp"
export AWJ_OHOS_NATIVE_ROOT="$native" AWJ_OHOS_RESOURCE_DIR="$resource" AWJ_OHOS_LLVM_ROOT="$llvm"
export VCPKG_DOWNLOADS="$work/cache/vcpkg/downloads"
export VCPKG_BINARY_SOURCES="clear;files,$work/cache/vcpkg/archives,readwrite"
export VCPKG_DEFAULT_BINARY_CACHE="$work/cache/vcpkg/archives"
export X_VCPKG_REGISTRIES_CACHE="$work/cache/vcpkg/registries"
export VCPKG_DISABLE_METRICS=1 VCPKG_MAX_CONCURRENCY="$jobs"
"$cmake" -DWORK_ROOT="$work" -DNATIVE_SDK_DIRECTORY="$native" -DLLVM_DIRECTORY="$llvm" \
    -DRESOURCE_DIRECTORY="$resource" -P "$repo/cmake/prepare-ohos-clang23-resource.cmake" \
    > "$results/resource.log" 2>&1
clang_resource=$("$llvm/bin/clang++" -print-resource-dir)
native_input_id() {
    {
        find -L "$native/sysroot" "$native/llvm/include/libcxx-ohos" "$native/llvm/lib/aarch64-linux-ohos" \
            "$native/llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos" "$clang_resource/include" \
            -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
        sha256sum "$native/oh-uni-package.json" "$llvm/bin/clang" "$llvm/bin/clang-scan-deps" \
            "$llvm/bin/ld.lld" "$llvm/bin/llvm-ar" "$llvm/bin/llvm-ranlib"
    } | sha256sum | cut -d ' ' -f 1
}
export AWJ_OHOS_INPUT_ID
AWJ_OHOS_INPUT_ID=$(native_input_id)
printf '%s\n' "$AWJ_OHOS_INPUT_ID" > "$results/native-input-id.txt"
before=$(bash "$repo/scripts/release-source-fingerprint.sh" --repository "$repo")
printf '%s\n' "$before" > "$results/source-fingerprint-before.txt"
"$vcpkg/vcpkg" install --triplet arm64-ohos-clang23 --host-triplet x64-linux \
    "--x-manifest-root=$repo/sdk" "--x-install-root=$installed" \
    "--overlay-triplets=$repo/cmake/triplets" "--overlay-ports=$repo/vcpkg-overlays" \
    "--x-buildtrees-root=$results/dependency-buildtrees" "--x-packages-root=$results/dependency-packages" \
    > "$results/dependencies.log" 2>&1
[[ $(native_input_id) == "$AWJ_OHOS_INPUT_ID" ]] || { printf '%s\n' '依赖构建期间原生输入变化。' >&2; exit 1; }
record_args=(-DMODE="$mode" -DWORK_ROOT="$work" -DBUILD_DIRECTORY="$build" -DRECORD_DIRECTORY="$results"
    -DDEPENDENCY_DIRECTORY="$installed" -DVCPKG_ROOT="$vcpkg" -DLLVM_DIRECTORY="$llvm"
    -DTARGET_NATIVE_SDK_DIRECTORY="$native" -DTARGET_RESOURCE_DIRECTORY="$resource"
    -DTARGET_CMAKE_TOOLCHAIN_FILE="$repo/cmake/toolchains/ohos-clang23.cmake")
configure_args=()
if [[ $mode != legacy ]]; then
    "$cmake" -DACTION=prepare "${record_args[@]}" -P "$repo/scripts/record-hmos-build.cmake" > "$results/prepare.log" 2>&1
    mapfile -t configure_args < "$results/hmos-configure-options.txt"
fi
"$cmake" -S "$repo/sdk/harmonyos" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
    -DCMAKE_BUILD_TYPE=Release -DAVIF_ENABLE_RELEASE_IPO=ON \
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$repo/cmake/toolchains/ohos-clang23.cmake" \
    -DVCPKG_TARGET_TRIPLET=arm64-ohos-clang23 -DVCPKG_INSTALLED_DIR="$installed" \
    -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_MANIFEST_INSTALL=OFF \
    -DVCPKG_OVERLAY_PORTS="$repo/vcpkg-overlays" -DVCPKG_OVERLAY_TRIPLETS="$repo/cmake/triplets" \
    -DAWJ_OHOS_NATIVE_ROOT="$native" -DAWJ_OHOS_RESOURCE_DIR="$resource" -DAWJ_OHOS_LLVM_ROOT="$llvm" \
    -DAWJ_SDK_NATIVE_CODECS=ON -DAWJ_SDK_FFMPEG=OFF -DAWJ_HARMONY_NODE_API=OFF \
    -DBUILD_TESTING=ON -DCMAKE_SKIP_RPATH=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DAWJ_RUNTIME_OUTPUT_ROOT="$results/runtime" -DAWJ_SDK_LINK_MAP="$results/awj-sdk.map" \
    "${configure_args[@]}" > "$results/configure.log" 2>&1
"$ninja" -C "$build" -t commands awj_sdk_validation > "$results/build-commands.log"
# nonce 只使本项目八个最终目标重新编译；无 clean-first，不清理依赖构建树。
"$cmake" --build "$build" --target awj_sdk_validation --parallel "$jobs" --verbose > "$results/build.log" 2>&1
"$cmake" --install "$build" --component AWJimageSDK --prefix "$results/install" > "$results/install.log" 2>&1
after=$(bash "$repo/scripts/release-source-fingerprint.sh" --repository "$repo")
printf '%s\n' "$after" > "$results/source-fingerprint-after.txt"
[[ $before == "$after" && $(native_input_id) == "$AWJ_OHOS_INPUT_ID" ]] || {
    printf '%s\n' '构建期间源码或原生输入变化；不能作为匹配源码的交付。' >&2; exit 1;
}
if [[ $mode != legacy ]]; then
    "$cmake" -DACTION=record "${record_args[@]}" -DBUILD_LOG="$results/build.log" \
        -DBUILD_COMMANDS="$results/build-commands.log" -DINSTALL_DIRECTORY="$results/install" \
        -DAUDIT_DIRECTORY="$results/producer-identities" -P "$repo/scripts/record-hmos-build.cmake" \
        > "$results/record.log" 2>&1
    if [[ -n $package ]]; then
        "$cmake" "${record_args[@]}" -DTARGET_PROFILE=hmos-arm64-api26-clang23 \
            -DINSTALL_DIRECTORY="$results/install" -DOUTPUT_DIRECTORY="$package" \
            -DBUILD_EVIDENCE="$results/hmos-build.json" -DBUILD_LOG="$results/build.log" \
            -DBUILD_COMMANDS="$results/build-commands.log" -DCROSS_TEST_BINARY_DIRECTORY="$results/runtime/Release" \
            -DTARGET_LINK_MAP="$results/awj-sdk.map" -P "$repo/scripts/package-sdk-release.cmake" \
            > "$results/package.log" 2>&1
    fi
    printf 'HMOS %s 实际交叉构建与来源采集完成：%s\n目标执行与 loader 均为 not-run；未签名或发布。\n' "$mode" "$results"
else
    printf '历史目标构建与安装完成：%s\n仍须遵循该版本原有预览或正式目标运行验收。\n' "$results"
fi
