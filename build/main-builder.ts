/** Bruga Build: project configuration is ordinary TypeScript, not a second DSL. */
import { readFile, writeFile, mkdir, rm, rename } from 'node:fs/promises';
import { basename, dirname, extname, isAbsolute, join, relative, resolve, sep } from 'node:path';
import { performance } from 'node:perf_hooks';
import { stableHash } from './extended';
import { existsSync } from 'node:fs';
import { detectToolchain, defaultJobs } from './src/toolchains';
import { parseDepfile, parseMsvcIncludes, ContentFingerprints, scanHeaders, validFile } from './src/depfile';
import { displayCommand, runPool, runProcess } from './src/process';
import { discoverSources, isSubPath, validateName } from './src/sources';
import type { BuildOverrides, BuildResult, Definitions, LtoMode, Profile, ProjectSettings, ResolvedCompiler, SourceInput, Target, BuildTask, TaskContext } from './src/types';

export { sourceTree } from './src/types';
export type { SourceTree, SourceInput, Target, TargetKind, ProjectSettings, BuildOverrides, BuildResult, ToolchainName, Profile, LtoMode, Definitions, BuildTask, TaskContext } from './src/types';

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
  return (tc.name === 'clang' || tc.name === 'clang-mingw') ? [`-flto=${lto}`] : ['-flto'];
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
  if (threads) flags.push('-pthread');
  return flags;
}

