#!/usr/bin/env bash
# Host build + run for the ct_prov golden-vector tests.
# Fragment layer is pure C; envelope layer also needs cJSON from the ESP-IDF
# source tree (same copy the firmware build links against).
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-gcc}"
IDF_PATH="${IDF_PATH:-/root/esp/esp-idf}"
CJSON_DIR="$IDF_PATH/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "cJSON.c not found under $CJSON_DIR (set IDF_PATH)" >&2
    exit 1
fi
mkdir -p build

"$CC" -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I../include -I. \
    test_fragment.c ../src/ct_prov_fragment.c \
    -o build/test_fragment

./build/test_fragment

"$CC" -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I../include -I. -I"$CJSON_DIR" \
    test_envelope.c ../src/ct_prov_envelope.c "$CJSON_DIR/cJSON.c" \
    -o build/test_envelope

./build/test_envelope
