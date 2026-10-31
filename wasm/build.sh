#!/bin/sh
# Rebuild the browser demo in docs/ (requires emsdk on PATH).
set -e
cd "$(dirname "$0")/.."
emcc -O2 -Isrc wasm/wrapper.c src/fs.c src/fsck.c \
    -o docs/strata.js \
    -sEXPORTED_FUNCTIONS=_strata_boot,_strata_exec \
    -sEXPORTED_RUNTIME_METHODS=ccall \
    -sFORCE_FILESYSTEM=1 \
    -sMODULARIZE=1 -sEXPORT_NAME=StrataModule \
    -sSTACK_SIZE=4194304 -sALLOW_MEMORY_GROWTH=1
