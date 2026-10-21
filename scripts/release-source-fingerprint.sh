#!/usr/bin/env bash
set -euo pipefail

# 与 release-safety.psm1 共用的只读源码指纹协议。
repo=''
while (($#)); do
  case "$1" in
    --repository) repo=${2-}; shift 2 ;;
    *) printf '未知源码指纹参数：%s\n' "$1" >&2; exit 2 ;;
  esac
done
[[ -n "$repo" ]] || { printf '%s\n' '需要 --repository' >&2; exit 2; }
repo=$(realpath -e -- "$repo")
git -C "$repo" rev-parse --git-dir >/dev/null

is_source() {
  local path=$1 rest=$1 component lower
  while :; do
    component=${rest%%/*}
    lower=${component,,}
    if [[ "$lower" =~ (^|[-_.])(secrets?|seed|private[-_]?key)([-_.]|$) ]] ||
       [[ "$lower" =~ \.(pem|pfx|key|seed)$ ]] ||
       [[ "$lower" =~ update-ed25519.*\.(hex|seed)$ ]] ||
       [[ "$lower" == release-signing.local.json ]]; then return 1; fi
    [[ "$rest" == */* ]] || break
    rest=${rest#*/}
  done
  case "$path" in
    src/*|ui/*|cmake/*|vcpkg-overlays/*|licenses/*|tests/*|scripts/*|docs/*) return 0 ;;
  esac
  [[ "$path" =~ ^(CMakeLists\.txt|CMakePresets\.json|release\.ps1|VERSION|\.gitattributes|\.gitignore|LICENSE|THIRD_PARTY_NOTICES\.txt|README(\.[^.]+)?\.md|CHANGELOG(\.[^.]+)?\.md|vcpkg[^/]*\.json)$ ]]
}

git -C "$repo" ls-files --cached --others --exclude-standard -z | LC_ALL=C sort -zu |
  while IFS= read -r -d '' path; do
    if ! is_source "$path"; then continue; fi
    cursor="$repo/$path"
    while [[ "$cursor" != "$repo" ]]; do
      [[ ! -L "$cursor" ]] || { printf '源码路径含链接：%s\n' "$path" >&2; exit 1; }
      if [[ -d "$cursor" && ! -x "$cursor" ]]; then printf '源码目录不可访问：%s\n' "$path" >&2; exit 1; fi
      cursor=${cursor%/*}
    done
    [[ -e "$repo/$path" ]] || continue
    [[ -f "$repo/$path" ]] || { printf '源码清单路径不是普通文件：%s\n' "$path" >&2; exit 1; }
    blob=$(git -C "$repo" -c core.autocrlf=input -c core.safecrlf=false hash-object "--path=$path" -- "$path")
    [[ "$blob" =~ ^[0-9a-f]{40,64}$ ]] || { printf '源码 blob 摘要无效：%s\n' "$path" >&2; exit 1; }
    printf '%s\0%s\n' "$path" "$blob"
  done | sha256sum | cut -d ' ' -f 1
