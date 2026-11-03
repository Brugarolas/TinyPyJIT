/**
 * PocketPy-specific build configuration.
 * Place the Bruga Build files in <pocketpy>/builder/ and run this script
 * from <pocketpy>/, with Bun installed.
 *
 * This uses the current upstream CMake source inventory for reference, but
 * never invokes CMake, Make, Ninja, or a shell script.
 */
import ProjectBuilder, { sourceTree } from './main-builder';
import { runBuildCli } from './cli';
import { existsSync } from 'node:fs';
import { join, resolve } from 'node:path';

const cwd = process.cwd();
const root = existsSync(join(cwd, 'src')) && existsSync(join(cwd, 'src2'))
  ? cwd : existsSync(join(cwd, '..', 'src')) && existsSync(join(cwd, '..', 'src2'))
    ? resolve(cwd, '..') : cwd;
const windows = process.platform === 'win32';
const linux = process.platform === 'linux';
// Flags must be selected by the compiler family; on Windows both MSVC and
// Clang/MinGW are supported, so never use /fp:precise for a GNU compiler.
const determinismFlags = windows ? [] : ['-ffp-contract=off', '-fno-fast-math'];

const builder = new ProjectBuilder({
  rootDir: root,
  buildDir: 'build-ts',
  toolchain: 'auto', // On Windows: CLANG64 -> MinGW-w64 -> MSVC -> clang-cl.
  profile: 'debug',
  includeDirs: ['include'],
  cFlags: determinismFlags,
  threads: true,
  defines: {
    PK_ENABLE_OS: 1,
    PK_ENABLE_THREADS: 1,
    PK_ENABLE_DLL: 1,
    PK_ENABLE_DETERMINISM: 1,
    PK_ENABLE_WATCHDOG: 0,
    PK_ENABLE_CUSTOM_SNAME: 0,
    ...(windows ? { _CRT_SECURE_NO_WARNINGS: 1 } : {}),
    // No working native JIT is required for this build.
    // RegVM/jitbridge.c is an interpreter-side interface, not an SLJIT backend.
  },
  libraries: windows ? ['ws2_32'] : linux ? ['dl', 'm'] : ['m'],
});

// CMake's default source set is all src/**/*.c, not src2/**/*.c.
// Explicitly exclude optional unfinished JIT backends while preserving
// the interpreter-side jitbridge.c hook API.
const interpreterSources = sourceTree('src', {
  extensions: ['.c'],
  exclude: [
    '**/sljit/**', '**/jit/**', '**/jit_backend*.c',
    '**/sljit_*.c', '**/jit_codegen*.c', '**/regjit_backend*.c',
  ],
});

builder.addStaticLibrary('pocketpy', {
  sources: [interpreterSources],
  includeDirs: ['include'],
  pic: true,
});

builder.addExecutable('pocketpy-cli', {
  sources: ['src2/main.c'],
  includeDirs: ['include'],
  dependencies: ['pocketpy'],
  outputName: 'pocketpy',
});

runBuildCli(builder, 'pocketpy-cli').catch(error => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
