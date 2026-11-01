#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g -Iinclude '-DLAB_VIDEO_PROFILE_PATH="build/profile-test.txt"' \
  src/video_profile.c src/video_pacer.c src/state.c tests/test_video_profile_pacer.c \
  -lm -o build/test-profile-pacer
build/test-profile-pacer
