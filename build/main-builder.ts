/** Clank Builder: project configuration is ordinary TypeScript, not a second DSL. */
import { readFile, writeFile, mkdir, rm, rename, lstat } from 'node:fs/promises';
import { basename, dirname, extname, isAbsolute, join, relative, resolve, sep } from 'node:path';
import { performance } from 'node:perf_hooks';
import { stableHash } from './extended.ts';
import { existsSync } from 'node:fs';
import { detectToolchain, defaultJobs, parseZigTarget } from './src/toolchains.ts';
import { parseDepfile, parseMsvcIncludes, ContentFingerprints, scanHeaders, validFile } from './src/depfile.ts';
import { displayCommand, runPool, runProcess } from './src/process.ts';
import { discoverSources, isSubPath, validateName } from './src/sources.ts';
import type { BuildOverrides, BuildResult, Definitions, LtoMode, Profile, ProjectSettings, ResolvedCompiler, SourceInput, Target, BuildTask, TaskContext, GitCheckoutOptions, CrossTargetOptions, CrossTargetOverride } from './src/types.ts';

export { sourceTree } from './src/types.ts';
export type { SourceTree, SourceInput, Target, TargetKind, ProjectSettings, BuildOverrides, BuildResult, ToolchainName, Profile, LtoMode, Definitions, BuildTask, TaskContext, GitCheckoutOptions, CrossTargetOptions, CrossTargetOverride } from './src/types.ts';

interface CachedUnit { fingerprint: string; dependencies: string[]; }
interface CacheFile { version: 2; units: Record<string, CachedUnit>; links: Record<string, string>; tasks: Record<string, string>; }
interface BuildUnit { source: string; object: string; depfile: string; args: string[]; includes: string[]; }
interface BuildTargetData { target: Target; sources: string[]; objects: string[]; output: string; }

const EMPTY_CACHE = (): CacheFile => ({ version: 2, units: {}, links: {}, tasks: {} });

function asDefinitions(definitions: Definitions | undefined, isMsvc: boolean): string[] {
  const result: string[] = [];
  for (const [key, value] of Object.entries(definitions ?? {}).sort(([a], [b]) => a.localeCompare(b))) {
    if (!/^[A-Za-z_][A-Za-z_0-9]*$/.test(key)) throw new Error(`Invalid C macro name: ${key}`);
    const rhs = value === null ? '' : `=${typeof value === 'boolean' ? Number(value) : String(value)}`;
    result.push(`${isMsvc ? '/D' : '-D'}${key}${rhs}`);
  }
  return result;
}

function compileKind(source: string): 'c' | 'cxx' | 'asm' {
  const extension = extname(source);
  if (['.cc', '.cpp', '.cxx', '.mm'].includes(extension)) return 'cxx';
  if (extension === '.S' || extension === '.s') return 'asm';
  return 'c';
}

function ltoFlags(lto: LtoMode, tc: ResolvedCompiler): string[] {
  if (!lto) return [];
  if (tc.family === 'msvc') return ['/GL'];
  return (tc.name === 'zig' || tc.name === 'clang' || tc.name === 'clang-mingw') ? [`-flto=${lto}`] : ['-flto'];
}

function compileOptions(tc: ResolvedCompiler, profile: Profile, lto: LtoMode, kind: 'c' | 'cxx' | 'asm', pic: boolean, threads: boolean): string[] {
  if (tc.family === 'msvc') {
    const flags = ['/nologo', '/utf-8', '/W3', '/FS', '/showIncludes'];
    if (kind === 'c') flags.push('/std:c11', '/experimental:c11atomics', '/TC');
    else if (kind === 'cxx') flags.push('/std:c++20', '/TP', '/EHsc');
    else throw new Error('MSVC cannot compile .s/.S directly; use MASM with an explicit build step.');
    if (profile === 'debug') flags.push('/Od', '/Z7', '/MDd', '/D_DEBUG');
    else {
      flags.push('/O2', '/DNDEBUG', '/MD');
      if (profile === 'relwithdebinfo') flags.push('/Z7');
      flags.push(...ltoFlags(lto, tc));
    }
    return flags;
  }
  const flags = kind === 'c' ? ['-std=c11'] : kind === 'cxx' ? ['-std=c++20'] : [];
  flags.push('-Wall', '-Wextra');
  if (profile === 'debug') flags.push('-O0', '-g3');
  else {
    flags.push('-O3', '-DNDEBUG');
    if (profile === 'relwithdebinfo') flags.push('-g');
    flags.push(...ltoFlags(lto, tc));
  }
  if (pic) flags.push('-fPIC');
  if (threads && tc.targetPlatform !== 'win32') flags.push('-pthread');
  return flags;
}

