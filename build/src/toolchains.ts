import { existsSync, statSync } from 'node:fs';
import { basename, delimiter, dirname, join, resolve } from 'node:path';
import { availableParallelism, cpus } from 'node:os';
import { spawnSync } from 'node:child_process';
import arch from '../arch';
import { stableHash } from '../extended';
import type { Architecture } from '../arch';
import type { ResolvedCompiler, ToolchainName } from './types';

export interface ToolchainSearchOptions {
  /** Absolute directory containing compiler binaries, or its installation prefix. */
  readonly toolchainDir?: string;
  /** Injectable for diagnostics and tests. The default is the current OS. */
  readonly platform?: NodeJS.Platform;
  readonly env?: NodeJS.ProcessEnv;
  /** Search prefixes; useful for portable installations and tests. */
  readonly extraRoots?: readonly string[];
}

export function defaultJobs(): number {
  const n = typeof availableParallelism === 'function' ? availableParallelism() : cpus().length;
  return Math.max(1, Math.min(n, 16));
}

function envValue(env: NodeJS.ProcessEnv, key: string): string | undefined {
  const actual = Object.keys(env).find(x => x.toLowerCase() === key.toLowerCase());
  return actual === undefined ? undefined : env[actual];
}

function envWithToolchain(original: NodeJS.ProcessEnv, bin: string, platform: NodeJS.Platform): NodeJS.ProcessEnv {
  const env = { ...original };
  for (const name of Object.keys(env)) if (name.toLowerCase() === 'path') delete env[name];
  env.PATH = [bin, envValue(original, 'PATH') ?? ''].filter(Boolean).join(platform === 'win32' ? ';' : delimiter);
  return env;
}

function isFile(filename: string): boolean {
  try { return statSync(filename).isFile(); } catch { return false; }
}

/** Locate tools in one directory only. Never combine two unrelated toolchain prefixes. */
function toolIn(bin: string, names: readonly string[], platform: NodeJS.Platform): string | undefined {
  for (const name of names) {
    const variants = platform === 'win32' && !name.toLowerCase().endsWith('.exe') ? [`${name}.exe`, name] : [name];
    for (const variant of variants) {
      const candidate = join(bin, variant);
      if (isFile(candidate)) return candidate;
    }
  }
  return undefined;
}

function pathDirectories(env: NodeJS.ProcessEnv, platform: NodeJS.Platform): string[] {
  // On Windows PATH entries are semicolon-separated; this remains correct when
  // the user launches Bun from PowerShell instead of an MSYS2 shell.
  return (envValue(env, 'PATH') ?? '').split(platform === 'win32' ? ';' : delimiter)
    .filter(Boolean).map(x => x.replace(/^"(.*)"$/, '$1'));
}

function candidateRoots(env: NodeJS.ProcessEnv): string[] {
  const roots = [
    env.MSYS2_ROOT, env.MSYS2_ROOT_DIR, env.MSYS2_HOME,
    env.MINGW_ROOT, env.MINGW_HOME, env.MINGW_PREFIX,
    env.LLVM_ROOT, env.LLVM_HOME, env.LLVM_DIR, env.LLVM_BIN,
    env.SCOOP ? join(env.SCOOP, 'apps', 'msys2', 'current') : undefined,
    env.USERPROFILE ? join(env.USERPROFILE, 'scoop', 'apps', 'msys2', 'current') : undefined,
    env.ChocolateyInstall ? join(env.ChocolateyInstall, 'lib', 'msys2', 'tools') : undefined,
    'C:\\msys64', 'C:\\msys2', 'D:\\msys64', 'C:\\tools\\msys64',
    'C:\\mingw64', 'C:\\tools\\mingw64', 'C:\\Program Files\\msys2',
  ];
  const programFiles = env['ProgramFiles'] ?? 'C:\\Program Files';
  roots.push(join(programFiles, 'LLVM'));
  return roots.filter((x): x is string => !!x);
}

