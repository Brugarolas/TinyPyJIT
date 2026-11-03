#!/usr/bin/env python3
"""Merge Clang/LLVM raw PGO profiles into a single indexed profile."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("profile_dir", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--llvm-profdata", default="llvm-profdata")
    args = parser.parse_args()

    tool = shutil.which(args.llvm_profdata)
    if tool is None:
        parser.error(f"cannot find {args.llvm_profdata!r} in PATH")

    raw = sorted(args.profile_dir.glob("*.profraw"))
    if not raw:
        parser.error(f"no .profraw files found in {args.profile_dir}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [tool, "merge", "-output", str(args.output), *map(str, raw)]
    completed = subprocess.run(cmd, check=False)
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
