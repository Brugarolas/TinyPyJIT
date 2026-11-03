/** Host architecture normalization; this never pretends to cross-compile. */
import { execFileSync } from 'node:child_process';
import { machine } from 'node:os';

export type Architecture = 'x86' | 'x64' | 'arm' | 'arm64' | 'riscv64' | 'loong64' | 'mips' | 'mips64' | 'ppc64' | 's390' | 's390x' | 'unknown';

export function normalizeArchitecture(input: string): Architecture {
  const value = input.toLowerCase().trim();
  switch (value) {
    case 'ia32': case 'x86': case 'i386': case 'i486': case 'i586': case 'i686': return 'x86';
    case 'amd64': case 'x86_64': case 'x64': return 'x64';
    case 'arm': case 'arm32': case 'armv6': case 'armv6l': case 'armv7': case 'armv7l': case 'armv8l': return 'arm';
    case 'aarch64': case 'aarch64_be': case 'arm64': return 'arm64';
    case 'riscv64': return 'riscv64';
    case 'loongarch64': case 'loong64': return 'loong64';
    case 'mips': case 'mipsel': return 'mips';
    case 'mips64': case 'mips64el': return 'mips64';
    case 'ppc64': case 'ppc64le': return 'ppc64';
    case 's390': return 's390';
    case 's390x': return 's390x';
    default: return 'unknown';
  }
}

export function arch(): Architecture {
  if (process.platform === 'darwin' && process.arch === 'x64') {
    try {
      if (execFileSync('/usr/sbin/sysctl', ['-in', 'sysctl.proc_translated'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim() === '1') {
        return 'arm64';
      }
    } catch { /* Rosetta detection is optional. */ }
  }
  const native = normalizeArchitecture(machine());
  return native !== 'unknown' ? native : normalizeArchitecture(process.arch);
}

export default arch;
