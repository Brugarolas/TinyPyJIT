/**
 * TinyPy project definition for Bruga Build.
 *
 * Commands (from the project root):
 *   bun run build/tinypy.build.ts --release --compiler mingw --allocator mimalloc
 *   bun run build/tinypy.build.ts --release --compiler mingw --allocator system
 *
 * All C sources are compiled directly; CMake and shell scripts are not used.
 */
import ProjectBuilder, { CLANG_MERGED_PROFILE, isClangDriver, sourceTree } from './main-builder.ts';
import { parseCli, runBuildCli } from './cli.ts';
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdir, readdir, rm } from 'node:fs/promises';
import { delimiter, join, resolve } from 'node:path';

type InterpreterMode = 'regvm' | 'stack';
type AllocatorMode = 'mimalloc' | 'system';
type JitMode = 'on' | 'off';

interface TinyPyOptions {
  readonly interpreter: InterpreterMode;
  readonly allocator: AllocatorMode;
  readonly jit: JitMode;
  readonly mirTest: boolean;
  readonly pgoTrain: boolean;
  readonly cliArgs: string[];
}

/** Read project-specific arguments without interfering with Bruga's CLI parser. */
function selectTinyPyOptions(argv: readonly string[]): TinyPyOptions {
  let interpreter: InterpreterMode = 'regvm';
  let allocator: AllocatorMode = 'mimalloc';
  let jit: JitMode = 'off';
  let mirTest = false;
  let pgoTrain = false;
  let interpreterSelected = false;
  let allocatorSelected = false;
  let jitSelected = false;
  const cliArgs: string[] = [];

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--') {
      cliArgs.push(...argv.slice(i));
      break;
    }

    if (arg === '--mir-test') {
      if (mirTest) throw new Error('Specify --mir-test only once.');
      mirTest = true;
      continue;
    }

    if (arg === '--pgo-train') {
      if (pgoTrain) throw new Error('Specify --pgo-train only once.');
      pgoTrain = true;
      continue;
    }

    const equal = arg.indexOf('=');
    const option = equal === -1 ? arg : arg.slice(0, equal);
    if (option !== '--interpreter' && option !== '--allocator' && option !== '--jit') {
      cliArgs.push(arg);
      continue;
    }

    let value: string;
    if (equal !== -1) {
      value = arg.slice(equal + 1);
    } else {
      const next = argv[i + 1];
      if (!next || next.startsWith('--')) {
        throw new Error(`Missing value for ${option}.`);
      }
      value = next;
      i++;
    }

    if (option === '--interpreter') {
      if (interpreterSelected) throw new Error('Specify --interpreter only once.');
      if (value !== 'regvm' && value !== 'stack') {
        throw new Error(`Invalid interpreter ${JSON.stringify(value)}. Expected regvm or stack.`);
      }
      interpreter = value;
      interpreterSelected = true;
    } else if (option === '--allocator') {
      if (allocatorSelected) throw new Error('Specify --allocator only once.');
      if (value !== 'mimalloc' && value !== 'system') {
        throw new Error(`Invalid allocator ${JSON.stringify(value)}. Expected mimalloc or system.`);
      }
      allocator = value;
      allocatorSelected = true;
    } else {
      if (jitSelected) throw new Error('Specify --jit only once.');
      if (value !== 'on' && value !== 'off') {
        throw new Error(`Invalid JIT mode ${JSON.stringify(value)}. Expected on or off.`);
      }
      jit = value;
      jitSelected = true;
    }
  }

  if (jit === 'on' && interpreter !== 'regvm') {
    throw new Error('The native JIT requires --interpreter regvm.');
  }
  if (mirTest && jit !== 'on') {
    throw new Error('--mir-test requires --jit on.');
  }
  if (pgoTrain && mirTest) {
    throw new Error('--pgo-train builds the interpreter; do not combine it with --mir-test.');
  }
  return { interpreter, allocator, jit, mirTest, pgoTrain, cliArgs };
}

const { interpreter, allocator, jit, mirTest, pgoTrain, cliArgs } = selectTinyPyOptions(process.argv.slice(2));

const cwd = process.cwd();
const hasProjectLayout = (path: string): boolean =>
  existsSync(join(path, 'src')) && existsSync(join(path, 'examples'));
const root = hasProjectLayout(cwd)
  ? cwd
  : hasProjectLayout(resolve(cwd, '..'))
    ? resolve(cwd, '..')
    : cwd;

