#!/usr/bin/env bash
set -euo pipefail

die() { printf '%s\n' "$*" >&2; exit 1; }
binary=""
output_dir=""
candidate_head=""
local_candidate=0
build_directory=""
build_parallelism=1
while (($#)); do
  case "$1" in
    --binary) binary=${2-}; shift 2 ;;
    --output-dir) output_dir=${2-}; shift 2 ;;
    --candidate-head) candidate_head=${2-}; shift 2 ;;
    --local-candidate) local_candidate=1; shift ;;
    --build-directory) build_directory=${2-}; shift 2 ;;
    --build-parallelism) build_parallelism=${2-}; shift 2 ;;
    -h|--help)
      printf 'usage: %s --binary /native/path/AWJ --build-directory /native/path/build-release [--build-parallelism 1] [--output-dir build/release-linux/VERSION] [--local-candidate | --candidate-head 40-hex-sha]\n' "$0"
      exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done
[[ -n "$binary" ]] || die '--binary is required'
[[ "$build_parallelism" =~ ^[1-9][0-9]?$ && "$build_parallelism" -le 64 ]] || die '--build-parallelism must be 1..64'
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(CDPATH= cd -- "$script_dir/.." && pwd)
repo=$(realpath -e -- "$repo")
workspace=$(dirname -- "$repo")
command -v mountpoint >/dev/null || die 'mountpoint is required for project path validation'
assert_project_path() {
  local original absolute scope cursor kind=${2-runtime}
  original=$1
  [[ "$original" = /* ]] || original="$PWD/$original"
  absolute=$(realpath -ms -- "$original")
  case "$absolute" in "$repo/bin/x64"|"$repo/bin/x64/"*) die 'bin/x64 is manually managed and must not be touched' ;; esac
  case "$absolute" in
    "$repo/build/"*) scope=$repo ;;
    *)
      [[ $(basename -- "$repo") == src ]] || die 'build and runtime paths must stay in this project build subtree'
      if [[ "$kind" == build ]]; then
        case "$absolute" in "$workspace/build-release"|"$workspace/build-release/"*|"$workspace/build-tests"|"$workspace/build-tests/"*) scope=$workspace ;; *) die 'unknown native project build path' ;; esac
      else
        case "$absolute" in "$workspace/bin-release"|"$workspace/bin-release/"*|"$workspace/bin-tests"|"$workspace/bin-tests/"*) scope=$workspace ;; *) die 'unknown native project runtime path' ;; esac
      fi ;;
  esac
  cursor=$absolute
  while [[ "$cursor" != "$scope" ]]; do
    [[ ! -L "$cursor" ]] || die "refusing a symbolic-link path: $cursor"
    if [[ -d "$cursor" ]] && mountpoint -q -- "$cursor"; then die "refusing a mount below the project root: $cursor"; fi
    cursor=${cursor%/*}
    [[ -n "$cursor" ]] || die 'invalid project path ancestor'
  done
  printf '%s\n' "$absolute"
}
[[ "$binary" = /* ]] || binary="$PWD/$binary"
binary=$(assert_project_path "$binary")
binary=$(realpath -e -- "$binary")
[[ -x "$binary" && $(basename -- "$binary") == AWJ ]] || die '--binary must name an executable AWJ'
case "$repo:$binary" in *'/mnt/'*) die 'build and package from the native Linux filesystem, not a Windows mount' ;; esac
version=$(tr -d '\r\n' < "$repo/VERSION")
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || die 'VERSION is invalid'
tracked_status=$(git -C "$repo" status --porcelain --untracked-files=no)
untracked_sources=$(git -C "$repo" ls-files --others --exclude-standard -- src ui cmake tests scripts docs licenses vcpkg-overlays)
head=$(git -C "$repo" rev-parse HEAD)
[[ "$head" =~ ^[0-9a-f]{40}$ ]] || die 'invalid source HEAD'
source_dirty=false
if [[ -n "$tracked_status$untracked_sources" ]]; then source_dirty=true; fi
if ((local_candidate)); then
  [[ -z "$candidate_head" ]] || die '--local-candidate must not be combined with --candidate-head'
  package_mode=local-candidate
else
  [[ "$source_dirty" == false ]] || die 'source changes require explicit --local-candidate'
  package_mode=formal
  if [[ -n "$candidate_head" ]]; then
    [[ "$candidate_head" =~ ^[0-9a-f]{40}$ && "$head" == "$candidate_head" ]] || die '--candidate-head must match the exact clean HEAD'
    package_mode=clean-candidate
  else
    [[ $(git -C "$repo" describe --exact-match --tags HEAD 2>/dev/null || true) == "$version" ]] || die 'HEAD must be the matching release tag'
  fi
fi
source_fingerprint=$(bash "$script_dir/release-source-fingerprint.sh" --repository "$repo")
[[ "$source_fingerprint" =~ ^[0-9a-f]{64}$ ]] || die 'source fingerprint is invalid'
command -v cmake >/dev/null || die 'cmake is required'
command -v 7z >/dev/null || die '7z is required'

# 将增量构建、当前源码指纹和真实二进制绑定，不能仅给旧二进制补写新源码标签。
if [[ -n "$build_directory" ]]; then
  build_directory=$(assert_project_path "$build_directory" build)
  build_directory=$(realpath -e -- "$build_directory")
  case "$build_directory" in /mnt/*) die 'build directory must be native Linux storage' ;; esac
  cache=$(assert_project_path "$build_directory/CMakeCache.txt" build)
  [[ -f "$cache" ]] || die '--build-directory must contain the configured AWJ build'
  cache_value() { sed -n "s/^$1:[^=]*=//p" "$cache"; }
  [[ $(realpath -e -- "$(cache_value CMAKE_HOME_DIRECTORY)") == "$repo" ]] || die 'build cache belongs to a different source tree'
  [[ $(cache_value CMAKE_BUILD_TYPE) == Release ]] || die 'Release build is required'
  [[ $(cache_value AVIF_ENABLE_RELEASE_IPO) == ON && $(cache_value AWJ_ENABLE_X64_V3) == ON ]] || die 'IPO/LTO and x86-64-v3 must stay enabled'
  [[ $(cache_value CMAKE_CXX_FLAGS) == *'-stdlib=libc++'* ]] || die 'the native libc++ toolchain must be retained'
  compiler=$(cache_value CMAKE_CXX_COMPILER)
  "$compiler" --version | grep -q 'clang version 23\.' || die 'Clang 23 is required'
  runtime_root=$(assert_project_path "$(cache_value AWJ_RUNTIME_OUTPUT_ROOT)")
  verified_binary=$(assert_project_path "$runtime_root/Release/AWJ")
  proof_path=$(assert_project_path "$binary.awj-source-proof")
  [[ -n "$runtime_root" && "$verified_binary" == "$binary" ]] || die '--binary is not the configured runtime output'
  build_proof=$(assert_project_path "$build_directory/.awj-build-provenance" build)
  profile_hash=$(sha256sum "$cache" | cut -d ' ' -f 1)
  reuse_inputs=false
  if [[ -f "$build_proof" ]]; then
    read -r prior_source prior_profile prior_binary prior_extra < "$build_proof"
    if [[ -z "${prior_extra-}" && "$prior_source" == "$source_fingerprint" && "$prior_profile" == "$profile_hash" && "$prior_binary" == "$(sha256sum "$binary" | cut -d ' ' -f 1)" ]]; then reuse_inputs=true; fi
  fi
  if [[ "$reuse_inputs" != true ]]; then
    # 未经来源证明的旧时间戳不能冒充当前源码，刷新项目输入而不清除有效依赖。
    while IFS= read -r -d '' source_path; do
      case "$source_path" in src/*|ui/*) ;; *) continue ;; esac
      lower=${source_path,,}
      [[ "$lower" =~ (^|[/_.-])(secrets?|seed|private[-_]?key)([/_.-]|$)|\.(pem|pfx|key|seed)$|update-ed25519.*\.(hex|seed)$ ]] && continue
      source_input="$repo/$source_path"
      cursor=$source_input
      while [[ "$cursor" != "$repo" ]]; do [[ ! -L "$cursor" ]] || die 'source input contains a symbolic link'; cursor=${cursor%/*}; done
      if [[ -f "$source_input" ]]; then touch -- "$source_input"; fi
    done < <(git -C "$repo" ls-files --cached --others --exclude-standard -z | LC_ALL=C sort -zu)
  fi
  cmake --build "$build_directory" --target AWJ --parallel "$build_parallelism"
  [[ $(bash "$script_dir/release-source-fingerprint.sh" --repository "$repo") == "$source_fingerprint" && $(git -C "$repo" rev-parse HEAD) == "$head" ]] || die 'source changed during the build'
  printf '%s %s %s\n' "$source_fingerprint" "$(sha256sum "$cache" | cut -d ' ' -f 1)" "$(sha256sum "$binary" | cut -d ' ' -f 1)" > "$build_proof"
  printf '%s %s\n' "$source_fingerprint" "$(sha256sum "$binary" | cut -d ' ' -f 1)" > "$proof_path"
fi
proof_path=$(assert_project_path "$binary.awj-source-proof")
[[ -f "$proof_path" ]] || die 'missing build provenance; provide --build-directory to verify an incremental build'
read -r proof_source proof_binary extra < "$binary.awj-source-proof"
[[ -z "${extra-}" && "$proof_source" == "$source_fingerprint" && "$proof_binary" =~ ^[0-9a-f]{64}$ && "$proof_binary" == "$(sha256sum "$binary" | cut -d ' ' -f 1)" ]] || die 'binary does not match the verified source build'
actual_version=$("$binary" --version) || die 'binary --version failed'
[[ "$actual_version" == "AWJimage $version" ]] || die "binary version mismatch: $actual_version"
if [[ -z "$output_dir" ]]; then output_dir="$repo/build/release-linux/$version"; fi
[[ "$output_dir" = /* ]] || output_dir="$repo/$output_dir"
output_dir=$(assert_project_path "$output_dir" build)
case "$output_dir" in "$repo"/build/*) ;; *) die '--output-dir must stay below repo/build' ;; esac
[[ ! -e "$output_dir" ]] || die "refusing to overwrite existing output: $output_dir"
lock_value() {
  cmake "-DLOCK_FILE=$repo/cmake/codec-dependencies.json" "-DDEPENDENCY=$1" "-DFIELD=$2" -P "$repo/cmake/read_codec_lock.cmake" 2>&1
}
baseline=$(sed -nE 's/.*"baseline"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p' "$repo/vcpkg-configuration.json" | head -n 1)
libavif_commit=$(lock_value libavif commit)
jpegli_commit=$(lock_value jpegli commit)
slint_commit=$(lock_value slint commit)
heif_version=$(lock_value libheif tag)
de265_version=$(lock_value libde265 tag)
ultrahdr_version=$(lock_value libultrahdr tag)
placebo_version=$(lock_value libplacebo version)
archive_version=$(lock_value libarchive version)
winit_version=$(lock_value winit version)
winit_commit=$(lock_value winit commit)
[[ -n "$baseline$libavif_commit$jpegli_commit$slint_commit$placebo_version$archive_version$winit_commit" ]] || die 'could not read build pins'
package="$output_dir/package/AWJ_Linux"
archive="$output_dir/AWJ_Linux.7z"
mkdir -p "$package"
install -m 0755 "$binary" "$package/AWJ"
cp -- "$repo/LICENSE" "$package/LICENSE"
{
  printf '%s\n' 'AWJimage Release Notice' '=======================' '' 'BUILD INFORMATION' '-----------------' \
    "AWJimage $version" 'Build Type: Release' "Build Mode: $package_mode" "Git Commit: $head" \
    "Source Fingerprint: $source_fingerprint" 'Architecture: x64' 'Minimum CPU: x86-64-v3' \
    'Release IPO/LTO: ON' 'C++ Runtime: libc++' 'Platform: Linux' "Vcpkg baseline: $baseline" \
    "libavif commit: $libavif_commit" "Jpegli commit: $jpegli_commit" "Slint commit: $slint_commit" \
    "libplacebo: v$placebo_version" "libarchive: v$archive_version" \
    "winit: $winit_version ($winit_commit; project-local DPI backport)" \
    "libheif: ${heif_version#v} (decoder-only; libde265 backend)" \
    "libde265: ${de265_version#v} (decoder library only)" \
    "libultrahdr: ${ultrahdr_version#v} (internal ISO 21496-1 computation engine)" \
    'Source: https://github.com/Dominic485649/AWJimage' '' 'THIRD-PARTY SOFTWARE NOTICES' '----------------------------'
  cat -- "$repo/THIRD_PARTY_NOTICES.txt"
  printf '\n%s\n' 'FULL THIRD-PARTY LICENSE TEXTS' '------------------------------'
  while IFS= read -r -d '' license; do
    name=${license#"$repo/"}
    printf '%s\n' '-------------------------------------------------------------------------------' "BEGIN FULL LICENSE: $name" '-------------------------------------------------------------------------------'
    cat -- "$license"
    printf '\n%s\n' '-------------------------------------------------------------------------------' "END FULL LICENSE: $name" '-------------------------------------------------------------------------------'
  done < <(find "$repo/licenses" -maxdepth 1 -type f -name '*.txt' -print0 | sort -z)
} > "$package/NOTICE.txt"
mapfile -t package_members < <(find "$package" -mindepth 1 -maxdepth 1 -printf '%f\n' | sort)
expected_members=(AWJ LICENSE NOTICE.txt)
[[ "${package_members[*]}" == "${expected_members[*]}" ]] || die "unexpected Linux package members: ${package_members[*]}"
pushd "$package" >/dev/null
7z a -t7z "$archive" ./* -m0=lzma2 -mx=9 -mmt=1 -mf=off -mtc=off -mta=off -mtm=off >/dev/null
popd >/dev/null
7z t "$archive" >/dev/null
verify_dir=$(mktemp -d "$output_dir/verify.XXXXXX")
7z x "$archive" "-o$verify_dir" -y >/dev/null
[[ -x "$verify_dir/AWJ" ]] || die 'archive did not retain AWJ executable mode'
while IFS= read -r -d '' source; do
  relative=${source#"$package/"}
  target="$verify_dir/$relative"
  [[ -f "$target" ]] || die "archive omitted $relative"
  [[ $(sha256sum "$source" | cut -d ' ' -f 1) == $(sha256sum "$target" | cut -d ' ' -f 1) ]] || die "archive hash mismatch: $relative"
done < <(find "$package" -type f -print0)
[[ $(find "$package" -type f | wc -l) == $(find "$verify_dir" -type f | wc -l) ]] || die 'archive contains an unexpected file'
"$verify_dir/AWJ" --help >/dev/null
[[ $("$verify_dir/AWJ" --version) == "AWJimage $version" ]] || die 'extracted binary version mismatch'
[[ $(bash "$script_dir/release-source-fingerprint.sh" --repository "$repo") == "$source_fingerprint" && $(git -C "$repo" rev-parse HEAD) == "$head" ]] || die 'source changed while packaging'
lock_sha=$(sed 's/\r$//' "$repo/cmake/codec-dependencies.json" | sha256sum | cut -d ' ' -f 1)
cat > "$output_dir/build-evidence.json" <<EOF
{
  "schema": 1,
  "mode": "$package_mode",
  "platform": "linux-x64",
  "version": "$version",
  "source_fingerprint_algorithm": "sha256/git-normalized-source-v1",
  "source_fingerprint": "$source_fingerprint",
  "git_head": "$head",
  "source_dirty": $source_dirty,
  "dependency_lock_sha256": "$lock_sha",
  "profile": {"build_type":"Release", "minimum_cpu":"x86-64-v3", "ipo":true, "cxx_runtime":"libc++", "compiler":"Clang 23"},
  "binaries": [{"path":"AWJ", "size":$(stat -c %s "$package/AWJ"), "sha256":"$proof_binary"}],
  "archive": {"path":"AWJ_Linux.7z", "size":$(stat -c %s "$archive"), "sha256":"$(sha256sum "$archive" | cut -d ' ' -f 1)"},
  "verification": {"compiled_source_verified":true, "native_execution":true, "executable_mode":true, "archive_roundtrip":true}
}
EOF
printf 'Linux package: %s\nLinux archive: %s\nBuild evidence: %s\n' "$package" "$archive" "$output_dir/build-evidence.json"
