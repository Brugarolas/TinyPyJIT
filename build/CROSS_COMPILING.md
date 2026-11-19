# Clank Builder: programmatic Zig cross-compilation

Clank's build plans are written in TypeScript. Install [Zig](https://ziglang.org/download/) and put `zig` on `PATH`. **No target/OS/architecture CLI flag is used.** Register each platform directly in the project script with `builder.addCrossTarget(triple)`.

```ts
import ProjectBuilder from './main-builder.ts';

const builder = new ProjectBuilder({
  rootDir: process.cwd(),
  buildDir: 'build',
  toolchain: 'zig',
  profile: 'release',
});

builder.addStaticLibrary('core', { sources: ['src/core.c'] });

builder.addCrossTarget('x86_64-windows-gnu');
builder.addCrossTarget('aarch64-windows-gnu');
builder.addCrossTarget('x86_64-linux-musl');
builder.addCrossTarget('aarch64-linux-musl');
builder.addCrossTarget('x86_64-macos');
builder.addCrossTarget('aarch64-macos');

const result = await builder.build();
console.log(result.crossTargets?.['aarch64-macos'].outputs.core);
```

`build()` builds every registered platform, in the order it was registered. When no platforms are registered, `build()` continues to build natively as before. All platforms have independent objects, depfiles, compile commands and caches (`build/release-zig-<triple>/`). `build().crossTargets` maps each triple to the original `BuildResult`; for convenience `build().outputs` also contains entries keyed by `<triple>:<target-name>`.

Targets can customize a C/C++ library per platform without duplicating the project definition:

```ts
builder.addCrossTarget('aarch64-macos', {
  targets: {
    core: {
      defines: { DISABLE_X64_ASSEMBLY: 1 },
      sources: ['src/core.c', 'src/platform/arm64.c'],
      pic: true,
    },
  },
});
```

`defines` are merged and flags/libraries are appended; `sources`, `pic` and `threads` replace the original target values. The `--target NAME` CLI option still selects a C/C++ *project target* (not a CPU or operating system), and will build it for **all** registered platforms. `--compiler`, `--release`, `--jobs`, `--dry-run`, `--clean` and `--doctor` retain their ordinary meanings. `--cross-target` is not a CLI option. `--run` is rejected for build matrices to avoid mistakenly executing a foreign binary. The output compiler always uses Zig for registered cross targets.

## FireZstd

Place the **contents** of [`meta/zstd/lib`](https://github.com/meta/zstd/tree/dev/lib) under `FireZstd/lib/`, and copy `firezstd.build.ts` alongside the Clank Builder sources in `FireZstd/builder/`. The `ZSTD_TARGETS` constant controls exactly which platforms are built. Edit it to add or remove destinations, then run:

```sh
bun run builder/firezstd.build.ts
```

The project generates **both** static `libzstd.a` archives (`zig ar`) and shared libraries (`zig cc`) for each registered platform. The two targets have independent object files and compiler flags: `zstd_static` and `zstd_shared`. Windows emits `bin/libzstd.dll` plus `lib/libzstd.dll.a`, Linux emits `lib/libzstd.so`, and macOS emits `lib/libzstd.dylib`. Shared builds enable `ZSTD_MULTITHREAD` (matching upstream defaults); static archives remain single-threaded by default. Windows shared objects define `ZSTD_DLL_EXPORT=1` so the public API is exported, and non-Windows shared objects use hidden-by-default visibility. Zstd's `common`, `compress`, `decompress` and `dictBuilder` directories are included; `legacy` and `deprecated` are excluded. Legacy format decoding is disabled via `ZSTD_LEGACY_SUPPORT=0`. Optional x86_64 Huffman assembly is enabled only for x86_64 targets; the others define `ZSTD_DISABLE_ASM=1`. Configure `MULTITHREADED_STATIC` and `MULTITHREADED_SHARED` independently in the script if needed (consumers of multithreaded archives must link the required thread libraries).

## Platform considerations

- The Windows DLL and `libzstd.dll.a` import library target the GNU ABI; use an appropriate import library for MSVC consumers. The static archive and import library are different files.
- Zig's target and libc support can vary with compiler version. Building a macOS dylib or other dynamically linked artifact may require the relevant SDK/sysroot or additional configuration; building an archive alone is not evidence that shared-library linking works. Validate with an actual Zig installation.
- Test the archives in CI on each actual platform, especially Windows ARM64 and macOS. A successful dry run only validates the build plan.
- Zig does not produce macOS universal/fat archives automatically. Package the x86_64 and ARM64 Mach-O archives separately or merge with `lipo` in a dedicated packaging step.
- Pin an upstream Zstd release or Git commit and preserve its BSD license when vendoring sources.

## Expected FireZstd artifacts

```text
build-zstd/release-zig-x86_64-windows-gnu/lib/libzstd.a
build-zstd/release-zig-x86_64-windows-gnu/bin/libzstd.dll
build-zstd/release-zig-x86_64-windows-gnu/lib/libzstd.dll.a
build-zstd/release-zig-aarch64-linux-musl/lib/libzstd.a
build-zstd/release-zig-aarch64-linux-musl/lib/libzstd.so
build-zstd/release-zig-aarch64-macos/lib/libzstd.a
build-zstd/release-zig-aarch64-macos/lib/libzstd.dylib
```

You can build only one *project target* on every registered platform with `--target zstd_shared` or `--target zstd_static`. Target triples are always declared in TypeScript, never passed through the CLI.
