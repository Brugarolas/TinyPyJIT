import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, basename } from 'node:path';
import ProjectBuilder, { sourceTree } from '../main-builder';
import { runProcess } from '../src/process';
import { parseCli } from '../cli';
import { parseDepfile } from '../src/depfile';
import { globRegex } from '../src/sources';

async function fixture(): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), 'bruga build spaces '));
  const files: Record<string, string> = {
    'include/value.h': '#pragma once\n#define MAGIC 7\nint answer(void);\nint extra(void);\n',
    'lib/a/value.c': '#include "value.h"\nint answer(void) { return MAGIC; }\n',
    'lib/b/value.c': '#include "value.h"\nint extra(void) { return 1; }\n',
    'app/main.c': '#include <stdio.h>\n#include "value.h"\nint main(void) { printf("%d\\n", answer() + extra()); return 0; }\n',
  };
  for (const [file, content] of Object.entries(files)) {
    const dest = join(root, file);
    await mkdir(dirname(dest), { recursive: true });
    await writeFile(dest, content);
  }
  return root;
}

function project(root: string, toolchain: 'clang' | 'gcc' = 'clang', suffix = ''): ProjectBuilder {
  const builder = new ProjectBuilder({ rootDir: root, buildDir: 'build-ts', toolchain, profile: 'debug', jobs: 2, includeDirs: ['include'], defines: { ...suffix ? { TOKEN: suffix } : {} } });
  builder.addStaticLibrary('core', { sources: [sourceTree('lib', { extensions: ['.c'] })] });
  builder.addExecutable('sample', { sources: ['app/main.c'], dependencies: ['core'] });
  return builder;
}

test('dependency parser handles escaped spaces and continuations', () => {
  const result = parseDepfile('obj.o: a\\ b.c folder/x.h \\\n sub/y.h\n', '/project');
  assert.deepEqual(result, ['/project/a b.c', '/project/folder/x.h', '/project/sub/y.h']);
});

test('glob patterns support recursive exclusions', () => {
  assert.equal(globRegex('**/tests/**').test('a/b/tests/c.c'), true);
  assert.equal(globRegex('**/tests/**').test('tests/c.c'), true);
  assert.equal(globRegex('**/tests/**').test('src/a.c'), false);
  assert.equal(globRegex('**/jit_backend*.c').test('interpreter/jit_backend_x.c'), true);
});

test('CLI validates arguments and supports forwarding args', () => {
  const opts = parseCli(['--release', '--compiler=clang', '--jobs', '4', '--target', 'sample', '--run', '--', '-c', 'test']);
  assert.notEqual(opts, 'help');
  if (opts !== 'help') {
    assert.equal(opts.profile, 'release');
    assert.equal(opts.toolchain, 'clang');
    assert.equal(opts.jobs, 4);
    assert.deepEqual(opts.only, ['sample']);
    assert.deepEqual(opts.programArgs, ['-c', 'test']);
  }
  assert.throws(() => parseCli(['--jobs', 'nan']), /Invalid jobs/);
  assert.throws(() => parseCli(['--compiler', 'unknown']), /Invalid compiler/);
});

