import * as vscode from 'vscode';
import * as commands from './commands';
import * as proc from './connect';
import { dispose as disposeOutput } from './output';

const HANDLERS: Array<[string, () => unknown]> = [
  ['frothy.connect',       () => commands.connect()],
  ['frothy.sendSelection', () => commands.sendSelection()],
  ['frothy.sendFile',      () => commands.sendFile()],
  ['frothy.see',           () => commands.see()],
  ['frothy.words',         () => commands.words()],
  ['frothy.save',          () => commands.save()],
  ['frothy.restore',       () => commands.restore()],
  ['frothy.interrupt',     () => commands.interrupt()],
];

export function activate(context: vscode.ExtensionContext): void {
  for (const [id, handler] of HANDLERS) {
    context.subscriptions.push(vscode.commands.registerCommand(id, handler));
  }
}

export async function deactivate(): Promise<void> {
  await proc.teardown();
  disposeOutput();
}
