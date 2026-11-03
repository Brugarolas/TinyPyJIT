import type { Architecture } from '../arch';

export type Profile = 'debug' | 'release' | 'relwithdebinfo';
export type LtoMode = false | 'thin' | 'full';
export type ToolchainName = 'auto' | 'clang' | 'gcc' | 'clang-mingw' | 'mingw' | 'msvc' | 'clang-cl';
export type TargetKind = 'executable' | 'static' | 'shared';
export type Definition = string | number | boolean | null;
export type Definitions = Readonly<Record<string, Definition>>;

export interface SourceTree {
  readonly kind: 'source-tree';
  readonly directory: string;
  readonly extensions: readonly string[];
  readonly exclude: readonly string[];
}

export type SourceInput = string | SourceTree;

export function sourceTree(directory: string, options: { extensions?: readonly string[]; exclude?: readonly string[] } = {}): SourceTree {
  return {
    kind: 'source-tree', directory,
    extensions: options.extensions ?? ['.c', '.cc', '.cpp', '.cxx', '.s', '.S', '.m', '.mm'],
    exclude: options.exclude ?? [],
  };
}

export interface Target {
  readonly name: string;
  readonly kind: TargetKind;
  readonly sources: readonly SourceInput[];
  readonly includeDirs?: readonly string[];
  readonly defines?: Definitions;
  readonly cFlags?: readonly string[];
  readonly cxxFlags?: readonly string[];
  readonly asmFlags?: readonly string[];
  readonly linkFlags?: readonly string[];
  readonly libraries?: readonly string[];
  readonly dependencies?: readonly string[];
  readonly outputName?: string;
  readonly pic?: boolean;
  readonly threads?: boolean;
  readonly tasks?: readonly string[];
}

export interface TaskContext {
  readonly rootDir: string;
  readonly buildDir: string;
  readonly env: NodeJS.ProcessEnv;
  run(argv: readonly string[], cwd?: string): Promise<void>;
}

export interface BuildTask {
  readonly name: string;
  readonly dependencies?: readonly string[];
  readonly inputs?: readonly string[];
  readonly outputs?: readonly string[];
  readonly version?: string;
  readonly action: (context: TaskContext) => void | Promise<void>;
}

export interface ProjectSettings {
  rootDir?: string;
  buildDir?: string;
  toolchain?: ToolchainName;
  toolchainDir?: string;
  profile?: Profile;
  lto?: LtoMode;
  jobs?: number;
  defines?: Definitions;
  includeDirs?: readonly string[];
  commonFlags?: readonly string[];
  cFlags?: readonly string[];
  cxxFlags?: readonly string[];
  asmFlags?: readonly string[];
  linkFlags?: readonly string[];
  libraries?: readonly string[];
  threads?: boolean;
  verbose?: boolean;
  responseFileThreshold?: number;
}

export interface BuildOverrides {
  readonly toolchain?: ToolchainName;
  readonly toolchainDir?: string;
  readonly profile?: Profile;
  readonly lto?: LtoMode;
  readonly jobs?: number;
  readonly rebuild?: boolean;
  readonly dryRun?: boolean;
  readonly verbose?: boolean;
  readonly only?: readonly string[];
}

export interface BuildResult {
  readonly outputs: Readonly<Record<string, string>>;
  readonly compiled: number;
  readonly reused: number;
  readonly linked: number;
  readonly durationMs: number;
  readonly toolchain: string;
  readonly arch: Architecture;
  readonly buildDir: string;
  readonly runtimeBinDir?: string;
}

export interface ResolvedCompiler {
  readonly family: 'gnu' | 'msvc';
  readonly name: Exclude<ToolchainName, 'auto'>;
  readonly cCompiler: string;
  readonly cxxCompiler: string;
  readonly linker: string;
  readonly librarian: string;
  readonly env: NodeJS.ProcessEnv;
  readonly identity: string;
  readonly hostArch: Architecture;
  readonly runtimeBinDir?: string;
  readonly targetTriple?: string;
}

export interface ProcessResult { stdout: string; stderr: string; code: number; }
