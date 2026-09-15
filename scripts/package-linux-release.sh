#!/usr/bin/env bash
set -euo pipefail

die() { printf '%s\n' "$*" >&2; exit 1; }

binary=""
output_dir=""
candidate_head=""
while (($#)); do
  case "$1" in
    --binary) binary=${2-}; shift 2 ;;
    --output-dir) output_dir=${2-}; shift 2 ;;
    --candidate-head) candidate_head=${2-}; shift 2 ;;
    -h|--help)
      printf 'usage: %s --binary /native/path/AWJ [--output-dir /native/path/build/release-linux/VERSION] [--candidate-head 40-hex-sha]\n' "$0"
      exit 0
      ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ -n "$binary" ]] || die '--binary is required'
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(CDPATH= cd -- "$script_dir/.." && pwd)
[[ "$binary" = /* ]] || binary="$PWD/$binary"
binary=$(realpath -e -- "$binary")
[[ -x "$binary" && $(basename -- "$binary") == AWJ ]] || die '--binary must name an executable AWJ'

version=$(tr -d '\r\n' < "$repo/VERSION")
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die 'VERSION is invalid'
actual_version=$("$binary" --version) || die 'binary --version failed'
[[ "$actual_version" == "AWJimage $version" ]] || die "binary version mismatch: expected AWJimage $version, actual $actual_version"
[[ -z $(git -C "$repo" status --porcelain --untracked-files=no) ]] || die 'tracked source changes are not allowed'
head=$(git -C "$repo" rev-parse HEAD)
if [[ -n "$candidate_head" ]]; then
  [[ "$candidate_head" =~ ^[0-9a-f]{40}$ ]] || die '--candidate-head must be an exact 40-character lowercase commit SHA'
  [[ "$head" == "$candidate_head" ]] || die "HEAD does not match --candidate-head: expected $candidate_head, actual $head"
else
  [[ $(git -C "$repo" describe --exact-match --tags HEAD 2>/dev/null || true) == "$version" ]] || die 'HEAD must be the matching release tag'
fi
command -v 7z >/dev/null || die '7z is required'

if [[ -z "$output_dir" ]]; then output_dir="$repo/build/release-linux/$version"; fi
[[ "$output_dir" = /* ]] || output_dir="$repo/$output_dir"
output_dir=$(realpath -m -- "$output_dir")
case "$output_dir" in "$repo"/build/*) ;; *) die '--output-dir must stay below repo/build' ;; esac
[[ ! -e "$output_dir" ]] || die "refusing to overwrite existing output: $output_dir"

cmake_value() {
  sed -nE "s/^set\\($1 \"([^\"]+)\".*/\\1/p" "$repo/CMakeLists.txt" | head -n 1
}

baseline=$(sed -nE 's/.*"baseline"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p' "$repo/vcpkg-configuration.json" | head -n 1)
libavif_commit=$(cmake_value AWJ_LIBAVIF_GIT_TAG)
jpegli_commit=$(cmake_value AWJ_JPEGLI_GIT_TAG)
slint_commit=$(cmake_value AWJ_SLINT_GIT_TAG)
[[ -n "$baseline$libavif_commit$jpegli_commit$slint_commit" ]] || die 'could not read build pins'

package="$output_dir/package/AWJ_Linux"
archive="$output_dir/AWJ_Linux.7z"
mkdir -p "$package"
install -m 0755 "$binary" "$package/AWJ"
cp -- "$repo/LICENSE" "$package/LICENSE"
{
  printf '%s\n' \
    'AWJimage Release Notice' \
    '=======================' \
    '' \
    'BUILD INFORMATION' \
    '-----------------' \
    "AWJimage $version" \
    'Build Type: Release' \
    "Git Commit: $(git -C "$repo" rev-parse HEAD)" \
    'Architecture: x64' \
    'Platform: Linux' \
    "Vcpkg baseline: $baseline" \
    "libavif commit: $libavif_commit" \
    "Jpegli commit: $jpegli_commit" \
    "Slint commit: $slint_commit" \
    'libplacebo: v7.360.1' \
    'libarchive: v3.8.9' \
    'libheif: 1.23.4 (decoder-only; libde265 backend)' \
    'libde265: 1.1.2 (decoder library only)' \
    'Source: https://github.com/Dominic485649/AWJimage' \
    '' \
    'THIRD-PARTY SOFTWARE NOTICES' \
    '----------------------------'
  cat -- "$repo/THIRD_PARTY_NOTICES.txt"
  printf '\n%s\n' 'FULL THIRD-PARTY LICENSE TEXTS' '------------------------------'
  while IFS= read -r -d '' license; do
    name=${license#"$repo/"}
    printf '%s\n' \
      '-------------------------------------------------------------------------------' \
      "BEGIN FULL LICENSE: $name" \
      '-------------------------------------------------------------------------------'
    cat -- "$license"
    printf '\n%s\n' \
      '-------------------------------------------------------------------------------' \
      "END FULL LICENSE: $name" \
      '-------------------------------------------------------------------------------'
  done < <(find "$repo/licenses" -maxdepth 1 -type f -name '*.txt' -print0 | sort -z)
} > "$package/NOTICE.txt"

[[ ! -d "$package/THIRD_PARTY_LICENSES" ]] || die 'release package must not contain subdirectories'
mapfile -t package_members < <(find "$package" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' | sort)
expected_members=(AWJ LICENSE NOTICE.txt)
[[ "${package_members[*]}" == "${expected_members[*]}" ]] || die "unexpected Linux package members: ${package_members[*]}"

pushd "$package" >/dev/null
7z a -t7z "$archive" ./* -m0=lzma2 -mx=9 -mmt=1 -mf=off -mtc=off -mta=off -mtm=off >/dev/null
popd >/dev/null
7z t "$archive" >/dev/null

verify_dir=$(mktemp -d "$output_dir/verify.XXXXXX")
trap 'rm -rf -- "$verify_dir"' EXIT
7z x "$archive" "-o$verify_dir" -y >/dev/null
[[ -x "$verify_dir/AWJ" ]] || die 'archive did not retain AWJ executable mode'
while IFS= read -r -d '' source; do
  relative=${source#"$package/"}
  target="$verify_dir/$relative"
  [[ -f "$target" ]] || die "archive omitted $relative"
  [[ $(sha256sum "$source" | awk '{print $1}') == $(sha256sum "$target" | awk '{print $1}') ]] || die "archive hash mismatch: $relative"
done < <(find "$package" -type f -print0)
[[ $(find "$package" -type f | wc -l) == $(find "$verify_dir" -type f | wc -l) ]] || die 'archive contains an unexpected file'
"$verify_dir/AWJ" --help >/dev/null
[[ $("$verify_dir/AWJ" --version) == "AWJimage $version" ]] || die 'extracted binary version mismatch'

printf 'Linux package: %s\nLinux archive: %s\n' "$package" "$archive"
