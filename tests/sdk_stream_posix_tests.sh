#!/usr/bin/env bash
set -euo pipefail
[ "$#" -eq 4 ]
stream=$1
ffmpeg=$2
helper=$3
work=$4
[ -x "$stream" ] && [ -x "$ffmpeg" ] && [ -x "$helper" ]
[ -d "$work" ]
session=$(mktemp -d "$work/stream.XXXXXXXX")
cd "$session"
# 像素、帧协议和编码输出只经 stdin/stdout；不给工具任何业务输出文件路径。
"$ffmpeg" -hide_banner -loglevel error -f lavfi -i testsrc=size=8x6:rate=1 -frames:v 2 \
  -pix_fmt rgba -f rawvideo pipe:1 |
  "$stream" --raw rgba8 8 6 --color 1 13 0 1 --codec png --lossless | "$helper" multi
"$helper" wire | "$stream" --wire --codec png --single | "$helper" png
set +e
"$helper" truncated | "$stream" --wire --codec png | "$helper" empty
codes=("${PIPESTATUS[@]}")
set -e
[ "${codes[0]}" -eq 0 ] && [ "${codes[1]}" -ne 0 ] && [ "${codes[2]}" -eq 0 ]
[ -z "$(find "$session" -mindepth 1 -print -quit)" ]
rmdir "$session"
printf 'Linux 真实 FFmpeg 多帧、AWJFRM1/AWJRES1、单帧 PNG 与截断拒绝通过；测试工作目录无业务中间文件。\n'
printf '边界：未做系统级全进程文件 I/O 跟踪；普通管道存在 OS/接收复制，不是端到端零复制。\n'