const useMimalloc = allocator === 'mimalloc';
const mimallocSource = join(root, 'lib', 'mimalloc', 'src', 'static.c');
const mimallocHeader = join(root, 'lib', 'mimalloc', 'include', 'mimalloc.h');
const mimallocBootstrap = join(root, 'build', 'mimalloc_bootstrap.c');
const sljitSource = join(root, 'lib', 'sljit', 'sljit_src', 'sljitLir.c');
const sljitHeader = join(root, 'lib', 'sljit', 'sljit_src', 'sljitLir.h');
const nativeSource = join(root, 'src', 'jit', 'native.c');
const nativeHeader = join(root, 'include', 'tinypy', 'jit', 'native.h');
const inspectionOnly = cliArgs.some(arg =>
  arg === '--doctor' || arg === '--help' || arg === '-h' || arg === '--clean');

if (useMimalloc && !inspectionOnly) {
  for (const filename of [mimallocSource, mimallocHeader, mimallocBootstrap]) {
    if (!existsSync(filename)) {
      throw new Error(`Mimalloc integration requires ${filename}. ` +
        'Check lib/mimalloc and install build/mimalloc_bootstrap.c, or pass --allocator system.');
    }
  }
}

if (jit === 'on' && !inspectionOnly) {
  for (const filename of [sljitSource, sljitHeader, nativeSource, nativeHeader]) {
    if (!existsSync(filename)) {
      throw new Error(`Native JIT requires ${filename}. Check lib/sljit and the JIT installation.`);
    }
  }
}

const windows = process.platform === 'win32';
const linux = process.platform === 'linux';
const determinismFlags = windows ? [] : ['-ffp-contract=off', '-fno-fast-math'];
const baseBuildDir = interpreter === 'regvm' ? 'build-ts' : 'build-ts-stack';

const builder = new ProjectBuilder({
  rootDir: root,
  // Separate allocator/interpreter combinations for reproducible benchmarks.
  buildDir: `${useMimalloc ? `${baseBuildDir}-mimalloc` : baseBuildDir}${jit === 'on' ? '-jit' : ''}`, 
  toolchain: 'auto',
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
  },
  libraries: windows ? ['ws2_32'] : linux ? ['dl', 'm'] : ['m'],
});

if (jit === 'on') {
  // SLJIT is a library. Only compile its translation unit, never all its sources.
  builder.addStaticLibrary('sljit', {
    sources: ['lib/sljit/sljit_src/sljitLir.c'],
    includeDirs: ['lib/sljit/sljit_src'],
    pic: true,
  });
}

if (useMimalloc) {
  // Official supported single-source build: do not compile src/**/*.c here.
  builder.addStaticLibrary('mimalloc', {
    sources: ['lib/mimalloc/src/static.c'],
    includeDirs: ['lib/mimalloc/include'],
    defines: { MI_STATIC_LIB: 1 },
    // Mimalloc's native Windows dependencies; the builder propagates these
    // through static-library dependencies to the executable's final link.
    libraries: windows ? ['psapi', 'shell32', 'user32', 'advapi32', 'bcrypt'] : [],
    pic: true,
  });
}

const interpreterSources = sourceTree('src', {
  extensions: ['.c'],
  exclude: [
    '**/sljit/**', '**/jit/**', '**/jit_backend*.c',
    '**/sljit_*.c', '**/jit_codegen*.c', '**/regjit_backend*.c',
  ],
});

builder.addStaticLibrary('tinypy', {
  sources: [
    interpreterSources,
    ...(jit === 'on' ? [
      'src/jit/ir.c',
      'src/jit/reg_frontend.c',
      'src/jit/optimizer.c',
      'src/jit/lir.c',
      'src/jit/regalloc.c',
      'src/jit/escape.c',
      'src/jit/native.c',
    ] : []),
    ...(useMimalloc ? ['build/mimalloc_bootstrap.c'] : []),
  ],
  includeDirs: [
    'include',
    ...(useMimalloc ? ['lib/mimalloc/include'] : []),
    ...(jit === 'on' ? ['lib/sljit/sljit_src'] : []),
  ],
  defines: {
    PK_ENABLE_REGVM: interpreter === 'regvm' ? 1 : 0,
    PK_ENABLE_JIT: jit === 'on' ? 1 : 0,
  },
  dependencies: [...(useMimalloc ? ['mimalloc'] : []), ...(jit === 'on' ? ['sljit'] : [])],
  pic: true,
});

builder.addExecutable('tinypy-cli', {
  sources: ['examples/main.c'],
  includeDirs: ['include'],
  // Redirect only main.c's initialization call to the Mimalloc bootstrap.
  // The VM/library itself still exports and calls the real py_initialize().
  defines: useMimalloc ? { py_initialize: 'tinypy_initialize_mimalloc' } : {},
  dependencies: ['tinypy'],
  outputName: 'tinypy',
});

if (mirTest) {
  builder.addExecutable('tinypy-mir-test', {
    sources: ['tests/reg_frontend_test.c'],
    includeDirs: ['include'],
    dependencies: ['tinypy'],
    outputName: 'tinypy-mir-test',
  });
}

