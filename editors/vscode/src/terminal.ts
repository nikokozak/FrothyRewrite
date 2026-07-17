import * as vscode from 'vscode';
import * as proc from './connect';

// Verbs that open the serial port themselves. The port has one owner, so the
// live session is torn down before the terminal takes it; the status bar's
// click-to-connect brings the session back afterwards.
const TAKES_PORT = new Set(['flash', 'install', 'wipe-user', 'doctor', 'connect', 'menu', 'stop', 'send']);

// ponytail: mirrors the flashable entries under boards/; replace with a CLI
// board listing once the CLI can produce one.
const FLASHABLE_BOARDS = ['esp32_devkit_v1', 'seeed_xiao_esp32s3'];

let terminal: vscode.Terminal | undefined;

export function initTerminal(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.window.onDidCloseTerminal((closed) => {
      if (closed === terminal) terminal = undefined;
    }),
  );
}

// ponytail: plain shell + sendText keeps verb output and prompts fully
// interactive; no exit detection — the status bar covers reconnecting.
export async function runVerb(verb: string, ...args: string[]): Promise<void> {
  const bin = vscode.workspace.getConfiguration('frothy').get<string>('binaryPath', 'frothy');
  if (TAKES_PORT.has(verb) && proc.isConnected()) await proc.teardown();
  if (!terminal || terminal.exitStatus !== undefined) {
    terminal = vscode.window.createTerminal({ name: 'Frothy' });
  }
  terminal.show();
  terminal.sendText([bin, verb, ...args].map(quoted).join(' '));
}

export async function flash(): Promise<void> {
  const other = 'Other…';
  const pick = await vscode.window.showQuickPick([...FLASHABLE_BOARDS, other], {
    placeHolder: 'Board to flash with Frothy firmware',
  });
  if (!pick) return;
  let board = pick;
  if (pick === other) {
    const typed = await vscode.window.showInputBox({
      prompt: 'Board name (a flashable directory under boards/)',
    });
    if (!typed) return;
    board = typed.trim();
  }
  await runVerb('flash', board);
}

function quoted(word: string): string {
  return /[^\w@%+=:,./-]/.test(word) ? `'${word.replace(/'/g, `'\\''`)}'` : word;
}
