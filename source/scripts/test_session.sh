#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g -Iinclude '-DLAB_TRACE_PATH="build/session-test.log"' \
  src/session_trace.c src/airplay_threads.c src/state.c tests/test_session_trace.c \
  -lm -lpthread -o build/test-session
build/test-session