function outputPath(dir: string, target: Target, tc: ResolvedCompiler): string {
  const stem = validateName(target.outputName ?? target.name);
  if (target.kind === 'executable') return join(dir, 'bin', stem + (process.platform === 'win32' ? '.exe' : ''));
  if (target.kind === 'shared') {
    return join(dir, process.platform === 'win32' ? 'bin' : 'lib', process.platform === 'win32' ? `${stem}.dll` : process.platform === 'darwin' ? `lib${stem}.dylib` : `lib${stem}.so`);
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
      const args = isMsvc
        ? [compiler, ...flags, '/c', source, `/Fo${object}`]
        : [compiler, ...flags, '-MMD', '-MF', depfile, '-MT', object, '-c', source, '-o', object];
      return { source, object, depfile, args, includes: includePaths };
    });
  }

  private linkArgs(target: Target, units: BuildUnit[], targetData: Map<string, BuildTargetData>, dir: string, tc: ResolvedCompiler, profile: Profile, lto: LtoMode): string[] {
    const output = outputPath(dir, target, tc);
    if (target.kind === 'static') {
      return tc.family === 'msvc'
        ? [tc.librarian, '/NOLOGO', `/OUT:${output}`, ...units.map(x => x.object)]
        : [tc.librarian, 'rcs', output, ...units.map(x => x.object)];
    }
    const dependencyOutputs: string[] = [];
    const seen = new Set<string>();
    const addDep = (name: string): void => {
      if (seen.has(name)) return;
      seen.add(name);
      const data = targetData.get(name);
      if (!data) throw new Error(`Unbuilt target dependency: ${name}`);
      if (data.target.kind === 'executable') throw new Error(`Cannot link an executable as a dependency: ${name}`);
      dependencyOutputs.push(data.target.kind === 'shared' && process.platform === 'win32'
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
    const rpath = dependentShared && process.platform !== 'win32'
      ? [process.platform === 'darwin' ? `-Wl,-rpath,${target.kind === 'shared' ? '@loader_path' : '@loader_path/../lib'}`
        : `-Wl,-rpath,${target.kind === 'shared' ? '$ORIGIN' : '$ORIGIN/../lib'}`] : [];
    return [driver, ...units.map(x => x.object), ...dependencyOutputs,
      ...[...libs].flatMap(l => systemLib(l, tc)),
      ...((target.kind === 'shared') ? [process.platform === 'darwin' ? '-dynamiclib' : '-shared'] : []),
      ...(threadEnabled ? ['-pthread'] : []), ...ltoFlags(lto, tc), ...(tc.name === 'clang' && lto && process.platform === 'linux' ? ['-fuse-ld=lld'] : []), ...rpath,
      ...(target.kind === 'shared' && process.platform === 'win32' ? [`-Wl,--out-implib,${join(dir, 'lib', validateName(target.outputName ?? target.name) + '.dll.a')}`] : []),
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
    const content = command.slice(1).map(quote).join('\n') + '\n';
    if (process.platform === 'win32' && tc.family === 'msvc') {
      // MSVC link.exe and lib.exe accept UTF-16LE response files. GNU uses UTF-8.
      await writeFile(filename, Buffer.concat([Buffer.from([0xff, 0xfe]), Buffer.from(content, 'utf16le')]));
    } else await writeFile(filename, content, 'utf8');
    return [command[0], `@${filename}`];
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

  async build(options: BuildOverrides = {}): Promise<BuildResult> {
    const started = performance.now();
    const tc = detectToolchain(options.toolchain ?? this.settings.toolchain ?? 'auto', {
      toolchainDir: options.toolchainDir ?? this.settings.toolchainDir,
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
    const targets = this.graph(options.only);
    if (!targets.length) throw new Error('No build targets were defined');
    const dir = join(this.outputBase, `${profile}-${tc.name}-${tc.hostArch}${lto ? `-${lto}lto` : ''}`);
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
      if (options.rebuild || !validOutputs || cache.tasks[name] !== fingerprint) {
        if (options.dryRun) console.log(`[dry-run] task: ${name}`);
        else {
          if (verbose) console.log(`[task] ${name}`);
          const context: TaskContext = {
            rootDir: this.root, buildDir: dir, env: tc.env,
            run: async (argv, cwd) => { await runProcess(argv, { cwd: cwd ?? this.root, env: tc.env, printOutput: verbose }); },
          };
          await task.action(context);
          for (const output of outputs) {
            if (!(await validFile(output))) throw new Error(`Task ${name} did not produce ${output}`);
          }
          for (const output of outputs) hash.invalidate(output);
          cache.tasks[name] = fingerprint;
          await writeAtomic(cachePath, JSON.stringify(cache, null, 2) + '\n');
        }
      }
      taskState.set(name, 'done');
    };
    const compilationCommands: { directory: string; file: string; arguments: string[]; output: string }[] = [];
    let compiled = 0, reused = 0, linked = 0;
    for (const target of targets) {
      for (const task of target.tasks ?? []) await executeTask(task);
      const sources = await discoverSources(this.root, target.sources, this.outputBase);
      if (!sources.length) throw new Error(`Target ${target.name}: no source files matched`);
      const units = this.makeUnits(target, sources, dir, tc, profile, lto);
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
      const inputs = [ ...units.map(x => x.object), ...(target.dependencies ?? []).map(n => {
        const dep = targetData.get(n)!;
        return dep.target.kind === 'shared' && process.platform === 'win32'
          ? join(dir, 'lib', validateName(dep.target.outputName ?? dep.target.name) + (tc.family === 'msvc' ? '.lib' : '.dll.a'))
          : dep.output;
      }) ];
      const linkFingerprint = options.dryRun ? '' : await hash.group(inputs, [tc.identity, ...command]);
      if (options.rebuild || options.dryRun || !(await validFile(output)) || cache.links[output] !== linkFingerprint) {
        if (options.dryRun) console.log(`[dry-run] ${displayCommand(command)}`);
        else {
          await mkdir(dirname(output), { recursive: true });
          if (target.kind === 'shared' && process.platform === 'win32') await mkdir(join(dir, 'lib'), { recursive: true });
          if (target.kind === 'static') await rm(output, { force: true });
          if (verbose) console.log(displayCommand(command));
          const actualCommand = await this.responseCommand(command, dir, target, tc);
          let result;
          try {
            result = await runProcess(actualCommand, { cwd: this.root, env: tc.env });
          } catch (error) {
            await rm(output, { force: true });
            if (target.kind === 'shared' && process.platform === 'win32') {
              await rm(join(dir, 'lib', validateName(target.outputName ?? target.name) + (tc.family === 'msvc' ? '.lib' : '.dll.a')), { force: true });
            }
            throw error;
          }
          if (result.stdout.trim()) process.stdout.write(result.stdout);
          if (result.stderr.trim()) process.stderr.write(result.stderr);
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
    return { outputs: Object.fromEntries([...targetData].map(([name, data]) => [name, data.output])), compiled, reused, linked, durationMs, toolchain: tc.name, arch: tc.hostArch, buildDir: dir, runtimeBinDir: tc.runtimeBinDir };
  }

  async compileProject(buildDir?: string, _llvmBinFolder?: string): Promise<BuildResult> {
    if (buildDir && resolve(this.root, buildDir) !== this.outputBase) {
      throw new Error('Specify buildDir in the ProjectBuilder constructor, not compileProject().');
    }
    return await this.build();
  }
}

export { CommandError } from './src/process';
