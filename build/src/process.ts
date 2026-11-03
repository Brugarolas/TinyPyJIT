import { spawn } from 'node:child_process';
import { resolve } from 'node:path';
import type { ProcessResult } from './types';

export class CommandError extends Error {
  constructor(
    readonly command: readonly string[],
    readonly cwd: string,
    readonly exitCode: number | null,
    readonly stdout: string,
    readonly stderr: string,
  ) {
    super(`Command failed (exit ${exitCode ?? 'signal'}): ${displayCommand(command)}\n${stderr || stdout}`);
    this.name = 'CommandError';
  }
}

export function displayCommand(command: readonly string[]): string {
  return command.map(arg => /[\s"']/u.test(arg) ? JSON.stringify(arg) : arg).join(' ');
}

/** Never runs through a shell; argument boundaries are preserved on all platforms. */
export async function runProcess(
  argv: readonly string[],
  options: { cwd: string; env?: NodeJS.ProcessEnv; signal?: AbortSignal; printOutput?: boolean },
): Promise<ProcessResult> {
  if (argv.length === 0) throw new Error('Cannot execute an empty command');
  return await new Promise<ProcessResult>((yes, no) => {
    let settled = false;
    const child = spawn(argv[0], [...argv.slice(1)], {
      cwd: resolve(options.cwd), env: options.env ?? process.env, shell: false,
      windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], signal: options.signal,
    });
    const stdout: Buffer[] = [];
    const stderr: Buffer[] = [];
    child.stdout.on('data', (data: Buffer) => { stdout.push(data); if (options.printOutput) process.stdout.write(data); });
    child.stderr.on('data', (data: Buffer) => { stderr.push(data); if (options.printOutput) process.stderr.write(data); });
    child.once('error', error => { if (!settled) { settled = true; no(error); } });
    child.once('close', (code, signal) => {
      if (settled) return;
      settled = true;
      const out = Buffer.concat(stdout).toString('utf8');
      const err = Buffer.concat(stderr).toString('utf8');
      if (code !== 0 || signal) no(new CommandError(argv, options.cwd, code, out, err));
      else yes({ stdout: out, stderr: err, code: code ?? 0 });
    });
  });
}

/** A bounded worker queue. The first failure prevents new work from being scheduled. */
export async function runPool<T>(items: readonly T[], concurrency: number, run: (item: T) => Promise<void>): Promise<void> {
  let next = 0;
  let firstError: unknown;
  const worker = async () => {
    while (firstError === undefined && next < items.length) {
      const index = next++;
      try { await run(items[index]); }
      catch (error) { if (firstError === undefined) firstError = error; }
    }
  };
  await Promise.all(Array.from({ length: Math.min(Math.max(1, concurrency), items.length) }, () => worker()));
  if (firstError !== undefined) throw firstError;
}
