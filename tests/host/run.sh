#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
test_binary=$(mktemp "${TMPDIR:-.}/shizuku-task-test.XXXXXX")
trap 'rm -f "$test_binary"' EXIT
"${CXX:-g++}" -std=c++23 -Wall -Wextra -Werror -g \
  -fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer \
  -Itests/host -Iinternal_headers tests/host/task_grants.cpp -o "$test_binary"
"$test_binary"

"${CXX:-g++}" -std=c++23 -Wall -Wextra -Werror -Wno-volatile -g -pthread \
  -fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer \
  -Itests/host -Iinternal_headers tests/host/object_destroy.cpp \
  source/cpu_manager/init.cpp source/kernel/{init,thread,dispatch,destroy}.cpp \
  source/kernel_object/{handler,memory,destroy}.cpp -o "$test_binary"
"$test_binary"
