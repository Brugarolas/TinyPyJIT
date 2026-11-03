set -e

# python prebuild.py

SRC=$(find src/ -name "*.c")

FLAGS="-std=c11 -lm -ldl -lpthread -Iinclude -O0 -Wfatal-errors -g -DDEBUG -DPK_ENABLE_OS=1"

SANITIZE_FLAGS="-fsanitize=undefined,thread -fno-sanitize=function"

if [ "$(uname)" == "Darwin" ]; then
    SANITIZE_FLAGS="-fsanitize=undefined,thread"
fi

EXAMPLES=${1:-examples/test_threads.c}

echo "Compiling C files..."
clang $FLAGS $SANITIZE_FLAGS $SRC $EXAMPLES -o main