test('compile, link, incremental noop, header edit, source edit, flags and clean', async t => {
  const root = await fixture();
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const builder = project(root);
  const first = await builder.build();
  assert.equal(first.compiled, 3);
  assert.equal(first.linked, 2);
  assert.equal(first.reused, 0);
  let result = await runProcess([first.outputs.sample], { cwd: root });
  assert.equal(result.stdout.trim(), '8');

  const second = await builder.build();
  assert.equal(second.compiled, 0);
  assert.equal(second.linked, 0);
  assert.equal(second.reused, 3);

  const commandDB: Array<{ file: string; output: string; arguments: string[] }> = JSON.parse(await readFile(join(second.buildDir, 'compile_commands.json'), 'utf8'));
  assert.equal(commandDB.length, 3);
  const objects = commandDB.filter(x => basename(x.file) === 'value.c').map(x => x.output);
  assert.equal(objects.length, 2);
  assert.notEqual(objects[0], objects[1], 'duplicate basenames must not collide');

  await writeFile(join(root, 'include/value.h'), '#pragma once\n#define MAGIC 11\nint answer(void);\nint extra(void);\n');
  const third = await builder.build();
  assert.equal(third.compiled, 3, 'header content change invalidates dependent TUs');
  assert.equal(third.linked, 2);
  result = await runProcess([third.outputs.sample], { cwd: root });
  assert.equal(result.stdout.trim(), '12');

  await writeFile(join(root, 'lib/a/value.c'), '#include "value.h"\nint answer(void) { return MAGIC + 2; }\n');
  const fourth = await builder.build();
  assert.equal(fourth.compiled, 1);
  assert.equal(fourth.reused, 2);
  assert.equal(fourth.linked, 2);
  result = await runProcess([fourth.outputs.sample], { cwd: root });
  assert.equal(result.stdout.trim(), '14');

  const flagged = project(root, 'clang', 'a');
  const fifth = await flagged.build();
  assert.equal(fifth.compiled, 3, 'changed macro invalidates compile commands');
  await flagged.clean();
  const rebuilt = await flagged.build();
  assert.equal(rebuilt.compiled, 3, 'clean removes all caches/outputs');
});

test('compilation errors propagate without silently linking stale output', async t => {
  const root = await fixture();
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const builder = project(root);
  await builder.build();
  await writeFile(join(root, 'app/main.c'), 'int main(void) { syntax error; }\n');
  await assert.rejects(() => builder.build(), /Command failed/);
  await writeFile(join(root, 'app/main.c'), 'int main(void) { return 0; }\n');
  const recovered = await builder.build();
  assert.equal(recovered.compiled, 1);
  assert.equal(recovered.linked, 1);
});

test('target dependency cycles and unsafe build directories are rejected', async t => {
  const root = await fixture();
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  assert.throws(() => new ProjectBuilder({ rootDir: root, buildDir: '.' }), /Build directory must/);
  const builder = new ProjectBuilder({ rootDir: root });
  builder.addStaticLibrary('a', { sources: ['lib/a/value.c'], dependencies: ['b'] });
  builder.addStaticLibrary('b', { sources: ['lib/b/value.c'], dependencies: ['a'] });
  await assert.rejects(() => builder.build({ toolchain: 'clang' }), /Cyclic target dependency/);
});

test('C++ translation units use C++ driver and system runtime', async t => {
  const root = await mkdtemp(join(tmpdir(), 'bruga-cxx-'));
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  await writeFile(join(root, 'main.cpp'), '#include <iostream>\nint main() { std::cout << 42 << "\\n"; return 0; }\n');
  const builder = new ProjectBuilder({ rootDir: root, toolchain: 'clang' });
  builder.addExecutable('cpp', { sources: ['main.cpp'] });
  const result = await builder.build({ profile: 'debug' });
  const run = await runProcess([result.outputs.cpp], { cwd: root });
  assert.equal(run.stdout.trim(), '42');
});

test('GCC alternative toolchain works independently', async t => {
  if (process.platform === 'win32') return;
  const root = await fixture();
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const result = await project(root, 'gcc').build({ profile: 'release' });
  const run = await runProcess([result.outputs.sample], { cwd: root });
  assert.equal(run.stdout.trim(), '8');
});

test('TypeScript build tasks generate headers and incrementally invalidate dependents', async t => {
  const root = await mkdtemp(join(tmpdir(), 'bruga-codegen-'));
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  await mkdir(join(root, 'include'));
  await writeFile(join(root, 'seed.txt'), '5');
  await writeFile(join(root, 'lib.c'), '#include "generated.h"\nint answer(void) { return GENERATED; }\n');
  await writeFile(join(root, 'main.c'), '#include <stdio.h>\nint answer(void);\nint main(void) { printf("%d\\n", answer()); return 0; }\n');
  let taskRuns = 0;
  const builder = new ProjectBuilder({ rootDir: root, toolchain: 'clang', includeDirs: ['include'] });
  builder.addTask({
    name: 'generate', inputs: ['seed.txt'], outputs: ['include/generated.h'], version: '1',
    async action(context) {
      taskRuns++;
      const seed = (await readFile(join(context.rootDir, 'seed.txt'), 'utf8')).trim();
      await writeFile(join(context.rootDir, 'include/generated.h'), `#define GENERATED ${seed}\n`);
    },
  });
  builder.addStaticLibrary('generated', { sources: ['lib.c'], tasks: ['generate'] });
  builder.addExecutable('sample', { sources: ['main.c'], dependencies: ['generated'] });
  const first = await builder.build();
  assert.equal(taskRuns, 1);
  assert.equal((await runProcess([first.outputs.sample], { cwd: root })).stdout.trim(), '5');
  const second = await builder.build();
  assert.equal(taskRuns, 1, 'up-to-date tasks should be skipped');
  assert.equal(second.compiled, 0);
  await writeFile(join(root, 'seed.txt'), '17');
  const third = await builder.build();
  assert.equal(taskRuns, 2);
  assert.equal(third.compiled, 1);
  assert.equal((await runProcess([third.outputs.sample], { cwd: root })).stdout.trim(), '17');
});