/*
 * One-shot PGO: instrumented build -> training run -> optimized build. The
 * training set is the regression suite (breadth: every opcode and module) plus
 * the benchmarks (depth: hot loops), so the profile reflects real dispatch.
 */
async function trainPgo(args: readonly string[]): Promise<void> {
  const options = parseCli(args);
  if (options === 'help' || options.doctor || options.clean) {
    throw new Error('--pgo-train cannot be combined with --help, --doctor or --clean.');
  }
  if (options.pgo) throw new Error('--pgo-train drives both PGO phases; do not pass --pgo as well.');
  const profile = options.profile === 'debug' || options.profile === undefined ? 'release' : options.profile;

  console.log('[pgo] 1/3 instrumented build');
  const generated = await builder.build({ ...options, profile, pgo: 'generate' });
  const instrumented = generated.outputs['tinypy-cli'];
  const clang = isClangDriver({ name: generated.toolchain });
  // GCC writes .gcda files next to the objects; Clang writes .profraw files here.
  const dataDir = clang
    ? resolve(root, options.pgoDir ?? join(generated.buildDir, 'pgo-data'))
    : generated.buildDir;
  const profileExt = clang ? '.profraw' : '.gcda';
  const profileFiles = async (): Promise<string[]> =>
    (await readdir(dataDir, { recursive: true })).filter(f => f.endsWith(profileExt)).map(f => join(dataDir, f));
  // Stale counters from a previous training run would be merged into this one.
  await mkdir(dataDir, { recursive: true });
  await Promise.all((await profileFiles()).map(file => rm(file, { force: true })));

  const workloads = [
    ...(await readdir(join(root, 'tests'))).filter(f => f.endsWith('.py')).map(f => `tests/${f}`),
    ...(await readdir(join(root, 'tests', 'jit'))).filter(f => f.endsWith('.py')).map(f => `tests/jit/${f}`),
    ...(await readdir(join(root, 'benchmarks'))).filter(f => f.endsWith('.py')).map(f => `benchmarks/${f}`),
    // Function-level hot paths (calls, methods, attributes, globals) - see the file header.
    'tools/pgo_workload.py',
  ].sort((a, b) => a.localeCompare(b));
  console.log(`[pgo] 2/3 training on ${workloads.length} scripts`);
  const env = { ...process.env };
  if (generated.runtimeBinDir) env.PATH = `${generated.runtimeBinDir}${delimiter}${env.PATH ?? ''}`;
  let failed = 0;
  const profilerErrors = new Set<string>();
  for (const script of workloads) {
    // Failures are fine for profiling (some tests need extra tools); only the counts matter.
    const run = spawnSync(instrumented, [script], {
      cwd: root, env, stdio: ['ignore', 'ignore', 'pipe'], encoding: 'utf8', timeout: 300_000,
    });
    if (run.status !== 0) failed++;
    // libgcov reports write failures as "profiling:<path>:<reason>" on stderr.
    for (const line of (run.stderr ?? '').split(/\r?\n/)) {
      if (line.startsWith('profiling:')) profilerErrors.add(line.replace(/^profiling:.*:/, ''));
    }
  }
  if (failed) console.log(`[pgo] ${failed} training script(s) exited non-zero (profile still recorded)`);
  const recorded = await profileFiles();
  if (profilerErrors.size || !recorded.length) {
    throw new Error(`PGO training recorded ${recorded.length} profile file(s) in ${dataDir}` +
      (profilerErrors.size ? `; profiler errors: ${[...profilerErrors].join(', ')}` : '') +
      '. Refusing to build with a missing profile.');
  }
  console.log(`[pgo] recorded ${recorded.length} profile file(s)`);

  if (clang) {
    const merge = spawnSync('llvm-profdata', ['merge', '-output', join(dataDir, CLANG_MERGED_PROFILE), ...recorded],
      { env, stdio: 'inherit' });
    if (merge.status !== 0) throw new Error('llvm-profdata merge failed (is it in PATH?).');
  }

  console.log('[pgo] 3/3 optimized build');
  const optimized = await builder.build({ ...options, profile, pgo: 'use' });
  console.log(`[pgo] done: ${optimized.outputs['tinypy-cli']}`);
}

const tinypyHelp =
  'TinyPy options:\n' +
  '  --interpreter MODE       regvm | stack (default: regvm)\n' +
  '  --allocator MODE         mimalloc | system (default: mimalloc)\n' +
  '  --jit MODE               on | off (default: off; requires regvm)\n' +
  '  --mir-test               build the standalone RegVM -> MIR C test\n' +
  '  --pgo-train              instrumented build, training run, then PGO build\n';

(pgoTrain
  ? trainPgo(cliArgs)
  : runBuildCli(builder, mirTest ? 'tinypy-mir-test' : 'tinypy-cli', cliArgs, tinypyHelp)
).catch(error => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
