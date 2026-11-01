#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -DLAB_PS4 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g -Iinclude -Itests/fixtures \
  '-DLAB_VIDEO_PROFILE_PATH="build/orbis-profile-test.txt"' \
  src/video_profile.c src/state.c tests/test_profile_orbis_io.c -lm -o build/test-profile-orbis
build/test-profile-orbis
