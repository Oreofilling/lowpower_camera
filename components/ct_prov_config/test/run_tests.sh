#!/usr/bin/env bash
# Host build + run for the ct_prov_config F2 tests (capabilities, config.get,
# config.set, communication.scan). The component is pure C on top of the
# ct_prov envelope layer and cJSON from the ESP-IDF source tree (same copy
# the firmware build links against).
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

CFLAGS="-std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I../include -I../../ct_prov/include -I\"$CJSON_DIR\""

SRCS="../src/ct_prov_caps.c ../src/ct_prov_set.c ../src/ct_prov_scan.c \
    ../../ct_prov/src/ct_prov_envelope.c \"$CJSON_DIR/cJSON.c\""

for t in caps set scan; do
    eval "$CC" $CFLAGS test_$t.c $SRCS -o build/test_$t
    ./build/test_$t
done
