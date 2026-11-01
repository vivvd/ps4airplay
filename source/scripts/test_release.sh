#!/bin/sh
# ASan/UBSan native-player mock + real metadata renderer. Does not emulate PS4.
set -eu
cd "$(dirname "$0")/.."
base="${PS4_AIRPLAY_PORT:-$PWD/../work/airplay-port}"
host="$base/build/ffmpeg-video-fast-host"
mkdir -p build/release
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -Wno-unused-function \
 -fsanitize=address,undefined -g -O1 -Iinclude -Igenerated -Ithird_party/stb \
 src/now_playing.c src/player_ui.c tests/test_now_playing.c -lm -lpthread -o build/release/test-now-playing
build/release/test-now-playing tests/fixtures/album.jpg build/release/now-playing.ppm
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
 -fsanitize=address,undefined -g -O1 -Itests/direct_stubs -Iinclude -I"$base/vendor/uxplay-lib" \
 -I"$base/../research/ffmpeg-7.1.2" -I"$host" \
 src/direct_player.c tests/test_direct_player.c "$host/libswscale/libswscale.a" \
 "$host/libswresample/libswresample.a" "$host/libavutil/libavutil.a" -lm -lpthread -o build/release/test-direct
build/release/test-direct
