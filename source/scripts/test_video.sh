#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
video_base="${PS4_AIRPLAY_PORT:-$PWD/../work/airplay-port}"
video_host="${PS4_HOST_FFMPEG:-$video_base/build/ffmpeg-video-fast-host}"
mkdir -p build
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g -O1 -Iinclude \
  -I"$video_base/../research/ffmpeg-7.1.2" -I"$video_host" \
  src/video_receiver.c src/video_profile.c src/video_pacer.c src/airplay_threads.c src/state.c tests/test_video.c \
  "$video_host/libavcodec/libavcodec.a" "$video_host/libswscale/libswscale.a" \
  "$video_host/libavutil/libavutil.a" -lm -lpthread -o build/test-video
build/test-video tests/fixtures/video/landscape.packets tests/fixtures/video/portrait.packets \
  tests/fixtures/video/720p-high.packets tests/fixtures/video/oversize.packets tests/fixtures/video/long-gop.packets tests/fixtures/video/1080p-high.packets
