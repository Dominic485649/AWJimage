#!/usr/bin/env bash
# WSL/Linux 原生构建入口；只交叉构建，不把宿主执行冒充目标验收。
set -euo pipefail
export LC_ALL=C.UTF-8
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
work=${AWJ_RELEASE_ROOT:-$repo}
native=${AWJ_OHOS_NATIVE_ROOT:-}
llvm=${AWJ_OHOS_LLVM_ROOT:-/usr/lib/llvm-23}
vcpkg=${VCPKG_ROOT:-}
cmake=${CMAKE:-cmake}
jobs=1
build= installed= results= resource=
usage() {
    printf '%s\n' '用法：bash scripts/build-hmos-sdk.sh --native-sdk DIR --vcpkg-root DIR [选项]' \
        '选项：--work-root DIR --llvm-directory DIR --cmake FILE --jobs N' \
        '      --build-directory DIR --installed-directory DIR --results-directory DIR' \
        '      --resource-directory DIR' \
        '所有工作目录必须位于当前项目原生文件系统；results-directory 必须全新。'
}
while (($#)); do
    case $1 in
        --help|-h) usage; exit 0 ;;
        --work-root|--native-sdk|--llvm-directory|--vcpkg-root|--cmake|--jobs|--build-directory|--installed-directory|--results-directory|--resource-directory)
            (($# >= 2)) || { usage >&2; exit 2; }
            case $1 in
                --work-root) work=$2 ;; --native-sdk) native=$2 ;;
                --llvm-directory) llvm=$2 ;; --vcpkg-root) vcpkg=$2 ;;
                --cmake) cmake=$2 ;; --jobs) jobs=$2 ;;
                --build-directory) build=$2 ;; --installed-directory) installed=$2 ;;
                --results-directory) results=$2 ;; --resource-directory) resource=$2 ;;
            esac
            shift 2 ;;
        *) printf '未知参数：%s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ $(uname -s) == Linux && $jobs =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }
[[ -n $native && -n $vcpkg && -d $work && -d $native && -x $vcpkg/vcpkg ]] || {
    printf '%s\n' '请提供已安装 SDK、vcpkg 和存在的项目工作根。' >&2; exit 2;
}
work=$(realpath -e -- "$work")
native=$(realpath -e -- "$native")
llvm=$(realpath -e -- "$llvm")
vcpkg=$(realpath -e -- "$vcpkg")
[[ $work == "$repo" || $work/src == "$repo" ]] || { printf '%s\n' '工作根不属于本项目。' >&2; exit 2; }
for path in "$repo" "$work" "$native" "$llvm" "$vcpkg"; do
    [[ ! $path =~ ^/mnt/[A-Za-z](/|$) ]] || { printf '%s\n' '禁止在 Windows 挂载目录交叉构建。' >&2; exit 2; }
done
version=$(tr -d '\r\n' < "$repo/VERSION")
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || exit 2
build=${build:-$work/build/hmos-clang23-$version}
installed=${installed:-$work/build/hmos-deps-$version/installed}
results=${results:-$work/results/hmos-$version-$(date -u +%Y%m%dT%H%M%S%NZ)}
resource=${resource:-$work/tools/hmos-native26-clang23-$version/clang23-resource}
build=$(realpath -m -- "$build")
installed=$(realpath -m -- "$installed")
results=$(realpath -m -- "$results")
resource=$(realpath -m -- "$resource")
for path in "$build" "$installed" "$results" "$resource"; do
    case $path in
        "$work"/build/*|"$work"/build-*|"$work"/vcpkg-installed-*|"$work"/results/*|"$work"/tools/*) ;;
        *) printf '工作路径不安全：%s\n' "$path" >&2; exit 2 ;;
    esac
done
paths=("$build" "$installed" "$results" "$resource")
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
# ABI 缓存身份同时覆盖复制后的全部目标头/库与本次 LLVM 驱动，不读取私钥。
export AWJ_OHOS_INPUT_ID
AWJ_OHOS_INPUT_ID=$({
    find "$native/sysroot" "$native/llvm/include/libcxx-ohos" "$native/llvm/lib/aarch64-linux-ohos" \
        "$native/llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos" -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
    sha256sum "$native/oh-uni-package.json" "$llvm/bin/clang" "$llvm/bin/clang-scan-deps" "$llvm/bin/ld.lld"
} | sha256sum | cut -d ' ' -f 1)
printf '%s\n' "$AWJ_OHOS_INPUT_ID" > "$results/native-input-id.txt"
before=$(bash "$repo/scripts/release-source-fingerprint.sh" --repository "$repo")
printf '%s\n' "$before" > "$results/source-fingerprint-before.txt"
"$vcpkg/vcpkg" install --triplet arm64-ohos-clang23 --host-triplet x64-linux \
    "--x-manifest-root=$repo/sdk" "--x-install-root=$installed" \
    "--overlay-triplets=$repo/cmake/triplets" "--overlay-ports=$repo/vcpkg-overlays" \
    "--x-buildtrees-root=$results/dependency-buildtrees" "--x-packages-root=$results/dependency-packages" \
    > "$results/dependencies.log" 2>&1
"$cmake" -S "$repo/sdk/harmonyos" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$repo/cmake/toolchains/ohos-clang23.cmake" \
    -DVCPKG_TARGET_TRIPLET=arm64-ohos-clang23 -DVCPKG_INSTALLED_DIR="$installed" \
    -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_MANIFEST_INSTALL=OFF \
    -DVCPKG_OVERLAY_PORTS="$repo/vcpkg-overlays" -DVCPKG_OVERLAY_TRIPLETS="$repo/cmake/triplets" \
    -DAWJ_OHOS_NATIVE_ROOT="$native" -DAWJ_OHOS_RESOURCE_DIR="$resource" -DAWJ_OHOS_LLVM_ROOT="$llvm" \
    -DAWJ_SDK_NATIVE_CODECS=ON -DAWJ_SDK_FFMPEG=OFF -DAWJ_HARMONY_NODE_API=OFF \
    -DBUILD_TESTING=ON -DCMAKE_SKIP_RPATH=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DAWJ_RUNTIME_OUTPUT_ROOT="$results/runtime" -DAWJ_SDK_LINK_MAP="$results/awj-sdk.map" \
    > "$results/configure.log" 2>&1
"$cmake" --build "$build" --target awj_sdk_validation --parallel "$jobs" > "$results/build.log" 2>&1
"$cmake" --install "$build" --component AWJimageSDK --prefix "$results/install" > "$results/install.log" 2>&1
after=$(bash "$repo/scripts/release-source-fingerprint.sh" --repository "$repo")
printf '%s\n' "$after" > "$results/source-fingerprint-after.txt"
[[ $before == "$after" ]] || { printf '%s\n' '构建期间源码变化；当前产物不能作为匹配源码的交付。' >&2; exit 1; }
"$llvm/bin/clang++" --version > "$results/compiler.txt"
"$llvm/bin/llvm-readobj" --file-headers --needed-libs --dynamic-table \
    "$results/install/lib/libawj_sdk.so" > "$results/library-identity.txt"
sha256sum "$results/install/lib/libawj_sdk.so" "$results/awj-sdk.map" > "$results/output-sha256.txt"
printf '%s\n' 'status=cross-linked' 'target_execution=not-run' 'profile=hmos-arm64-api26-clang23' \
    "version=$version" "git_head=$(git -C "$repo" rev-parse HEAD)" "source_fingerprint=$after" > "$results/BUILD-INPUTS.txt"
printf '目标构建与安装已完成：%s\n尚未执行真实 HMOS 目标测试，不能据此正式发布。\n' "$results"
