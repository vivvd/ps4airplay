#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
  -O2 -g ${LAB_SANITIZERS:-} -Iinclude -Igenerated \
  src/state.c src/video_profile.c src/http.c src/render.c src/main_host.c -lm -o build/receiver-host
