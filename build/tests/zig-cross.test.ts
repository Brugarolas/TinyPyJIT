import test from 'node:test';
import assert from 'node:assert/strict';
import { chmodSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import ProjectBuilder from '../main-builder.ts';
import { parseCli } from '../cli.ts';
import { detectToolchain, parseZigTarget } from '../src/toolchains.ts';

function fakeZig(): { root: string; bin: string } {
  const root = mkdtempSync(join(tmpdir(), 'clank-zig-test-'));
  const bin = join(root, 'zig-install');
  mkdirSync(bin);
  const executable = join(bin, 'zig');
  writeFileSync(executable, '#!/bin/sh\nif [ "$1" = "version" ]; then echo 0.15.2; exit 0; fi\nexit 2\n');
  chmodSync(executable, 0o755);
  return { root, bin };
}

test('Zig target parsing validates OS and CPU', () => {
  assert.deepEqual(parseZigTarget('aarch64-macos'), { triple: 'aarch64-macos', platform: 'darwin', architecture: 'arm64' });
  assert.deepEqual(parseZigTarget('x86_64-windows-gnu'), { triple: 'x86_64-windows-gnu', platform: 'win32', architecture: 'x64' });
  assert.deepEqual(parseZigTarget('x86_64-linux-musl'), { triple: 'x86_64-linux-musl', platform: 'linux', architecture: 'x64' });
  assert.throws(() => parseZigTarget('garbage-macos'), /Unsupported Zig target CPU/);
  assert.throws(() => parseZigTarget('x86_64-plan9'), /Unsupported Zig target OS/);
  assert.throws(() => parseZigTarget('something;evil'), /Invalid Zig target/);
});

test('cross-compilation is configured in TypeScript, not via CLI flags', () => {
  assert.throws(() => parseCli(['--cross-target', 'aarch64-macos']), /Unknown argument/);
  const args = parseCli(['--target', 'zstd', '--release']);
  assert.notEqual(args, 'help');
  if (args === 'help') return;
  assert.deepEqual(args.only, ['zstd']);
  assert.equal(args.profile, 'release');
});

test('Zig is discovered as one executable with cc, c++ and ar subcommands', () => {
  if (process.platform === 'win32') return;
  const { root, bin } = fakeZig();
  try {
    const toolchain = detectToolchain('zig', { toolchainDir: bin, crossTarget: 'aarch64-macos' });
    assert.equal(toolchain.cCompiler, join(bin, 'zig'));
    assert.deepEqual(toolchain.cCompilerArgs, ['cc', '-target', 'aarch64-macos']);
    assert.deepEqual(toolchain.cxxCompilerArgs, ['c++', '-target', 'aarch64-macos']);
    assert.deepEqual(toolchain.librarianArgs, ['ar']);
    assert.equal(toolchain.targetPlatform, 'darwin');
    assert.throws(() => detectToolchain('gcc', { toolchainDir: bin, crossTarget: 'aarch64-macos' }), /require the Zig toolchain/);
  } finally { rmSync(root, { force: true, recursive: true }); }
});

test('registered platform matrix plans correct Mach-O, ELF and PE artifacts', async () => {
  if (process.platform === 'win32') return;
  const { root, bin } = fakeZig();
  try {
    writeFileSync(join(root, 'source.c'), 'int answer(void) { return 42; }\n');
    const builder = new ProjectBuilder({ rootDir: root, buildDir: 'out', toolchain: 'zig', toolchainDir: bin });
    builder.addStaticLibrary('core', { sources: ['source.c'] });
    builder.addSharedLibrary('shared', { sources: ['source.c'] });
    builder.addExecutable('main', { sources: ['source.c'] });
    const targets = [
      ['aarch64-macos', '', '.dylib'], ['x86_64-linux-musl', '', '.so'], ['x86_64-windows-gnu', '.exe', '.dll'],
    ] as const;
    for (const [triple] of targets) builder.addCrossTarget(triple);
    assert.throws(() => builder.addCrossTarget('aarch64-macos'), /Duplicate cross target/);
    assert.throws(() => builder.addCrossTarget('garbage-macos'), /Unsupported Zig target CPU/);
    const messages: string[] = [];
    const old = console.log;
    console.log = (...args: unknown[]) => { messages.push(args.map(String).join(' ')); };
    let result: Awaited<ReturnType<ProjectBuilder['build']>>;
    try { result = await builder.build({ dryRun: true }); }
    finally { console.log = old; }
    assert.equal(Object.keys(result.crossTargets ?? {}).length, targets.length);
    for (const [triple, suffix, libExtension] of targets) {
      const child = result.crossTargets?.[triple];
      assert.ok(child, `missing result for ${triple}`);
      assert.ok(child.outputs.main.endsWith('main' + suffix));
      assert.ok(child.outputs.shared.endsWith('shared' + libExtension) || child.outputs.shared.endsWith('libshared' + libExtension));
      assert.ok(child.outputs.core.endsWith('libcore.a'));
      assert.ok(child.buildDir.includes(triple));
      assert.equal(result.outputs[`${triple}:core`], child.outputs.core);
      assert.match(messages.join('\n'), new RegExp(`-target ${triple}`));
    }
    assert.match(messages.join('\n'), /zig ar rcs/);
  } finally { rmSync(root, { force: true, recursive: true }); }
});

test('per-platform C target overrides select sources and macros', async () => {
  if (process.platform === 'win32') return;
  const { root, bin } = fakeZig();
  try {
    writeFileSync(join(root, 'source.c'), 'int answer(void) { return 42; }\n');
    writeFileSync(join(root, 'x64.S'), '/* dummy assembly */\n');
    const builder = new ProjectBuilder({ rootDir: root, buildDir: 'out', toolchain: 'zig', toolchainDir: bin });
    builder.addStaticLibrary('core', { sources: ['source.c'], defines: { BASE: 1 } });
    builder.addCrossTarget('x86_64-linux-musl', { targets: { core: { sources: ['source.c', 'x64.S'] } } });
    builder.addCrossTarget('aarch64-macos', { targets: { core: { defines: { NO_ASM: 1 }, pic: true } } });
    const messages: string[] = [];
    const old = console.log;
    console.log = (...args: unknown[]) => { messages.push(args.map(String).join(' ')); };
    try { await builder.build({ dryRun: true }); }
    finally { console.log = old; }
    const output = messages.join('\n');
    const x64 = output.slice(output.indexOf('[cross] x86_64-linux-musl'), output.indexOf('[cross] aarch64-macos'));
    const arm = output.slice(output.indexOf('[cross] aarch64-macos'));
    assert.match(x64, /x64\.S/);
    assert.doesNotMatch(arm, /x64\.S/);
    assert.match(arm, /-DNO_ASM=1/);
    assert.match(arm, /-DBASE=1/);
    assert.match(arm, /-fPIC/);
  } finally { rmSync(root, { force: true, recursive: true }); }
});

test('real Zig produces cross-platform static archives when installed', async t => {
  const { spawnSync } = await import('node:child_process');
  const probe = spawnSync('zig', ['version'], { encoding: 'utf8', timeout: 5000 });
  if (probe.status !== 0) { t.skip('Zig is not installed on this test host'); return; }
  const root = mkdtempSync(join(tmpdir(), 'clank-zig-real-'));
  try {
    writeFileSync(join(root, 'answer.c'), 'int zstd_test_answer(void) { return 42; }\n');
    const builder = new ProjectBuilder({ rootDir: root, buildDir: 'out', toolchain: 'zig', profile: 'release' });
    builder.addStaticLibrary('test', { sources: ['answer.c'] });
    for (const triple of ['x86_64-linux-musl', 'aarch64-macos', 'x86_64-windows-gnu']) {
      builder.addCrossTarget(triple);
    }
    const result = await builder.build();
    assert.equal(Object.keys(result.crossTargets ?? {}).length, 3);
    for (const child of Object.values(result.crossTargets ?? {})) {
      assert.ok(child.outputs.test.endsWith('libtest.a'));
      const archive = (await import('node:fs')).readFileSync(child.outputs.test);
      assert.equal(archive.subarray(0, 8).toString(), '!<arch>\n');
    }
    const cached = await builder.build();
    assert.equal(cached.compiled, 0);
    assert.equal(cached.linked, 0);
  } finally { rmSync(root, { force: true, recursive: true }); }
});


test('FireZstd project script selects modern modules and architecture-specific ASM', async () => {
  if (process.platform === 'win32') return;
  const { spawnSync } = await import('node:child_process');
  const { cpSync } = await import('node:fs');
  const { dirname, resolve } = await import('node:path');
  const { fileURLToPath } = await import('node:url');
  const project = mkdtempSync(join(tmpdir(), 'clank-firezstd-script-'));
  const builderDir = resolve(dirname(fileURLToPath(import.meta.url)), '..');
  try {
    mkdirSync(join(project, 'builder', 'src'), { recursive: true });
    for (const file of ['arch.ts', 'extended.ts', 'main-builder.ts', 'cli.ts', 'firezstd.build.ts']) {
      cpSync(join(builderDir, file), join(project, 'builder', file));
    }
    for (const file of ['types.ts', 'toolchains.ts', 'sources.ts', 'process.ts', 'depfile.ts']) {
      cpSync(join(builderDir, 'src', file), join(project, 'builder', 'src', file));
    }
    for (const folder of ['common', 'compress', 'decompress', 'dictBuilder', 'deprecated', 'legacy']) {
      mkdirSync(join(project, 'lib', folder), { recursive: true });
      writeFileSync(join(project, 'lib', folder, `${folder}.c`), 'int dummy(void) { return 1; }\n');
    }
    writeFileSync(join(project, 'lib', 'decompress', 'huf_decompress_amd64.S'), '/* stub */\n');
    const bin = join(project, 'zig-install');
    mkdirSync(bin);
    const zig = join(bin, 'zig');
    writeFileSync(zig, '#!/bin/sh\nif [ "$1" = "version" ]; then echo 0.15.2; exit 0; fi\nexit 2\n');
    chmodSync(zig, 0o755);
    const result = spawnSync(process.execPath, [
      '--experimental-strip-types', join(project, 'builder', 'firezstd.build.ts'),
      '--dry-run', '--toolchain-dir', bin,
    ], { cwd: project, encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    const output = result.stdout;
    assert.match(output, /common\.c/);
    assert.match(output, /compress\.c/);
    assert.match(output, /decompress\.c/);
    assert.match(output, /dictBuilder\.c/);
    assert.doesNotMatch(output, /deprecated\.c|legacy\.c/);
    assert.match(output, /zig ar rcs/);
    // Both library kinds must be built independently for every requested OS.
    assert.match(output, /libzstd\.a/);
    assert.match(output, /libzstd\.so/);
    assert.match(output, /libzstd\.dylib/);
    assert.match(output, /libzstd\.dll/);
    assert.match(output, /libzstd\.dll\.a/);
    assert.match(output, /-shared/);
    assert.match(output, /-dynamiclib/);
    assert.match(output, /-DZSTD_DLL_EXPORT=1/);
    assert.match(output, /-DZSTD_MULTITHREAD=1/);
    assert.equal((output.match(/\[dry-run\] \S+\s+ar rcs /g) ?? []).length, 8,
      'one static library must be archived per platform');
    assert.equal((output.match(/\[dry-run\].*\s(?:-dynamiclib|-shared)\s/g) ?? []).length, 8,
      'one shared library must be linked per platform');
    for (const triple of ['aarch64-macos', 'x86_64-linux-musl', 'x86_64-windows-gnu']) {
      assert.match(output, new RegExp(`\\[cross\\] ${triple}`));
    }
    const windows = output.slice(output.indexOf('[cross] x86_64-windows-gnu'), output.indexOf('[cross] aarch64-windows-gnu'));
    assert.match(windows, /-DZSTD_DLL_EXPORT=1/);
    assert.match(windows, /-Wl,--out-implib,/);
    assert.match(windows, /libzstd\.dll/);
    assert.match(windows, /-shared/);
    const linux = output.slice(output.indexOf('[cross] x86_64-linux-gnu'), output.indexOf('[cross] aarch64-linux-gnu'));
    assert.match(linux, /libzstd\.so/);
    assert.doesNotMatch(linux, /-DZSTD_DLL_EXPORT=1/);
    const arm = output.slice(output.indexOf('[cross] aarch64-macos'));
    const x64 = output.slice(output.indexOf('[cross] x86_64-linux-musl'), output.indexOf('[cross] aarch64-linux-musl'));
    assert.match(arm, /libzstd\.dylib/);
    assert.doesNotMatch(arm, /huf_decompress_amd64\.S/);
    assert.match(arm, /-DZSTD_DISABLE_ASM=1/);
    assert.match(x64, /huf_decompress_amd64\.S/);
    assert.doesNotMatch(x64, /-DZSTD_DISABLE_ASM=1/);
  } finally { rmSync(project, { force: true, recursive: true }); }
});

test('Zig command response files preserve cc and ar subcommands', async t => {
  if (process.platform !== 'linux') { t.skip('This test uses a native POSIX Zig-driver shim'); return; }
  const { spawnSync } = await import('node:child_process');
  if (spawnSync('clang', ['--version']).status !== 0 || spawnSync('ar', ['--version']).status !== 0) {
    t.skip('clang and ar are unavailable'); return;
  }
  const { root, bin } = fakeZig();
  try {
    const zig = join(bin, 'zig');
    writeFileSync(zig, '#!/bin/sh\n' +
      'if [ "$1" = "version" ]; then echo 0.15.2; exit 0; fi\n' +
      'if [ "$1" = "cc" ]; then shift; exec clang "$@"; fi\n' +
      'if [ "$1" = "ar" ]; then shift; exec ar "$@"; fi\n' +
      'exit 3\n');
    chmodSync(zig, 0o755);
    writeFileSync(join(root, 'source.c'), 'int answer(void) { return 42; }\n');
    const builder = new ProjectBuilder({
      rootDir: root, buildDir: 'out', toolchain: 'zig', toolchainDir: bin,
      profile: 'release', responseFileThreshold: 1,
    });
    builder.addStaticLibrary('zstd_static', { sources: ['source.c'], outputName: 'zstd' });
    builder.addSharedLibrary('zstd_shared', { sources: ['source.c'], outputName: 'zstd' });
    builder.addCrossTarget('native');
    const first = await builder.build();
    assert.equal(first.linked, 2);
    const native = first.crossTargets?.native;
    assert.ok(native?.outputs.zstd_static.endsWith('libzstd.a'));
    assert.ok(native?.outputs.zstd_shared.endsWith('libzstd.so'));
    const second = await builder.build();
    assert.equal(second.linked, 0);
    assert.equal(second.compiled, 0);
  } finally { rmSync(root, { force: true, recursive: true }); }
});
