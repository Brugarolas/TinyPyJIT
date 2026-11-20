"""Run the original workloads serially; report internal timing and keep raw samples.

python tools/benchmark_compare.py --engine before=tmp/perf/tinypy-before.exe \
    --engine after=build-ts-mimalloc-jit/release-mingw-x64/bin/tinypy.exe \
    --engine cpython=python --repeat 5 --output tmp/perf/results.json
"""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", required=True, metavar="NAME=EXE")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--benchmarks", nargs="+", default=["fib", "primes", "sort", "vec"])
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    root = Path(__file__).resolve().parents[1]
    engines = dict(item.split("=", 1) for item in args.engine)
    samples = {name: {engine: [] for engine in engines} for name in args.benchmarks}
    env = os.environ.copy()
    for key in ("TINYPY_JIT_STATS", "TINYPY_JIT_TRACE", "TINYPY_REGVM_DUMP"):
        env.pop(key, None)
    # Rotate engine order to reduce drift bias, never run timed processes concurrently.
    for trial in range(args.repeat):
        order = list(engines)
        order = order[trial % len(order):] + order[:trial % len(order)]
        for name in args.benchmarks:
            for engine in order:
                start = time.perf_counter()
                run = subprocess.run([engines[engine], str(root / "benchmarks" / (name + ".py"))],
                                     cwd=root, env=env, text=True, capture_output=True,
                                     timeout=180, check=True)
                wall = time.perf_counter() - start
                match = re.search(r"[Tt]ime\s*=\s*([0-9.]+)\s*s", run.stdout)
                if match is None:
                    raise RuntimeError("No workload timing in " + repr(run.stdout))
                seconds = float(match[1])
                samples[name][engine].append({"seconds": seconds, "wall_seconds": wall,
                                              "stdout": run.stdout, "stderr": run.stderr})
                print(f"{trial + 1}/{args.repeat} {name:8} {engine:12} {seconds:.6f}s", flush=True)
    print("\nMedian workload seconds:")
    for name, results in samples.items():
        print(name + ": " + ", ".join(f"{engine}={statistics.median(s['seconds'] for s in runs):.6f}"
                                      for engine, runs in results.items()))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"engines": engines, "samples": samples}, indent=2))


if __name__ == "__main__":
    main()
