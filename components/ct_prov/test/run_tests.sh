#!/usr/bin/env bash
# Host build + run for the ct_prov golden-vector tests.
# Requires nothing but gcc; the fragment layer is pure C with no ESP deps.
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-gcc}"
mkdir -p build

"$CC" -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I../include -I. \
    test_fragment.c ../src/ct_prov_fragment.c \
    -o build/test_fragment

./build/test_fragment
