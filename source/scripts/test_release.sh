#!/bin/sh
set -eu
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
cd "$(dirname "$0")/.."
base="${PS4_AIRPLAY_PORT:-$PWD/../work/airplay-port}"
host="$base/build/ffmpeg-video-direct-host"
mkdir -p build/release
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -Wno-unused-function \
 -fsanitize=address,undefined -g -O1 -Iinclude -Igenerated -Ithird_party/stb \
 src/now_playing.c src/player_ui.c tests/test_now_playing.c -lm -lpthread -o build/release/test-now-playing
build/release/test-now-playing tests/fixtures/album.jpg build/release/now-playing.ppm
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
 -fsanitize=address,undefined -g -O1 -Iinclude -I/opt/homebrew/opt/openssl@3/include src/media_io.c src/tls_http.c tests/test_media_io.c -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto -lpthread -o build/release/test-media-io
build/release/test-media-io
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
 -fsanitize=address,undefined -g -O1 -Iinclude -I"$base/vendor/uxplay-lib" -I"$base/../research/ffmpeg-7.1.2" -I"$host" \
 src/direct_player.c src/media_io.c src/tls_http.c tests/direct_host_shim.c tests/test_direct_ffmpeg.c "$host/libavformat/libavformat.a" \
 "$host/libavcodec/libavcodec.a" "$host/libswscale/libswscale.a" "$host/libswresample/libswresample.a" "$host/libavutil/libavutil.a" \
 -I/opt/homebrew/opt/openssl@3/include -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto -lcurl -lm -lpthread -o build/release/test-direct-ffmpeg
python3 tests/run_direct_test.py
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
 -fsanitize=address,undefined -g -O1 -Iinclude -I/opt/homebrew/opt/openssl@3/include \
 src/media_io.c src/tls_http.c tests/test_tls_http.c \
 -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto -lpthread -o build/release/test-tls-http
python3 tests/run_tls_test.py
