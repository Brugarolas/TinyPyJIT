/**
 * TinyPy project definition for Bruga Build.
 *
 * Commands (from the project root):
 *   bun run build/tinypy.build.ts --release --compiler mingw --allocator mimalloc
 *   bun run build/tinypy.build.ts --release --compiler mingw --allocator system
 *
 * All C sources are compiled directly; CMake and shell scripts are not used.
 */
import ProjectBuilder, { sourceTree } from './main-builder';
import { runBuildCli } from './cli';
import { existsSync } from 'node:fs';
import { join, resolve } from 'node:path';

type InterpreterMode = 'regvm' | 'stack';
type AllocatorMode = 'mimalloc' | 'system';

interface TinyPyOptions {
  readonly interpreter: InterpreterMode;
  readonly allocator: AllocatorMode;
  readonly cliArgs: string[];
}

/** Read project-specific arguments without interfering with Bruga's CLI parser. */
function selectTinyPyOptions(argv: readonly string[]): TinyPyOptions {
  let interpreter: InterpreterMode = 'regvm';
  let allocator: AllocatorMode = 'mimalloc';
  let interpreterSelected = false;
  let allocatorSelected = false;
  const cliArgs: string[] = [];

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--') {
      cliArgs.push(...argv.slice(i));
      break;
    }

    const equal = arg.indexOf('=');
    const option = equal === -1 ? arg : arg.slice(0, equal);
    if (option !== '--interpreter' && option !== '--allocator') {
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
    } else {
      if (allocatorSelected) throw new Error('Specify --allocator only once.');
      if (value !== 'mimalloc' && value !== 'system') {
        throw new Error(`Invalid allocator ${JSON.stringify(value)}. Expected mimalloc or system.`);
      }
      allocator = value;
      allocatorSelected = true;
    }
  }

  return { interpreter, allocator, cliArgs };
}

const { interpreter, allocator, cliArgs } = selectTinyPyOptions(process.argv.slice(2));

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

const windows = process.platform === 'win32';
const linux = process.platform === 'linux';
const determinismFlags = windows ? [] : ['-ffp-contract=off', '-fno-fast-math'];
const baseBuildDir = interpreter === 'regvm' ? 'build-ts' : 'build-ts-stack';

const builder = new ProjectBuilder({
  rootDir: root,
  // Separate allocator/interpreter combinations for reproducible benchmarks.
  buildDir: useMimalloc ? `${baseBuildDir}-mimalloc` : baseBuildDir,
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

await builder.addFolderRecursivelyToProject('lib/sljit/sljit_src');

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
    ...(useMimalloc ? ['build/mimalloc_bootstrap.c'] : []),
  ],
  includeDirs: [
    'include',
    ...(useMimalloc ? ['lib/mimalloc/include'] : []),
  ],
  defines: {
    PK_ENABLE_REGVM: interpreter === 'regvm' ? 1 : 0,
  },
  dependencies: useMimalloc ? ['mimalloc'] : [],
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

runBuildCli(
  builder,
  'tinypy-cli',
  cliArgs,
  'TinyPy options:\n' +
  '  --interpreter MODE       regvm | stack (default: regvm)\n' +
  '  --allocator MODE         mimalloc | system (default: mimalloc)\n',
).catch(error => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