function outputPath(dir: string, target: Target, tc: ResolvedCompiler): string {
  const stem = validateName(target.outputName ?? target.name);
  const platform = tc.targetPlatform ?? process.platform;
  if (target.kind === 'executable') return join(dir, 'bin', stem + (platform === 'win32' ? '.exe' : ''));
  if (target.kind === 'shared') {
    return join(dir, platform === 'win32' ? 'bin' : 'lib', platform === 'win32' ? `${stem}.dll` : platform === 'darwin' ? `lib${stem}.dylib` : `lib${stem}.so`);
  }
  return join(dir, 'lib', tc.family === 'msvc' ? `${stem}.lib` : `lib${stem}.a`);
}

async function loadCache(path: string): Promise<CacheFile> {
  try {
    const json: unknown = JSON.parse(await readFile(path, 'utf8'));
    if (json && typeof json === 'object' && (json as CacheFile).version === 2 && (json as CacheFile).units && (json as CacheFile).links && (json as CacheFile).tasks) {
      return json as CacheFile;
    }
  } catch { /* A corrupt cache only causes a rebuild. */ }
  return EMPTY_CACHE();
}

async function writeAtomic(path: string, value: string): Promise<void> {
  await mkdir(dirname(path), { recursive: true });
  const tmp = `${path}.${process.pid}.tmp`;
  await writeFile(tmp, value, 'utf8');
  await rename(tmp, path);
}

function systemLib(value: string, tc: ResolvedCompiler): string[] {
  if (value.startsWith('-') || isAbsolute(value) || /\.(?:a|so|dylib|lib|o|obj)$/i.test(value)) return [value];
  return tc.family === 'msvc' ? [`${value}.lib`] : [`-l${value}`];
}

export default class ProjectBuilder {
  private readonly root: string;
  private readonly outputBase: string;
  private readonly settings: ProjectSettings;
  private readonly targets = new Map<string, Target>();
  private readonly tasks = new Map<string, BuildTask>();
  private readonly crossTargets = new Map<string, CrossTargetOptions>();
  private readonly legacySources: SourceInput[] = [];
  private legacyName = 'app';
  private legacyIcon?: string;

  constructor(settings: ProjectSettings = {}) {
    this.settings = { ...settings };
    this.root = resolve(settings.rootDir ?? process.cwd());
    this.outputBase = resolve(this.root, settings.buildDir ?? 'build-ts');
    if (!isSubPath(this.root, this.outputBase)) {
      throw new Error('Build directory must be a child of the project root, not the root itself.');
    }
    const first = relative(this.root, this.outputBase).split(sep)[0].toLowerCase();
    if (new Set(['src', 'examples', 'include', 'lib', 'tests', 'tools', 'builder', 'node_modules', '.git', '.hg']).has(first)) {
      throw new Error(`Refusing a build output directory inside protected source tree: ${this.outputBase}`);
    }
  }

  /** Return the canonical project root for tool invocation and run scripts. */
  projectRoot(): string { return this.root; }

  /** Register an arbitrary TypeScript build task, such as code generation. */
  addTask(task: BuildTask): this {
    validateName(task.name);
    if (this.tasks.has(task.name) || this.targets.has(task.name)) throw new Error(`Duplicate task name: ${task.name}`);
    if ((task.outputs ?? []).some(x => !isSubPath(this.root, resolve(this.root, x)))) {
      throw new Error(`Task ${task.name}: outputs must be inside the project`);
    }
    this.tasks.set(task.name, task);
    return this;
  }