/** Public for actionable --doctor diagnostics. */
export function searchDirectories(options: ToolchainSearchOptions = {}): string[] {
  const platform = options.platform ?? process.platform;
  const env = options.env ?? process.env;
  const dirs: string[] = [];
  const seen = new Set<string>();
  const add = (p: string): void => {
    if (!p) return;
    const v = resolve(p);
    const key = platform === 'win32' ? v.toLowerCase() : v;
    if (!seen.has(key)) { seen.add(key); dirs.push(v); }
  };
  const expandRoot = (p: string): void => {
    const base = basename(p).toLowerCase();
    if (base === 'bin') { add(p); return; }
    // Explicit CLANG64/UCRT64 prefixes should be used as-is, not silently
    // switched to a different C runtime or C++ standard library.
    if (['clang64', 'ucrt64', 'mingw64', 'clangarm64', 'mingw32'].includes(base)) { add(join(p, 'bin')); return; }
    for (const prefix of ['clang64', 'ucrt64', 'mingw64', 'clangarm64', 'mingw32']) add(join(p, prefix, 'bin'));
    add(join(p, 'bin'));
  };
  if (options.toolchainDir) {
    expandRoot(options.toolchainDir);
    return dirs;
  }
  // PATH is authoritative for users with an existing custom toolchain.
  for (const p of pathDirectories(env, platform)) add(p);
  if (platform === 'win32') {
    for (const root of [...(options.extraRoots ?? []), ...candidateRoots(env)]) expandRoot(root);
  } else {
    for (const root of options.extraRoots ?? []) expandRoot(root);
  }
  return dirs;
}

function probe(executable: string, args: readonly string[], env: NodeJS.ProcessEnv): string | undefined {
  try {
    const p = spawnSync(executable, [...args], {
      env, encoding: 'utf8', timeout: 6000, windowsHide: true, maxBuffer: 512 * 1024,
    });
    if (p.status !== 0) return undefined;
    return `${p.stdout ?? ''}\n${p.stderr ?? ''}`.trim();
  } catch { return undefined; }
}

function isWindowsGnuTriple(triple: string): boolean {
  return /(?:mingw|w64.*(?:mingw|windows)|windows-gnu)/i.test(triple);
}

function matchesHostTriple(triple: string, hostArch: Architecture): boolean {
  const cpu = triple.trim().split('-')[0].toLowerCase();
  if (hostArch === 'x64') return cpu === 'x86_64' || cpu === 'amd64';
  if (hostArch === 'arm64') return cpu === 'aarch64' || cpu === 'arm64';
  if (hostArch === 'x86') return /^(i[3-6]86|x86)$/.test(cpu);
  return true;
}

function compilerCandidate(
  name: 'clang-mingw' | 'mingw' | 'clang' | 'gcc',
  bin: string, platform: NodeJS.Platform, baseEnv: NodeJS.ProcessEnv, nativeArch: Architecture,
): ResolvedCompiler | undefined {
  const clang = name === 'clang' || name === 'clang-mingw';
  const cc = toolIn(bin, clang ? ['clang', 'x86_64-w64-mingw32-clang'] : ['gcc', 'x86_64-w64-mingw32-gcc'], platform);
  const cxx = toolIn(bin, clang ? ['clang++', 'x86_64-w64-mingw32-clang++'] : ['g++', 'x86_64-w64-mingw32-g++'], platform);
  const ar = toolIn(bin, clang ? ['llvm-ar', 'ar', 'x86_64-w64-mingw32-ar'] : ['gcc-ar', 'ar', 'x86_64-w64-mingw32-ar'], platform);
  if (!cc || !cxx || !ar) return undefined;
  const env = envWithToolchain(baseEnv, bin, platform);
  const triple = probe(cc, ['-dumpmachine'], env);
  if (!triple || (platform === 'win32' &&
    (!isWindowsGnuTriple(triple) || !matchesHostTriple(triple, nativeArch)))) return undefined;
  const version = probe(cc, ['--version'], env);
  if (!version) return undefined;
  const actualName = platform === 'win32' ? (clang ? 'clang-mingw' : 'mingw') : (clang ? 'clang' : 'gcc');
  return {
    name: actualName, family: 'gnu', cCompiler: cc, cxxCompiler: cxx, linker: cxx, librarian: ar,
    env, hostArch: nativeArch, runtimeBinDir: bin, targetTriple: triple.trim().split(/\s+/)[0],
    identity: stableHash(JSON.stringify({ cc, cxx, ar, version, triple, platform, bin })),
  };
}

