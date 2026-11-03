/** Pure collection and hashing helpers. No global prototype mutation. */
import { createHash } from 'node:crypto';

export function partition<T>(values: Iterable<T>, predicate: (value: T) => boolean): { match: T[]; others: T[] } {
  const match: T[] = [];
  const others: T[] = [];
  for (const value of values) (predicate(value) ? match : others).push(value);
  return { match, others };
}

export function without<T>(values: ReadonlySet<T>, removed: ReadonlySet<T>): Set<T> {
  return new Set([...values].filter(value => !removed.has(value)));
}

export function containsAny(value: string, needles: ReadonlySet<string>): boolean {
  for (const needle of needles) if (value.includes(needle)) return true;
  return false;
}

export function stableHash(value: string | Uint8Array): string {
  return createHash('sha256').update(value).digest('hex');
}