  /**
   * Check out a pinned Git dependency before compiling targets which require it.
   * No Git hooks or shell commands are executed by the builder. A complete
   * commit SHA is mandatory so a moved tag cannot change the source silently.
   */
  addGitCheckout(options: GitCheckoutOptions): this {
    const name = validateName(options.name);
    const directory = resolve(this.root, options.directory);
    const commit = options.commit.toLowerCase();
    if (!/^[0-9a-f]{40}$/.test(commit)) throw new Error(`Git checkout ${name} requires a 40-character commit SHA`);
    if (!options.repository.trim() || !options.ref.trim() || options.ref.startsWith('-')) {
      throw new Error(`Git checkout ${name}: invalid repository or ref`);
    }
    if (!isSubPath(this.root, directory) || directory === this.outputBase || isSubPath(this.outputBase, directory)) {
      throw new Error(`Git checkout ${name}: destination must be inside the project and outside the build directory`);
    }
    const marker = join(directory, '.clank-checkout.json');
    const fingerprint = stableHash(JSON.stringify({ repository: options.repository, ref: options.ref, commit }));
    const verify = async (ctx: TaskContext): Promise<void> => {
      const actual = (await runProcess(['git', '-C', directory, 'rev-parse', 'HEAD'], {
        cwd: ctx.rootDir, env: ctx.env,
      })).stdout.trim().toLowerCase();
      if (actual !== commit) {
        throw new Error(`Git checkout ${name} is at ${actual}; expected ${commit}. Remove ${directory} to restore the pinned version.`);
      }
      const status = (await runProcess(['git', '-C', directory, 'status', '--porcelain', '--untracked-files=all'], {
        cwd: ctx.rootDir, env: ctx.env,
      })).stdout;
      const changes = status.split(/\r?\n/).filter(line =>
        line.length > 0 && line !== '?? .clank-checkout.json');
      if (changes.length) {
        throw new Error(`Git checkout ${name} has modified or untracked files: ${directory}\n${changes.join('\n')}`);
      }
      const recorded = JSON.parse(await readFile(marker, 'utf8')) as { fingerprint?: string };
      if (recorded.fingerprint !== fingerprint) {
        throw new Error(`Git checkout ${name} has a stale source marker. Remove ${directory} to refresh it.`);
      }
    };
    return this.addTask({
      name,
      version: fingerprint,
      outputs: [relative(this.root, marker)],
      verify,
      action: async ctx => {
        let exists = false;
        try {
          const info = await lstat(directory);
          if (!info.isDirectory() || info.isSymbolicLink()) throw new Error(`Unsafe Git destination: ${directory}`);
          exists = true;
        } catch (error) {
          if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error;
        }
        if (exists) {
          if (!existsSync(join(directory, '.git'))) {
            throw new Error(`Cannot reuse a non-Git directory: ${directory}`);
          }
          const actual = (await runProcess(['git', '-C', directory, 'rev-parse', 'HEAD'], {
            cwd: ctx.rootDir, env: ctx.env,
          })).stdout.trim().toLowerCase();
          if (actual !== commit) {
            throw new Error(`Git checkout ${name} is at ${actual}; expected ${commit}. Remove ${directory} to update it.`);
          }
        } else {
          await mkdir(dirname(directory), { recursive: true });
          try {
            await runProcess(['git', 'clone', '--depth', '1', '--branch', options.ref, '--', options.repository, directory], {
              cwd: ctx.rootDir, env: ctx.env, printOutput: true,
            });
          } catch (error) {
            await rm(directory, { recursive: true, force: true });
            throw error;
          }
          const actual = (await runProcess(['git', '-C', directory, 'rev-parse', 'HEAD'], {
            cwd: ctx.rootDir, env: ctx.env,
          })).stdout.trim().toLowerCase();
          if (actual !== commit) {
            await rm(directory, { recursive: true, force: true });
            throw new Error(`Git checkout ${name}: ${options.ref} resolved to ${actual}, expected ${commit}`);
          }
        }
        await writeAtomic(marker, JSON.stringify({ repository: options.repository, ref: options.ref, commit, fingerprint }) + '\n');
        await verify(ctx);
      },
    });
  }

  /** Register a platform build in TypeScript, not through CLI arguments.
   * Each registered triple produces its own independent build and cache.
   */
  addCrossTarget(triple: string, options: CrossTargetOptions = {}): this {
    const parsed = parseZigTarget(triple);
    const name = parsed.triple ?? 'native';
    if (this.crossTargets.has(name)) throw new Error(`Duplicate cross target: ${name}`);
    for (const [targetName, overrides] of Object.entries(options.targets ?? {})) {
      validateName(targetName);
      if (overrides.sources?.length === 0) throw new Error(`Cross target ${name}: ${targetName} has no sources`);
    }
    this.crossTargets.set(name, options);
    return this;
  }

  /** True when build() will produce the registered platform matrix. */
  hasCrossTargets(): boolean { return this.crossTargets.size !== 0; }

  /** Define independently buildable C/C++ targets; dependencies form a DAG. */
  addTarget(target: Target): this {
    const name = validateName(target.name);
    if (this.targets.has(name) || this.tasks.has(name)) throw new Error(`Duplicate build target: ${name}`);
    if (target.sources.length === 0) throw new Error(`Target ${name} has no sources`);
    this.targets.set(name, { ...target });
    return this;
  }
  addExecutable(name: string, options: Omit<Target, 'name' | 'kind'>): this { return this.addTarget({ name, kind: 'executable', ...options }); }
  addStaticLibrary(name: string, options: Omit<Target, 'name' | 'kind'>): this { return this.addTarget({ name, kind: 'static', ...options }); }
  addSharedLibrary(name: string, options: Omit<Target, 'name' | 'kind'>): this { return this.addTarget({ name, kind: 'shared', ...options }); }

  /** Compatibility with the original script's one-executable API. */
  addFolderRecursivelyToProject(folderPath: string): Promise<void> {
    this.legacySources.push({ kind: 'source-tree', directory: folderPath,
      extensions: ['.c', '.h','.cc', '.cpp', '.cxx', '.hpp', '.s', '.S', '.m', '.mm'], exclude: [] });
    return Promise.resolve();
  }
  addFileToProject(file: string): void { this.legacySources.push(file); }
  setGeneralCompilerFlags(flags: string[]): void { this.settings.commonFlags = [...flags]; }
  setCppCompilerFlags(flags: string[]): void { this.settings.cxxFlags = [...flags]; }
  setLinkerFlags(flags: string[]): void { this.settings.linkFlags = [...flags]; }
  setCompilerMacros(macros: string[]): void {
    this.settings.defines = Object.fromEntries(macros.map(x => { const index = x.indexOf('='); return index < 0 ? [x, null] : [x.slice(0, index), x.slice(index + 1)]; }));
  }
  setExecutableData(name: string, icon?: string): void { this.legacyName = validateName(name); this.legacyIcon = icon; }
  setDirectoryPath(folder: string): void {
    throw new Error(`setDirectoryPath(${folder}) is replaced by the constructor's buildDir option.`);
  }
  setForceCompiledHeaderFiles(files: string[]): void {
    if (files.length) throw new Error('Do not compile headers as objects. Use includeDirs and dependency tracking.');
  }
  setMainExecutableFile(filePath: string): void { this.addFileToProject(filePath); }
  setUseClangCppAsDefault(_enabled: boolean): void { /* The driver is selected per source language automatically. */ }