test('shared libraries link and load with runtime rpath', async t => {
  if (process.platform === 'win32') return;
  const root = await mkdtemp(join(tmpdir(), 'bruga-shared-'));
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  await writeFile(join(root, 'lib.c'), 'int answer(void) { return 99; }\n');
  await writeFile(join(root, 'main.c'), '#include <stdio.h>\nint answer(void);\nint main(void) { printf("%d\\n", answer()); return 0; }\n');
  const builder = new ProjectBuilder({ rootDir: root, toolchain: 'clang' });
  builder.addSharedLibrary('shared', { sources: ['lib.c'] });
  builder.addExecutable('sample', { sources: ['main.c'], dependencies: ['shared'] });
  const first = await builder.build();
  const run = await runProcess([first.outputs.sample], { cwd: root });
  assert.equal(run.stdout.trim(), '99');
});

test('linker and archiver response files support paths containing spaces', async t => {
  if (process.platform === 'win32') return;
  const root = await fixture();
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const builder = new ProjectBuilder({ rootDir: root, toolchain: 'clang', includeDirs: ['include'], responseFileThreshold: 1 });
  builder.addStaticLibrary('core', { sources: [sourceTree('lib', { extensions: ['.c'] })] });
  builder.addExecutable('sample', { sources: ['app/main.c'], dependencies: ['core'] });
  const output = await builder.build();
  assert.equal((await runProcess([output.outputs.sample], { cwd: root })).stdout.trim(), '8');
});

test('TinyPy-specific CLI configuration selects src and examples without CMake', async t => {
  if (process.platform === 'win32') return;
  const root = await mkdtemp(join(tmpdir(), 'bruga-tinypy-smoke-'));
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  await mkdir(join(root, 'src'), { recursive: true });
  await mkdir(join(root, 'examples'), { recursive: true });
  await mkdir(join(root, 'include'), { recursive: true });
  await writeFile(join(root, 'src/interpreter.c'), 'int tinypy_value(void) { return 4; }\n');
  await writeFile(join(root, 'examples/main.c'), '#include <stdio.h>\nint tinypy_value(void);\nint main(void) { printf("tinypy=%d\\n", tinypy_value()); return 0; }\n');
  const script = join(process.cwd(), 'dist', 'tinypy.build.js');
  const run = await runProcess([process.execPath, script, '--release', '--compiler', 'clang', '--run'], { cwd: root });
  assert.match(run.stdout, /tinypy=4/);
  const reused = await runProcess([process.execPath, script, '--release', '--compiler', 'clang'], { cwd: root });
  assert.match(reused.stdout, /0 compiled, 2 reused, 0 linked/);
});

test('Clang ThinLTO builds executable and archives correctly', async t => {
  if (process.platform !== 'linux') return;
  const root = await fixture();
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const result = await project(root).build({ profile: 'release', lto: 'thin' });
  assert.equal((await runProcess([result.outputs.sample], { cwd: root })).stdout.trim(), '8');
});

// These tests simulate Windows toolchain directories on Linux. They verify
// discovery logic and executable grouping, not Windows PE/COFF linking.
import { chmod } from 'node:fs/promises';
import { detectToolchain, toolchainDoctor } from '../src/toolchains';