function msvcEnvironment(hostArch: Architecture, env: NodeJS.ProcessEnv, platform: NodeJS.Platform): NodeJS.ProcessEnv | undefined {
  if (platform !== 'win32') return undefined;
  const pathDirs = pathDirectories(env, platform);
  if (pathDirs.some(d => toolIn(d, ['cl.exe'], platform) && toolIn(d, ['link.exe'], platform) && toolIn(d, ['lib.exe'], platform))) {
    return { ...env, VSLANG: '1033' };
  }
  const installRoot = env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)';
  const vswhere = join(installRoot, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
  if (!existsSync(vswhere)) return undefined;
  const path = probe(vswhere, [
    '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '-property', 'installationPath',
  ], env)?.trim();
  if (!path) return undefined;
  const vcvars = join(path, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat');
  if (!existsSync(vcvars)) return undefined;
  const target = hostArch === 'arm64' ? 'arm64' : hostArch === 'x86' ? 'x86' : 'x64';
  const cmd = `call "${vcvars.replace(/"/g, '""')}" ${target} >nul && set`;
  const p = spawnSync('cmd.exe', ['/d', '/s', '/c', cmd], {
    env: { ...env, VSCMD_SKIP_SENDTELEMETRY: '1' }, encoding: 'utf8',
    timeout: 15000, windowsHide: true, maxBuffer: 8 * 1024 * 1024,
  });
  if (p.status !== 0) return undefined;
  const out: NodeJS.ProcessEnv = { ...env, VSLANG: '1033' };
  for (const key of Object.keys(out)) if (key.toLowerCase() === 'path') delete out[key];
  for (const line of p.stdout.split(/\r?\n/)) {
    const pos = line.indexOf('=');
    if (pos > 0) out[line.slice(0, pos).toLowerCase() === 'path' ? 'PATH' : line.slice(0, pos)] = line.slice(pos + 1);
  }
  return out;
}

function msvcCandidate(
  kind: 'msvc' | 'clang-cl', env: NodeJS.ProcessEnv,
  platform: NodeJS.Platform, nativeArch: Architecture,
): ResolvedCompiler | undefined {
  const dirs = pathDirectories(env, platform);
  const find = (names: readonly string[]): string | undefined => {
    for (const bin of dirs) { const p = toolIn(bin, names, platform); if (p) return p; }
    return undefined;
  };
  const cc = find(kind === 'msvc' ? ['cl.exe'] : ['clang-cl.exe']);
  const linker = find(kind === 'msvc' ? ['link.exe'] : ['lld-link.exe', 'link.exe']);
  const ar = find(kind === 'msvc' ? ['lib.exe'] : ['llvm-lib.exe', 'lib.exe']);
  if (!cc || !linker || !ar) return undefined;
  const version = probe(cc, kind === 'msvc' ? ['/Bv'] : ['--version'], env);
  if (!version) return undefined;
  return {
    name: kind, family: 'msvc', cCompiler: cc, cxxCompiler: cc, linker, librarian: ar,
    env, hostArch: nativeArch, runtimeBinDir: dirname(cc),
    identity: stableHash(JSON.stringify({ cc, linker, ar, version, include: env.INCLUDE, lib: env.LIB })),
  };
}

function resolvedFor(requested: ToolchainName, options: ToolchainSearchOptions): ResolvedCompiler | undefined {
  const platform = options.platform ?? process.platform;
  const env = options.env ?? process.env;
  const nativeArch = arch();
  const dirs = searchDirectories(options);
  const tryGnu = (name: 'clang-mingw' | 'mingw' | 'clang' | 'gcc'): ResolvedCompiler | undefined => {
    for (const bin of dirs) {
      const result = compilerCandidate(name, bin, platform, env, nativeArch);
      if (result) return result;
    }
    return undefined;
  };
  const tryMsvc = (name: 'msvc' | 'clang-cl'): ResolvedCompiler | undefined => {
    const configured = msvcEnvironment(nativeArch, env, platform);
    return configured && msvcCandidate(name, configured, platform, nativeArch);
  };
  switch (requested) {
    case 'clang-mingw': return platform === 'win32' ? tryGnu('clang-mingw') : undefined;
    case 'mingw': return platform === 'win32' ? tryGnu('mingw') : undefined;
    case 'clang': return tryGnu(platform === 'win32' ? 'clang-mingw' : 'clang');
    case 'gcc': return tryGnu(platform === 'win32' ? 'mingw' : 'gcc');
    case 'msvc': return tryMsvc('msvc');
    case 'clang-cl': return tryMsvc('clang-cl');
    case 'auto':
      return platform === 'win32'
        ? tryGnu('clang-mingw') ?? tryGnu('mingw') ?? tryMsvc('msvc') ?? tryMsvc('clang-cl')
        : tryGnu('clang') ?? tryGnu('gcc');
  }
}

export function toolchainDoctor(options: ToolchainSearchOptions = {}): string {
  const platform = options.platform ?? process.platform;
  const env = options.env ?? process.env;
  const names: ToolchainName[] = platform === 'win32'
    ? ['clang-mingw', 'mingw', 'msvc', 'clang-cl'] : ['clang', 'gcc'];
  const lines = [
    `Bruga Build toolchain doctor (${platform}, ${arch()})`,
    `PATH entries: ${pathDirectories(env, platform).length}`,
    `Custom toolchain: ${options.toolchainDir ?? '(auto-search)'}`,
  ];
  for (const name of names) {
    const tc = resolvedFor(name, options);
    lines.push(tc ? `  [OK]   ${name.padEnd(12)} ${tc.cCompiler} (${tc.targetTriple ?? 'MSVC ABI'})`
      : `  [MISS] ${name}`);
  }
  if (platform === 'win32') {
    lines.push('', 'Discovered candidate directories (directory existence / executable files):');
    for (const bin of searchDirectories(options).slice(0, 80)) {
      if (!existsSync(bin)) continue;
      const have = ['clang', 'clang++', 'gcc', 'g++', 'cl.exe', 'ar', 'llvm-ar']
        .filter(t => toolIn(bin, [t], platform)).join(', ');
      if (have) lines.push(`  ${bin}  [${have}]`);
    }
    lines.push('', 'If no GNU toolchain is found, install MSYS2 CLANG64 or UCRT64:',
      '  CLANG64: pacman -S mingw-w64-clang-x86_64-clang',
      '  UCRT64:  pacman -S mingw-w64-ucrt-x86_64-gcc',
      '  bun run builder/tinypy.build.ts --release --compiler clang-mingw --toolchain-dir "C:\\msys64\\clang64\\bin"',
      '  bun run builder/tinypy.build.ts --release --compiler mingw --toolchain-dir "C:\\msys64\\ucrt64\\bin"',
      'A standalone LLVM Windows-MSVC-targeted Clang is NOT a complete MinGW toolchain.',
      'Use MSYS2 CLANG64 or provide a complete GNU-targeted Clang + MinGW sysroot.',
    );
  }
  return lines.join('\n');
}

export function detectToolchain(requested: ToolchainName, options: ToolchainSearchOptions = {}): ResolvedCompiler {
  const result = resolvedFor(requested, options);
  if (result) return result;
  throw new Error(`Cannot find a complete ${requested} toolchain.\n\n${toolchainDoctor(options)}`);
}
