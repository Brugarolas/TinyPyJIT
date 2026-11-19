import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, test } from 'node:test';
import ProjectBuilder from '../main-builder.ts';

const temporaryDirectories: string[] = [];
after(async () => {
  for (const directory of temporaryDirectories) {
    await rm(directory, { recursive: true, force: true });
  }
});

async function temporaryProject(): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), 'clank-test-'));
  temporaryDirectories.push(root);
  await mkdir(join(root, 'src'));
  return root;
}

function run(executable: string, args: readonly string[] = [], cwd?: string): string {
  return execFileSync(executable, [...args], { cwd, encoding: 'utf8' }).trim();
}

test('relinks shared libraries when a transitive static dependency changes', async () => {
  const root = await temporaryProject();
  await writeFile(join(root, 'src', 'leaf.c'), 'int leaf(void) { return 2; }\n');
  await writeFile(join(root, 'src', 'middle.c'), 'extern int leaf(void); int middle(void) { return leaf() + 1; }\n');
  await writeFile(join(root, 'src', 'wrapper.c'), 'extern int middle(void); int wrapper(void) { return middle() + 1; }\n');
  await writeFile(join(root, 'src', 'main.c'), '#include <stdio.h>\nextern int wrapper(void); int main(void) { printf("%d\\n", wrapper()); return 0; }\n');

  const builder = new ProjectBuilder({ rootDir: root, buildDir: 'out', toolchain: 'auto', profile: 'release' });
  builder.addStaticLibrary('leaf', { sources: ['src/leaf.c'] });
  builder.addStaticLibrary('middle', { sources: ['src/middle.c'], dependencies: ['leaf'] });
  builder.addSharedLibrary('wrapper', { sources: ['src/wrapper.c'], dependencies: ['middle'] });
  builder.addExecutable('app', { sources: ['src/main.c'], dependencies: ['wrapper'] });

  const first = await builder.build();
  assert.equal(run(first.outputs.app), '4');
  const second = await builder.build();
  assert.equal(second.compiled, 0);
  assert.equal(second.linked, 0);

  await writeFile(join(root, 'src', 'leaf.c'), 'int leaf(void) { return 3; }\n');
  const third = await builder.build();
  assert.equal(third.compiled, 1);
  assert.ok(third.linked >= 2, 'leaf and wrapper must be relinked');
  assert.equal(run(third.outputs.app), '5');
});

test('checks out and validates an exact Git commit without running a shell', async () => {
  const root = await temporaryProject();
  const repository = join(root, 'upstream');
  await mkdir(repository);
  run('git', ['init', '--quiet', repository]);
  await writeFile(join(repository, 'value.c'), 'int value(void) { return 7; }\n');
  run('git', ['-C', repository, 'add', '.']);
  run('git', ['-C', repository, '-c', 'user.name=Clank', '-c', 'user.email=clank@example.com', 'commit', '--quiet', '-m', 'initial']);
  run('git', ['-C', repository, 'tag', 'v1.0.0']);
  const commit = run('git', ['-C', repository, 'rev-parse', 'HEAD']);
  const builder = new ProjectBuilder({ rootDir: root, buildDir: 'out', toolchain: 'auto', profile: 'release' });
  builder.addGitCheckout({
    name: 'vendor-source', repository, ref: 'v1.0.0', commit, directory: 'vendor/upstream',
  });
  builder.addStaticLibrary('vendor', {
    sources: ['vendor/upstream/value.c'], tasks: ['vendor-source'],
  });
  const first = await builder.build();
  assert.ok(first.outputs.vendor.endsWith(process.platform === 'win32' ? '.lib' : '.a'));
  const second = await builder.build();
  assert.equal(second.compiled, 0);
  assert.equal(second.linked, 0);
  const checkedOutFile = join(root, 'vendor', 'upstream', 'value.c');
  assert.match(await readFile(checkedOutFile, 'utf8'), /return 7/);
  const injectedFile = join(root, 'vendor', 'upstream', 'injected.c');
  await writeFile(injectedFile, 'int injected(void) { return 123; }\n');
  await assert.rejects(() => builder.build(), /modified or untracked files/);
  await rm(injectedFile);
  await writeFile(checkedOutFile, 'int value(void) { return 999; }\n');
  await assert.rejects(() => builder.build(), /modified or untracked files/);
});