async function fakeWindowsToolchain(
  root: string, prefix: string, tools: readonly string[], triple: string,
): Promise<string> {
  const bin = join(root, prefix, 'bin');
  await mkdir(bin, { recursive: true });
  for (const tool of tools) {
    const dest = join(bin, `${tool}.exe`);
    await writeFile(dest,
      `#!/bin/sh\nif [ "$1" = "-dumpmachine" ]; then echo "${triple}"; else echo "fake ${tool} 1.0"; fi\n`);
    await chmod(dest, 0o755);
  }
  return bin;
}

test('Windows auto prioritizes GNU Clang over MinGW GCC without MSVC', async t => {
  if (process.platform === 'win32') return;
  const root = await mkdtemp(join(tmpdir(), 'bruga-windows-toolchains-'));
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const clangBin = await fakeWindowsToolchain(root, 'clang64', ['clang', 'clang++', 'llvm-ar'], 'x86_64-w64-windows-gnu');
  const gccBin = await fakeWindowsToolchain(root, 'ucrt64', ['gcc', 'g++', 'ar'], 'x86_64-w64-mingw32');
  const options = { platform: 'win32' as const, env: { PATH: '' }, extraRoots: [root] };
  const auto = detectToolchain('auto', options);
  assert.equal(auto.name, 'clang-mingw');
  assert.equal(auto.runtimeBinDir, clangBin);
  assert.equal(auto.env.PATH, clangBin);
  const gcc = detectToolchain('mingw', options);
  assert.equal(gcc.runtimeBinDir, gccBin);
  assert.equal(gcc.name, 'mingw');
  assert.match(toolchainDoctor(options), /\[OK\].*clang-mingw/);
  assert.match(toolchainDoctor(options), /\[OK\].*mingw/);
});

test('Windows explicit compiler directory works without PATH and rejects POSIX MSYS GCC', async t => {
  if (process.platform === 'win32') return;
  const root = await mkdtemp(join(tmpdir(), 'bruga-windows-gnu-'));
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const bin = await fakeWindowsToolchain(root, 'ucrt64', ['gcc', 'g++', 'ar'], 'x86_64-w64-mingw32');
  assert.equal(detectToolchain('mingw', { platform: 'win32', env: { PATH: '' }, toolchainDir: bin }).cCompiler,
    join(bin, 'gcc.exe'));
  const msys = await fakeWindowsToolchain(root, 'msys', ['gcc', 'g++', 'ar'], 'x86_64-pc-msys');
  assert.throws(() => detectToolchain('mingw', { platform: 'win32', env: { PATH: '' }, toolchainDir: msys }),
    /Cannot find a complete mingw toolchain/);
});

test('Windows never mixes GNU clang and archiver from unrelated directories', async t => {
  if (process.platform === 'win32') return;
  const root = await mkdtemp(join(tmpdir(), 'bruga-no-cross-prefix-'));
  t.after(async () => { await rm(root, { recursive: true, force: true }); });
  const clangBin = await fakeWindowsToolchain(root, 'clang64', ['clang', 'clang++'], 'x86_64-w64-windows-gnu');
  const gccBin = await fakeWindowsToolchain(root, 'ucrt64', ['ar'], 'x86_64-w64-mingw32');
  const opts = { platform: 'win32' as const, env: { PATH: clangBin + ';' + gccBin } };
  assert.throws(() => detectToolchain('clang-mingw', opts), /Cannot find a complete clang-mingw/);
});

test('Windows CLI accepts --doctor and --toolchain-dir and validates toolchain names', () => {
  const opts = parseCli(['--doctor', '--compiler', 'clang-mingw', '--toolchain-dir', 'C:\\msys64\\clang64\\bin']);
  assert.notEqual(opts, 'help');
  if (opts !== 'help') {
    assert.equal(opts.doctor, true);
    assert.equal(opts.toolchain, 'clang-mingw');
    assert.equal(opts.toolchainDir, 'C:\\msys64\\clang64\\bin');
  }
  assert.throws(() => parseCli(['--toolchain-dir']), /Missing value/);
  assert.throws(() => parseCli(['--compiler=']), /Empty value/);
});
