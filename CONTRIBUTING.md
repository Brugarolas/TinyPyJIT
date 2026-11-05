# Contributing to tinypy

Thank you for your interest in contributing to tinypy.

## Development Environment

### Prerequisites

- Python 3
- CMake 3.10+
- A C11 compiler
  - Linux/macOS: `clang` or `gcc`
  - Windows: MSVC (recommended)

### Clone the repository

```bash
git clone --recursive https://github.com/tinypy/tinypy.git
cd tinypy
```

## Build from Source

A standard local build:

```bash
python cmake_build.py Release
```

A build with deterministic math tests (same style as CI):

```bash
python cmake_build.py Release -DPK_BUILD_MATH_TESTS=ON
```

Build outputs are copied to the repository root (`main`, and `libtinypy.so`/`tinypy.dll`/`libtinypy.dylib` when available).

## Run Tests

Run unit tests:

```bash
python scripts/run_tests.py
```

Run benchmarks:

```bash
python scripts/run_tests.py benchmark
```

On Linux, you can run the existing coverage script:

```bash
bash run_tests.sh
```

## Notes for Contributors

- `python cmake_build.py` and `bash run_tests.sh` run `python prebuild.py` automatically.
- If you edit files under `python/`, regenerate embedded sources with:

```bash
python prebuild.py
```

## Pull Requests

- Keep changes focused and minimal.
- Add/update tests when code behavior changes.
- Open a PR with a clear summary of what changed and why.
