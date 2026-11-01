#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -DLAB_HEN_TEST -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g -Iinclude src/hen_bridge.c src/state.c \
  tests/test_hen_bridge.c -lm -o build/test-hen
for scenario in success negative-version negative-version-denied unsupported unknown-abi denied restore-failed; do
  build/test-hen "$scenario"
done
