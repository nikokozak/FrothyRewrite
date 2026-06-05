import * as vscode from 'vscode';
import * as connect from './connect';
import { dispose as disposeOutput } from './output';

const NOOP_IDS = [
  'frothy.sendSelection',
  'frothy.sendFile',
  'frothy.see',
  'frothy.words',
  'frothy.save',
  'frothy.restore',
  'frothy.interrupt',
] as const;

export function activate(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.commands.registerCommand('frothy.connect', () => connect.connect()),
  );
  for (const id of NOOP_IDS) {
    context.subscriptions.push(vscode.commands.registerCommand(id, () => {}));
  }
}

export async function deactivate(): Promise<void> {
  await connect.teardown();
  disposeOutput();
}
