import { readdir } from 'node:fs/promises';
import { extname, isAbsolute, relative, resolve, sep } from 'node:path';
import type { SourceInput } from './types';

const IGNORED_DIRS = new Set(['.git', '.hg', '.svn', 'node_modules', '.cache']);
const COMPILE_EXTENSIONS = new Set(['.c', '.cc', '.cpp', '.cxx', '.s', '.S', '.m', '.mm']);

export function normalizePath(path: string): string { return path.split(sep).join('/'); }

export function globRegex(pattern: string): RegExp {
  let out = '^';
  const p = normalizePath(pattern);
  for (let i = 0; i < p.length; i++) {
    const c = p[i];
    if (c === '*') {
      if (p[i + 1] === '*') {
        i++;
        if (p[i + 1] === '/') { i++; out += '(?:.*/)?'; }
        else out += '.*';
      } else out += '[^/]*';
    } else if (c === '?') out += '[^/]';
    else out += c.replace(/[\\^$+?.()|{}\[\]]/g, '\\$&');
  }
  return new RegExp(out + '$');
}

export async function discoverSources(root: string, inputs: readonly SourceInput[], buildDir?: string): Promise<string[]> {
  const result = new Set<string>();
  const absBuild = buildDir ? resolve(buildDir) : undefined;
  for (const input of inputs) {
    if (typeof input === 'string') {
      const absolute = resolve(root, input);
      if (!COMPILE_EXTENSIONS.has(extname(absolute))) throw new Error(`Not a compilable source: ${input}`);
      result.add(absolute);
      continue;
    }
    const directory = resolve(root, input.directory);
    const allowed = new Set(input.extensions);
    const excluded = input.exclude.map(globRegex);
    const walk = async (dir: string): Promise<void> => {
      const entries = (await readdir(dir, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name));
      for (const entry of entries) {
        const abs = resolve(dir, entry.name);
        if (abs === absBuild || (absBuild && abs.startsWith(absBuild + sep))) continue;
        if (entry.isSymbolicLink()) continue;
        if (entry.isDirectory()) {
          if (!IGNORED_DIRS.has(entry.name)) await walk(abs);
        } else if (entry.isFile() && allowed.has(extname(entry.name))) {
          const rel = normalizePath(relative(directory, abs));
          if (!excluded.some(regex => regex.test(rel))) result.add(abs);
        }
      }
    };
    await walk(directory);
  }
  return [...result].sort((a, b) => a.localeCompare(b));
}

export function validateName(name: string): string {
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_.-]*$/.test(name) || name.includes('..')) {
    throw new Error(`Invalid target name: ${JSON.stringify(name)}`);
  }
  return name;
}

export function isSubPath(parent: string, candidate: string): boolean {
  const rel = relative(resolve(parent), resolve(candidate));
  return rel !== '' && !isAbsolute(rel) && rel !== '..' && !rel.startsWith('..' + sep);
}
