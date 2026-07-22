import * as fs from 'fs';
import * as path from 'path';

// GUI-launched VS Code does not inherit the login shell's PATH on macOS, so
// the bare name "frothy" often fails even with a healthy Homebrew install.
// An empty binaryPath means auto-discover: the process PATH first, then the
// standard Homebrew prefixes.
const HOMEBREW_BINS = [
  '/opt/homebrew/bin/frothy',
  '/usr/local/bin/frothy',
  '/home/linuxbrew/.linuxbrew/bin/frothy',
];

export function findFrothy(
  configured: string,
  envPath: string,
  platform: NodeJS.Platform,
  fallbackBins: readonly string[] = HOMEBREW_BINS,
): string {
  if (configured) return configured;
  const names = platform === 'win32' ? ['frothy.exe', 'frothy.cmd', 'frothy'] : ['frothy'];
  for (const dir of envPath.split(path.delimiter)) {
    if (!dir) continue;
    for (const name of names) {
      const candidate = path.join(dir, name);
      if (isExecutable(candidate)) return candidate;
    }
  }
  for (const candidate of fallbackBins) {
    if (isExecutable(candidate)) return candidate;
  }
  return 'frothy';
}

function isExecutable(candidate: string): boolean {
  try {
    fs.accessSync(candidate, fs.constants.X_OK);
    return fs.statSync(candidate).isFile();
  } catch {
    return false;
  }
}
