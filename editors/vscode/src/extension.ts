import * as vscode from 'vscode';

const COMMAND_IDS = [
  'frothy.connect',
  'frothy.sendSelection',
  'frothy.sendFile',
  'frothy.see',
  'frothy.words',
  'frothy.save',
  'frothy.restore',
  'frothy.interrupt',
] as const;

export function activate(context: vscode.ExtensionContext): void {
  for (const id of COMMAND_IDS) {
    context.subscriptions.push(vscode.commands.registerCommand(id, () => {}));
  }
}

export function deactivate(): void {}
