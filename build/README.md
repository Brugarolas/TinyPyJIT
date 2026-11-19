# Clank Builder — C/C++ build system written in TypeScript

Clank directly compiles translation units with Clang/GCC/MinGW/MSVC and links
static archives, shared libraries and executables. There is no CMake, Make,
Ninja or XMake in the build pipeline.

## Files

- `main-builder.ts` — incremental compiler, target DAG and linker.
- `src/types.ts` — typed public configuration API.
- `src/toolchains.ts` — compiler and linker discovery.
- `cli.ts` — reusable CLI flags (Bun or Node 22.16+ with type stripping).
- `tests/*.test.ts` — offline integration tests.
- `build-project.ts` — generic example project definition.

To embed Clank in another project, copy these files into `builder/`, then
write a project-specific entry point, e.g. `builder/zstd.build.ts`. The
separately provided Fire Zstd package contains a complete example.

## New in this version

### Pinned Git checkouts

```typescript
builder.addGitCheckout({
  name: 'upstream-source',
  repository: 'https://github.com/facebook/zstd.git',
  ref: 'v1.5.7',
  commit: 'f8745da6ff1ad1e7bab384bd1f9d742439278e99',
  directory: 'vendor/zstd',
});
```

Attach `tasks: ['upstream-source']` to a target to clone and verify that source
before discovering or compiling its `.c` files. The commit must be the exact
40-character hash. Existing cached checkouts are checked again on every build
for correct HEAD and clean tracked/untracked sources. No `git pull`, no shell evaluation,
and no silently updated tags. `--clean` intentionally preserves vendor trees.
To change an upstream revision, delete the existing vendored checkout and
rerun the build. `--dry-run` needs a pre-existing source checkout.

### Static-to-shared linking

```typescript
builder.addStaticLibrary('codec', {
  sources: [sourceTree('vendor/codec/src', { extensions: ['.c'] })],
  tasks: ['upstream-source'],
  includeDirs: ['vendor/codec/include'],
});

builder.addSharedLibrary('module', {
  sources: ['native/module.c'],
  dependencies: ['codec'],
});
```

The `codec` archive is linked into the `module` shared library, rather than
becoming a separate runtime dependency. On ELF/Mach-O, Clank now enables
position-independent code automatically for static dependencies of shared
libraries (unless explicitly disabled, which produces an error).

The link cache tracks *transitive* static archive changes, not just direct
libraries; if a dependency three levels deep changes, the final module
is relinked. Existing `addStaticLibrary`, `addSharedLibrary`, `--lto`,
`--jobs`, `--rebuild` and `--target` APIs are preserved.

`runBuildCli` now returns the `BuildResult | undefined`. Existing programs
that ignore its return value still work, while project entry points can
copy outputs into a package staging directory.

## Running tests

```bash
node --experimental-strip-types --test tests/*.test.ts
```

The tests cover Git checkouts, recursive static-link invalidation, static
archives into shared libraries, and (on Linux x86-64 with libzstd-dev)
linking the real Zstd static archive into the Fire native wrapper.

`node --experimental-strip-types` is a Node 22.16+ alternative to Bun; it
is not needed when executing the same TypeScript project through Bun.

## Toolchain notes

The build is *native*, not cross-compilation. For a particular OS/CPU, run
Clank with a compiler targeting that platform. An MSVC `.lib` linked into a
DLL does not need an extra Zstd DLL. An ELF `.so` produced from a PIC `.a`
likewise has no `libzstd.so` dependency. This does **not** statically link
all system libraries or the CRT; that is an independent ABI/redistribution
choice.
