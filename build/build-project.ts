/** Example reusable project definition. Edit the paths for your project. */
import ProjectBuilder, { sourceTree } from './main-builder';
import { runBuildCli } from './cli';

const builder = new ProjectBuilder({
  rootDir: process.cwd(),
  buildDir: 'build-ts',
  toolchain: 'auto',
  includeDirs: ['include'],
  profile: 'release',
});

// Unlike the original builder, headers are not compiled as object files.
// They are tracked automatically as dependencies of the C/C++ sources.
builder.addExecutable('app', {
  sources: [sourceTree('src', {
    exclude: ['**/tests/**', '**/benchmarks/**', '**/examples/**'],
  })],
  outputName: 'app',
});

runBuildCli(builder, 'app').catch(error => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
