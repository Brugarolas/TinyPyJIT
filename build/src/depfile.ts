import { readFile, readdir, stat } from 'node:fs/promises';
import { resolve, join, relative, sep } from 'node:path';
import { stableHash } from '../extended';

/** Parse GCC/Clang -MMD Makefile prerequisites, including escaped spaces. */
export function parseDepfile(input: string, cwd: string): string[] {
  const flattened = input.replace(/\\\r?\n/g, ' ');
  // The rule separator must be a colon followed by whitespace (not C: on Windows).
  const match = /:\s+/.exec(flattened);
  if (!match || match.index === undefined) throw new Error('Malformed compiler dependency file');
  const payload = flattened.slice(match.index + match[0].length);
  const dependencies: string[] = [];
  let current = '';
  for (let i = 0; i < payload.length; i++) {
    const c = payload[i];
    if (c === '\\' && i + 1 < payload.length && /[\s\\#:]/.test(payload[i + 1])) {
      current += payload[++i];
    } else if (/\s/.test(c)) {
      if (current) { dependencies.push(resolve(cwd, current)); current = ''; }
    } else current += c;
  }
  if (current) dependencies.push(resolve(cwd, current));
  return [...new Set(dependencies)];
}

export function parseMsvcIncludes(output: string): { dependencies: string[]; diagnostic: string } {
  const dependencies: string[] = [];
  const remaining: string[] = [];
  for (const line of output.split(/\r?\n/)) {
    const match = /^\s*Note: including file:\s*(.+)\s*$/i.exec(line);
    if (match) dependencies.push(resolve(match[1].trim()));
    else if (line) remaining.push(line);
  }
  return { dependencies: [...new Set(dependencies)], diagnostic: remaining.join('\n') };
}

/** Conservative MSVC fallback if localized /showIncludes output cannot be parsed. */
export async function scanHeaders(root: string, includeDirs: readonly string[], buildDir: string): Promise<string[]> {
  const files: string[] = [];
  const visited = new Set<string>();
  const walk = async (dir: string): Promise<void> => {
    if (visited.has(dir)) return;
    visited.add(dir);
    let entries;
    try { entries = await readdir(dir, { withFileTypes: true }); } catch { return; }
    for (const entry of entries) {
      if (entry.isSymbolicLink()) continue;
      const path = join(dir, entry.name);
      if (path === buildDir || path.startsWith(buildDir + sep)) continue;
      if (entry.isDirectory()) await walk(path);
      else if (entry.isFile() && /\.(h|hh|hpp|hxx|inc|inl)$/i.test(entry.name)) files.push(path);
    }
  };
  for (const dir of includeDirs) {
    const abs = resolve(root, dir);
    // Do not enumerate system include directories or paths outside the project.
    const rel = relative(root, abs);
    if (!rel.startsWith('..') && !rel.startsWith(sep)) await walk(abs);
  }
  return files.sort();
}

export class ContentFingerprints {
  private readonly digests = new Map<string, Promise<string>>();
  invalidate(path: string): void { this.digests.delete(path); }
  async file(path: string): Promise<string> {
    let promise = this.digests.get(path);
    if (!promise) {
      promise = readFile(path).then(bytes => stableHash(bytes));
      this.digests.set(path, promise);
    }
    return promise;
  }
  async group(files: readonly string[], extras: readonly string[]): Promise<string> {
    const normalized = [...new Set(files)].sort();
    const items = await Promise.all(normalized.map(async path => [path, await this.file(path)]));
    return stableHash(JSON.stringify({ extras, items }));
  }
}

export async function validFile(path: string): Promise<boolean> {
  try { return (await stat(path)).isFile(); } catch { return false; }
}