  private graph(only: readonly string[] | undefined): Target[] {
    const resolved: Target[] = [];
    const state = new Map<string, 'visiting' | 'done'>();
    const visit = (name: string): void => {
      const target = this.targets.get(name);
      if (!target) throw new Error(`Unknown target or dependency: ${name}`);
      if (state.get(name) === 'visiting') throw new Error(`Cyclic target dependency at ${name}`);
      if (state.get(name) === 'done') return;
      state.set(name, 'visiting');
      for (const dep of target.dependencies ?? []) visit(dep);
      state.set(name, 'done');
      resolved.push(target);
    };
    for (const name of only?.length ? only : this.targets.keys()) visit(name);
    return resolved;
  }

  private makeUnits(target: Target, sources: readonly string[], dir: string, tc: ResolvedCompiler, profile: Profile, lto: LtoMode): BuildUnit[] {
    const includePaths = [...(this.settings.includeDirs ?? []), ...(target.includeDirs ?? [])].map(p => resolve(this.root, p));
    const defines = { ...this.settings.defines, ...target.defines };
    const isMsvc = tc.family === 'msvc';
    return sources.map(source => {
      const kind = compileKind(source);
      const hash = stableHash(source).slice(0, 12);
      const object = join(dir, 'obj', target.name, `${basename(source).replace(/[^\w.-]/g, '_')}-${hash}.${isMsvc ? 'obj' : 'o'}`);
      const depfile = object + '.d';
      const compiler = kind === 'cxx' ? tc.cxxCompiler : tc.cCompiler;
      const flags = [
        ...compileOptions(tc, profile, lto, kind, target.pic ?? target.kind === 'shared', target.threads ?? this.settings.threads ?? false),
        ...asDefinitions(defines, isMsvc),
        ...includePaths.map(p => isMsvc ? `/I${p}` : `-I${p}`),
        ...(this.settings.commonFlags ?? []),
        ...(kind === 'cxx' ? [...(this.settings.cxxFlags ?? []), ...(target.cxxFlags ?? [])]
          : kind === 'asm' ? [...(this.settings.asmFlags ?? []), ...(target.asmFlags ?? [])]
            : [...(this.settings.cFlags ?? []), ...(target.cFlags ?? [])]),
      ];
      const driverArgs = kind === 'cxx' ? (tc.cxxCompilerArgs ?? []) : (tc.cCompilerArgs ?? []);
      const args = isMsvc
        ? [compiler, ...driverArgs, ...flags, '/c', source, `/Fo${object}`]
        : [compiler, ...driverArgs, ...flags, '-MMD', '-MF', depfile, '-MT', object, '-c', source, '-o', object];
      return { source, object, depfile, args, includes: includePaths };
    });
  }

