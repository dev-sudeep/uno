#!/bin/bash

# Configurable options
compiler="clang"
source="uno.c"
output="uno"
flags="-Wall -Wextra -pedantic"

# Compile
echo "[INFO] Compiling $source with $compiler..."
if $compiler -o "$output" "$source" $flags; then
    echo "build.sh [SUCCESS] Build complete: $output"
else
    echo "build.sh: [ERROR] Build failed"
    exit 1
fi
