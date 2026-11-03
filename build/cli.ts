import { delimiter } from 'node:path';
import ProjectBuilder, { type BuildOverrides, type LtoMode, type Profile, type ToolchainName } from './main-builder';
import { runProcess } from './src/process';
import { toolchainDoctor } from './src/toolchains';

export function usage(): string {
  return `Bruga Build: TypeScript-driven C/C++ builds

  bun run builder/pocketpy.build.ts [options] [-- program-args]

  --debug                  Unoptimized debug build (default)
  --release                Optimized build
  --relwithdebinfo         Optimized build with debug symbols
  --compiler NAME          auto | clang-mingw | mingw | msvc | clang-cl | clang | gcc
  --toolchain-dir PATH     Explicit toolchain bin directory or installation prefix
  --doctor                 Diagnose compiler discovery without compiling
  --jobs N                 Maximum parallel compile processes
  --lto MODE               off | thin | full
  --target NAME            Only build a selected target and its dependencies
  --rebuild                Recompile and relink everything
  --clean                  Remove builder-owned outputs and stop
  --dry-run                Display build commands without executing
  --verbose                Print compiler/linker commands
  --run                    Run the selected executable after successful build
  --help                   Show this help

  Windows auto mode: CLANG64 / GNU Clang -> MinGW-w64 -> MSVC -> clang-cl.
  Use --doctor even before installing any compilers.
`;
}

export interface CliOptions extends BuildOverrides {
  clean: boolean;
  run: boolean;
  doctor: boolean;
  programArgs: string[];
}

export function parseCli(argv: readonly string[]): CliOptions | 'help' {
  const result: {
    toolchain?: ToolchainName; toolchainDir?: string; profile?: Profile; lto?: LtoMode;
    jobs?: number; rebuild?: boolean; dryRun?: boolean; verbose?: boolean;
    only: string[]; clean: boolean; run: boolean; doctor: boolean; programArgs: string[];
  } = { only: [], clean: false, run: false, doctor: false, programArgs: [] };
  const requiredValue = (index: number, name: string): string => {
    const value = argv[index + 1];
    if (!value || value.startsWith('--')) throw new Error(`Missing value after ${name}`);
    return value;
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--') { result.programArgs = argv.slice(i + 1); break; }
    const split = arg.indexOf('=');
    const key = split >= 0 ? arg.slice(0, split) : arg;
    const inline = split >= 0 ? arg.slice(split + 1) : undefined;
    if (key === '--help' || key === '-h') return 'help';
    if (key === '--debug') result.profile = 'debug';
    else if (key === '--release') result.profile = 'release';
    else if (key === '--relwithdebinfo') result.profile = 'relwithdebinfo';
    else if (key === '--rebuild') result.rebuild = true;
    else if (key === '--dry-run') result.dryRun = true;
    else if (key === '--verbose') result.verbose = true;
    else if (key === '--clean') result.clean = true;
    else if (key === '--run') result.run = true;
    else if (key === '--doctor') result.doctor = true;
    else if (['--compiler', '--toolchain-dir', '--jobs', '--lto', '--target'].includes(key)) {
      const value = inline ?? requiredValue(i++, key);
      if (!value) throw new Error(`Empty value for ${key}`);
      if (key === '--compiler') {
        if (!['auto', 'clang-mingw', 'mingw', 'msvc', 'clang-cl', 'clang', 'gcc'].includes(value)) {
          throw new Error(`Invalid compiler: ${value}`);
        }
        result.toolchain = value as ToolchainName;
      } else if (key === '--toolchain-dir') result.toolchainDir = value;
      else if (key === '--jobs') {
        const jobs = Number(value);
        if (!Number.isSafeInteger(jobs) || jobs < 1) throw new Error(`Invalid jobs: ${value}`);
        result.jobs = jobs;
      } else if (key === '--lto') {
        if (!['off', 'thin', 'full'].includes(value)) throw new Error(`Invalid LTO mode: ${value}`);
        result.lto = value === 'off' ? false : value as LtoMode;
      } else result.only.push(value);
    } else throw new Error(`Unknown argument: ${arg}`);
  }
  return result;
}

/** Small opt-in CLI adapter. Project scripts may call builder.build() directly. */
export async function runBuildCli(builder: ProjectBuilder, defaultExecutable?: string): Promise<void> {
  const options = parseCli(process.argv.slice(2));
  if (options === 'help') { console.log(usage()); return; }
  if (options.doctor) { console.log(toolchainDoctor({ toolchainDir: options.toolchainDir })); return; }
  if (options.clean) { await builder.clean(); console.log('Build directory removed.'); return; }
  const result = await builder.build(options);
  if (options.run && !options.dryRun) {
    const selected = options.only ?? [];
    const name = selected.find(n => result.outputs[n] && n === defaultExecutable) ?? defaultExecutable ?? selected[0];
    const executable = name && result.outputs[name];
    if (!executable) throw new Error('Cannot select executable to run; specify --target NAME.');
    const env = { ...process.env };
    if (result.runtimeBinDir && process.platform === 'win32') {
      for (const key of Object.keys(env)) if (key.toLowerCase() === 'path') delete env[key];
      env.PATH = result.runtimeBinDir + delimiter + (process.env.PATH ?? process.env.Path ?? '');
    }
    const status = await runProcess([executable, ...options.programArgs], {
      cwd: builder.projectRoot(), env, printOutput: true,
    });
    if (status.code !== 0) process.exitCode = status.code;
  }
}