  private linkArgs(target: Target, units: BuildUnit[], targetData: Map<string, BuildTargetData>, dir: string, tc: ResolvedCompiler, profile: Profile, lto: LtoMode): string[] {
    const output = outputPath(dir, target, tc);
    if (target.kind === 'static') {
      return tc.family === 'msvc'
        ? [tc.librarian, '/NOLOGO', `/OUT:${output}`, ...units.map(x => x.object)]
        : [tc.librarian, ...(tc.librarianArgs ?? []), 'rcs', output, ...units.map(x => x.object)];
    }
    const dependencyOutputs: string[] = [];
    const seen = new Set<string>();
    const addDep = (name: string): void => {
      if (seen.has(name)) return;
      seen.add(name);
      const data = targetData.get(name);
      if (!data) throw new Error(`Unbuilt target dependency: ${name}`);
      if (data.target.kind === 'executable') throw new Error(`Cannot link an executable as a dependency: ${name}`);
      dependencyOutputs.push(data.target.kind === 'shared' && (tc.targetPlatform ?? process.platform) === 'win32'
        ? join(dir, 'lib', validateName(data.target.outputName ?? data.target.name) + (tc.family === 'msvc' ? '.lib' : '.dll.a'))
        : data.output);
      for (const child of data.target.dependencies ?? []) addDep(child);
    };
    for (const name of target.dependencies ?? []) addDep(name);
    const libs = new Set([...(this.settings.libraries ?? []), ...(target.libraries ?? [])]);
    const addLibraries = (name: string): void => {
      const dep = targetData.get(name)?.target;
      if (!dep) return;
      for (const lib of dep.libraries ?? []) libs.add(lib);
      for (const sub of dep.dependencies ?? []) addLibraries(sub);
    };
    for (const name of target.dependencies ?? []) addLibraries(name);
    const threadEnabled = target.threads ?? this.settings.threads ?? false;
    const isCxx = units.some(x => compileKind(x.source) === 'cxx') || [...seen].some(n => targetData.get(n)?.sources.some(s => compileKind(s) === 'cxx'));
    if (tc.family === 'msvc') {
      const flags = ['/NOLOGO', `/OUT:${output}`, ...units.map(x => x.object), ...dependencyOutputs,
        ...[...libs].flatMap(l => systemLib(l, tc)), ...(this.settings.linkFlags ?? []), ...(target.linkFlags ?? [])];
      if (target.kind === 'shared') flags.push('/DLL', `/IMPLIB:${join(dir, 'lib', validateName(target.outputName ?? target.name) + '.lib')}`);
      if (profile !== 'release') flags.push('/DEBUG');
      if (lto && profile !== 'debug') flags.push('/LTCG');
      return [tc.linker, ...flags];
    }
    const driver = isCxx ? tc.cxxCompiler : tc.cCompiler;
    const dependentShared = [...seen].some(n => targetData.get(n)?.target.kind === 'shared');
    const platform = tc.targetPlatform ?? process.platform;
    const rpath = dependentShared && platform !== 'win32'
      ? [platform === 'darwin' ? `-Wl,-rpath,${target.kind === 'shared' ? '@loader_path' : '@loader_path/../lib'}`
        : `-Wl,-rpath,${target.kind === 'shared' ? '$ORIGIN' : '$ORIGIN/../lib'}`] : [];
    return [driver, ...(isCxx ? (tc.cxxCompilerArgs ?? []) : (tc.cCompilerArgs ?? [])), ...units.map(x => x.object), ...dependencyOutputs,
      ...[...libs].flatMap(l => systemLib(l, tc)),
      ...((target.kind === 'shared') ? [platform === 'darwin' ? '-dynamiclib' : '-shared'] : []),
      ...(threadEnabled && platform !== 'win32' ? ['-pthread'] : []), ...ltoFlags(lto, tc), ...(tc.name === 'clang' && lto && platform === 'linux' ? ['-fuse-ld=lld'] : []), ...rpath,
      ...(target.kind === 'shared' && (tc.targetPlatform ?? process.platform) === 'win32' ? [`-Wl,--out-implib,${join(dir, 'lib', validateName(target.outputName ?? target.name) + '.dll.a')}`] : []),
      ...(this.settings.linkFlags ?? []), ...(target.linkFlags ?? []), '-o', output];
  }

