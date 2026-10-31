#!/usr/bin/env bash
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=$(tr -d '\r\n' < "$project/VERSION")
mkdir -p -- "$project/build"
sandbox=$(mktemp -d "$project/build/linux-release-paths.XXXXXX")
repo="$sandbox/src"
workspace=$sandbox
mkdir -p -- "$repo/build" "$repo/bin/x64" "$workspace/build-tests/runtime-$version/Release" "$workspace/outside"
die() { printf '%s\n' "$*" >&2; exit 1; }
# 只加载路径校验函数，隔离测试不会编译、打包或访问签名配置。
source <(sed -n '/^assert_project_path() {/,/^}/p' "$project/scripts/package-linux-release.sh")
expect_reject() {
  if ( "$@" ) >/dev/null 2>&1; then die "非法路径被接受：$*"; fi
}
[[ $(assert_project_path "$repo/build/new/AWJ") == "$repo/build/new/AWJ" ]] || die '项目 build 运行目录失效'
[[ $(assert_project_path "$workspace/build-tests/runtime-$version/Release/AWJ") == "$workspace/build-tests/runtime-$version/Release/AWJ" ]] || die '版本化原生 runtime 目录失效'
[[ $(assert_project_path "$workspace/build-tests" build) == "$workspace/build-tests" ]] || die '原生构建目录失效'
expect_reject assert_project_path "$repo/bin/x64/Release/AWJ"
expect_reject assert_project_path "$workspace/outside/AWJ"
expect_reject assert_project_path "$workspace/build-tests/runtime-0.0.0/Release/AWJ"
expect_reject assert_project_path "$workspace/build-tests/runtime-$version/../../../outside/AWJ"
ln -s -- "$workspace/outside" "$workspace/build-tests/runtime-$version/linked"
expect_reject assert_project_path "$workspace/build-tests/runtime-$version/linked/AWJ"
printf 'Linux 发布路径 8 项契约通过；隔离夹具：%s\n' "$sandbox"