  private async responseCommand(command: readonly string[], dir: string, target: Target, tc: ResolvedCompiler): Promise<string[]> {
    const threshold = this.settings.responseFileThreshold ?? (process.platform === 'win32' ? 12000 : 32000);
    if (command.join(' ').length < threshold) return [...command];
    const filename = join(dir, 'rsp', `${target.name}.rsp`);
    await mkdir(dirname(filename), { recursive: true });
    const quote = (value: string): string => {
      if (!/[\s"']/u.test(value)) return value;
      if (process.platform === 'win32') {
        return `"${value.replace(/(\\*)"/g, (_full, slashes: string) => slashes + slashes + '\\"').replace(/(\\+)$/g, '$1$1')}"`;
      }
      return JSON.stringify(value);
    };
    // Zig is a multi-call driver: cc/c++/ar must remain before the response file.
    // A response like `zig @args.rsp` would interpret @args.rsp as a Zig command.
    const prefixLength = tc.name === 'zig' && ['cc', 'c++', 'ar'].includes(command[1]) ? 2 : 1;
    const content = command.slice(prefixLength).map(quote).join('\n') + '\n';
    if (process.platform === 'win32' && tc.family === 'msvc') {
      // MSVC link.exe and lib.exe accept UTF-16LE response files. GNU uses UTF-8.
      await writeFile(filename, Buffer.concat([Buffer.from([0xff, 0xfe]), Buffer.from(content, 'utf16le')]));
    } else await writeFile(filename, content, 'utf8');
    return [...command.slice(0, prefixLength), `@${filename}`];
  }

  async clean(): Promise<void> {
    if (!isSubPath(this.root, this.outputBase)) throw new Error('Unsafe clean path');
    if (!(await validFile(join(this.outputBase, '.bruga-build.json')))) {
      if (!existsSync(this.outputBase)) return;
      throw new Error(`Refusing to clean non-Bruga directory: ${this.outputBase}`);
    }
    const marker: unknown = JSON.parse(await readFile(join(this.outputBase, '.bruga-build.json'), 'utf8'));
    if (!marker || typeof marker !== 'object' || (marker as { root?: string }).root !== this.root) {
      throw new Error(`Refusing to clean a build directory belonging to another project: ${this.outputBase}`);
    }
    await rm(this.outputBase, { recursive: true, force: true });
  }

  /** Build all registered platform targets, or a normal native build when none exist. */
  async build(options: BuildOverrides = {}): Promise<BuildResult> {
    if (!this.crossTargets.size) return await this.buildOne(options);
    if (options.toolchain && options.toolchain !== 'zig' && options.toolchain !== 'auto') {
      throw new Error('Registered cross targets require Zig; remove --compiler or choose zig.');
    }
    const started = performance.now();
    const results: Record<string, BuildResult> = Object.create(null) as Record<string, BuildResult>;
    const outputs: Record<string, string> = {};
    let compiled = 0, reused = 0, linked = 0;
    for (const [triple, configuration] of this.crossTargets) {
      console.log(`[cross] ${triple}`);
      const result = await this.buildOne({ ...options, toolchain: 'zig' }, triple, configuration);
      results[triple] = result;
      compiled += result.compiled;
      reused += result.reused;
      linked += result.linked;
      for (const [name, path] of Object.entries(result.outputs)) outputs[`${triple}:${name}`] = path;
    }
    return {
      outputs, compiled, reused, linked, durationMs: performance.now() - started,
      toolchain: 'zig', arch: 'unknown', buildDir: this.outputBase,
      crossTargets: results,
    };
  }

  private async buildOne(
    options: BuildOverrides,
    crossTarget?: string,
    crossOptions?: CrossTargetOptions,
  ): Promise<BuildResult> {
    const started = performance.now();
    const tc = detectToolchain(options.toolchain ?? this.settings.toolchain ?? 'auto', {
      toolchainDir: options.toolchainDir ?? this.settings.toolchainDir,
      crossTarget,
    });
    const profile = options.profile ?? this.settings.profile ?? 'debug';
    const lto = options.lto ?? this.settings.lto ?? false;
    if (profile === 'debug' && lto) throw new Error('LTO in debug mode is unsupported; use release or relwithdebinfo.');
    const jobs = options.jobs ?? this.settings.jobs ?? defaultJobs();
    if (!Number.isSafeInteger(jobs) || jobs < 1) throw new Error(`Invalid job count: ${jobs}`);
    const verbose = options.verbose ?? this.settings.verbose ?? false;
    if (this.legacyIcon) throw new Error('Embedding icons requires an .rc resource; .webp is not a native executable icon.');
    if (this.targets.size === 0 && this.legacySources.length) {
      this.addExecutable(this.legacyName, { sources: this.legacySources });
    }
    const targets = this.graph(options.only).map(target => {
      const replacement: CrossTargetOverride | undefined = crossOptions?.targets?.[target.name];
      if (!replacement) return target;
      return {
        ...target, ...replacement,
        defines: { ...target.defines, ...replacement.defines },
        cFlags: [...(target.cFlags ?? []), ...(replacement.cFlags ?? [])],
        cxxFlags: [...(target.cxxFlags ?? []), ...(replacement.cxxFlags ?? [])],
        asmFlags: [...(target.asmFlags ?? []), ...(replacement.asmFlags ?? [])],
        linkFlags: [...(target.linkFlags ?? []), ...(replacement.linkFlags ?? [])],
        libraries: [...(target.libraries ?? []), ...(replacement.libraries ?? [])],
      };
    });
    for (const targetName of Object.keys(crossOptions?.targets ?? {})) {
      if (!this.targets.has(targetName)) throw new Error(`Cross target ${crossTarget}: unknown C/C++ target ${targetName}`);
    }
    if (!targets.length) throw new Error('No build targets were defined');
    const requiredPic = new Set<string>();
    const targetLookup = new Map(targets.map(target => [target.name, target]));
    const markStaticDependencies = (target: Target): void => {
      for (const name of target.dependencies ?? []) {
        const dependency = targetLookup.get(name);
        if (!dependency) throw new Error(`Unknown target dependency: ${name}`);
        if (dependency.kind === 'static') requiredPic.add(name);
        markStaticDependencies(dependency);
      }
    };
    for (const target of targets) if (target.kind === 'shared') markStaticDependencies(target);
    for (const name of requiredPic) {
      if (targetLookup.get(name)?.pic === false && (tc.targetPlatform ?? process.platform) !== 'win32') {
        throw new Error(`Static library ${name} must be compiled as PIC because a shared library depends on it`);
      }
    }
    const targetLabel = (tc.name === 'zig' ? (tc.targetTriple ?? tc.hostArch) : tc.hostArch).replace(/[^a-zA-Z0-9_.-]/g, '_');
    const dir = join(this.outputBase, `${profile}-${tc.name}-${targetLabel}${lto ? `-${lto}lto` : ''}`);
    if (!options.dryRun) {
      await mkdir(this.outputBase, { recursive: true });
      const markerPath = join(this.outputBase, '.bruga-build.json');
      if (await validFile(markerPath)) {
        const existing: unknown = JSON.parse(await readFile(markerPath, 'utf8'));
        if (!existing || typeof existing !== 'object' || (existing as { root?: string }).root !== this.root) {
          throw new Error(`Build directory belongs to a different project: ${this.outputBase}`);
        }
      } else if (existsSync(this.outputBase) && (await (await import('node:fs/promises')).readdir(this.outputBase)).length > 0) {
        throw new Error(`Refusing to use an existing unowned build directory: ${this.outputBase}`);
      } else await writeAtomic(markerPath, JSON.stringify({ root: this.root, version: 1 }) + '\n');
    }
    const cachePath = join(dir, 'cache.json');
    const cache = await loadCache(cachePath);
    const hash = new ContentFingerprints();
    const targetData = new Map<string, BuildTargetData>();
    const taskState = new Map<string, 'visiting' | 'done'>();
    const executeTask = async (name: string): Promise<void> => {
      const task = this.tasks.get(name);
      if (!task) throw new Error(`Unknown build task: ${name}`);
      if (taskState.get(name) === 'visiting') throw new Error(`Cyclic build task dependency at ${name}`);
      if (taskState.get(name) === 'done') return;
      taskState.set(name, 'visiting');
      for (const dependency of task.dependencies ?? []) await executeTask(dependency);
      const outputs = (task.outputs ?? []).map(p => resolve(this.root, p));
      const inputs = (task.inputs ?? []).map(p => resolve(this.root, p));
      for (const dep of task.dependencies ?? []) {
        for (const path of this.tasks.get(dep)?.outputs ?? []) inputs.push(resolve(this.root, path));
      }
      const fingerprint = await hash.group(inputs, [task.name, task.version ?? '0', ...outputs]);
      const validOutputs = outputs.length > 0 && (await Promise.all(outputs.map(validFile))).every(Boolean);
      const context: TaskContext = {
        rootDir: this.root, buildDir: dir, env: tc.env,
        run: async (argv, cwd) => { await runProcess(argv, { cwd: cwd ?? this.root, env: tc.env, printOutput: verbose }); },
      };
      if (options.rebuild || !validOutputs || cache.tasks[name] !== fingerprint) {
        if (options.dryRun) console.log(`[dry-run] task: ${name}`);
        else {
          if (verbose) console.log(`[task] ${name}`);
          await task.action(context);
          for (const output of outputs) {
            if (!(await validFile(output))) throw new Error(`Task ${name} did not produce ${output}`);
          }
          for (const output of outputs) hash.invalidate(output);
          cache.tasks[name] = fingerprint;
          await writeAtomic(cachePath, JSON.stringify(cache, null, 2) + '\n');
        }
      } else if (task.verify && !options.dryRun) {
        await task.verify(context);
      }
      taskState.set(name, 'done');
    };
    const compilationCommands: { directory: string; file: string; arguments: string[]; output: string }[] = [];
    let compiled = 0, reused = 0, linked = 0;
    for (const target of targets) {
      for (const task of target.tasks ?? []) await executeTask(task);
      const sources = await discoverSources(this.root, target.sources, this.outputBase);
      if (!sources.length) throw new Error(`Target ${target.name}: no source files matched`);
      const units = this.makeUnits(requiredPic.has(target.name) && target.pic === undefined ? { ...target, pic: true } : target, sources, dir, tc, profile, lto);
      const output = outputPath(dir, target, tc);
      for (const unit of units) compilationCommands.push({ directory: this.root, file: unit.source, arguments: unit.args, output: unit.object });
      const changed: BuildUnit[] = [];
      for (const unit of units) {
        const previous = cache.units[unit.object];
        if (options.rebuild || !previous || !(await validFile(unit.object))) { changed.push(unit); continue; }
        try {
          const fingerprint = await hash.group(previous.dependencies, [tc.identity, ...unit.args]);
          if (fingerprint === previous.fingerprint) { reused++; continue; }
        } catch { /* A deleted dependency always invalidates the object. */ }
        changed.push(unit);
      }
      if (changed.length) console.log(`[${target.name}] Compiling ${changed.length}/${units.length} source files (${jobs} jobs)`);
      if (options.dryRun) {
        for (const unit of changed) console.log(`  [dry-run] ${displayCommand(unit.args)}`);
        compiled += changed.length;
      } else {
        await runPool(changed, jobs, async unit => {
          await mkdir(dirname(unit.object), { recursive: true });
          if (verbose) console.log(displayCommand(unit.args));
          let result;
          try {
            result = await runProcess(unit.args, { cwd: this.root, env: tc.env });
          } catch (error) {
            await Promise.all([rm(unit.object, { force: true }), rm(unit.depfile, { force: true })]);
            throw error;
          }
          let dependencies: string[];
          if (tc.family === 'gnu') {
            if (!(await validFile(unit.depfile))) throw new Error(`Compiler did not create dependency file: ${unit.depfile}`);
            dependencies = parseDepfile(await readFile(unit.depfile, 'utf8'), this.root);
            if (result.stdout.trim()) process.stdout.write(result.stdout);
            if (result.stderr.trim()) process.stderr.write(result.stderr);
          } else {
            const stdout = parseMsvcIncludes(result.stdout);
            const stderr = parseMsvcIncludes(result.stderr);
            if (stdout.diagnostic) console.log(stdout.diagnostic);
            if (stderr.diagnostic) console.error(stderr.diagnostic);
            const found = [...stdout.dependencies, ...stderr.dependencies];
            dependencies = found.length ? found : await scanHeaders(this.root, [...unit.includes, dirname(unit.source)], this.outputBase);
          }
          dependencies.push(unit.source);
          dependencies = [...new Set(dependencies)];
          const fingerprint = await hash.group(dependencies, [tc.identity, ...unit.args]);
          cache.units[unit.object] = { fingerprint, dependencies };
          compiled++;
          if (!verbose) console.log(`  [C/C++] ${relative(this.root, unit.source)}`);
        });
        // Preserve successfully compiled objects even if a subsequent link fails.
        await writeAtomic(cachePath, JSON.stringify(cache, null, 2) + '\n');
      }
      targetData.set(target.name, { target, sources, objects: units.map(x => x.object), output });
      const command = this.linkArgs(target, units, targetData, dir, tc, profile, lto);
      const inputs = units.map(x => x.object);
      // Include every transitive archive/import library in the link cache key.
      // A static archive does not contain the archives it depends on.
      if (target.kind !== 'static') {
        const visited = new Set<string>();
        const includeDependency = (name: string): void => {
          if (visited.has(name)) return;
          visited.add(name);
          const dep = targetData.get(name);
          if (!dep) throw new Error(`Unbuilt target dependency: ${name}`);
          inputs.push(dep.target.kind === 'shared' && (tc.targetPlatform ?? process.platform) === 'win32'
            ? join(dir, 'lib', validateName(dep.target.outputName ?? dep.target.name) + (tc.family === 'msvc' ? '.lib' : '.dll.a'))
            : dep.output);
          for (const child of dep.target.dependencies ?? []) includeDependency(child);
        };
        for (const name of target.dependencies ?? []) includeDependency(name);
        for (const library of [...(this.settings.libraries ?? []), ...(target.libraries ?? [])]) {
          const candidate = resolve(this.root, library);
          if (await validFile(candidate)) inputs.push(candidate);
        }
      }
      const linkFingerprint = options.dryRun ? '' : await hash.group(inputs, [tc.identity, ...command]);
      const importLibrary = target.kind === 'shared' && (tc.targetPlatform ?? process.platform) === 'win32'
        ? join(dir, 'lib', validateName(target.outputName ?? target.name) + (tc.family === 'msvc' ? '.lib' : '.dll.a'))
        : undefined;
      const missingImportLibrary = importLibrary !== undefined && !(await validFile(importLibrary));
      if (options.rebuild || options.dryRun || missingImportLibrary || !(await validFile(output)) || cache.links[output] !== linkFingerprint) {
        if (options.dryRun) console.log(`[dry-run] ${displayCommand(command)}`);
        else {
          await mkdir(dirname(output), { recursive: true });
          if (importLibrary) await mkdir(dirname(importLibrary), { recursive: true });
          if (target.kind === 'static') await rm(output, { force: true });
          if (verbose) console.log(displayCommand(command));
          const actualCommand = await this.responseCommand(command, dir, target, tc);
          let result;
          try {
            result = await runProcess(actualCommand, { cwd: this.root, env: tc.env });
          } catch (error) {
            await rm(output, { force: true });
            if (importLibrary) await rm(importLibrary, { force: true });
            throw error;
          }
          if (result.stdout.trim()) process.stdout.write(result.stdout);
          if (result.stderr.trim()) process.stderr.write(result.stderr);
          if (importLibrary && !(await validFile(importLibrary))) {
            await rm(output, { force: true });
            throw new Error(`Linker did not produce Windows import library: ${importLibrary}`);
          }
          cache.links[output] = linkFingerprint;
          await writeAtomic(cachePath, JSON.stringify(cache, null, 2) + '\n');
          linked++;
          console.log(`  [${target.kind}] ${relative(this.root, output)}`);
        }
      } else if (verbose) console.log(`  [cached] ${relative(this.root, output)}`);
    }
    if (!options.dryRun) await writeAtomic(join(dir, 'compile_commands.json'), JSON.stringify(compilationCommands, null, 2) + '\n');
    const durationMs = performance.now() - started;
    console.log(`Build ${options.dryRun ? 'plan' : 'complete'}: ${compiled} compiled, ${reused} reused, ${linked} linked in ${(durationMs / 1000).toFixed(2)}s`);
    return { outputs: Object.fromEntries([...targetData].map(([name, data]) => [name, data.output])), compiled, reused, linked, durationMs, toolchain: tc.name, arch: tc.hostArch, buildDir: dir, runtimeBinDir: tc.runtimeBinDir, targetTriple: tc.targetTriple, targetPlatform: tc.targetPlatform };
  }

  async compileProject(buildDir?: string, _llvmBinFolder?: string): Promise<BuildResult> {
    if (buildDir && resolve(this.root, buildDir) !== this.outputBase) {
      throw new Error('Specify buildDir in the ProjectBuilder constructor, not compileProject().');
    }
    return await this.build();
  }
}

export { CommandError } from './src/process.ts';
