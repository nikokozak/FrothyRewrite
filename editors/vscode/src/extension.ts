import * as vscode from 'vscode';
import * as commands from './commands';
import * as proc from './connect';
import { dispose as disposeOutput, show as showOutput } from './output';
import { setOnConnectionChanged } from './connect';
import { initStatusBar, updateStatusBar, disposeStatusBar } from './status-bar';

// Single source of truth for command registration; package.json maps
// IDs to titles + icons, extension.ts wires them to handlers.
const HANDLERS: Array<[string, () => unknown]> = [
  ['frothy.connect',       () => commands.connect()],
  ['frothy.disconnect',    () => commands.disconnect()],
  ['frothy.runLine',       () => commands.runLine()],
  ['frothy.sendSelection', () => commands.sendSelection()],
  ['frothy.runLast',       () => commands.runLast()],
  ['frothy.sendFile',      () => commands.sendFile()],
  ['frothy.openExample',   () => commands.openExample()],
  ['frothy.see',           () => commands.see()],
  ['frothy.words',         () => commands.words()],
  ['frothy.status',        () => commands.status()],
  ['frothy.mem',           () => commands.mem()],
  ['frothy.save',          () => commands.save()],
  ['frothy.restore',       () => commands.restore()],
  ['frothy.interrupt',     () => commands.interrupt()],
  ['frothy.showOutput',    () => showOutput()],
];

export function activate(context: vscode.ExtensionContext): void {
  for (const [id, handler] of HANDLERS) {
    context.subscriptions.push(vscode.commands.registerCommand(id, handler));
  }

  initStatusBar(context);
  refreshContext();

  // Keep both the status bar and the when-clause context in sync with
  // the subprocess lifecycle. proc.connect / proc.teardown fire this;
  // setLastForm in commands.ts fires its own refresh.
  setOnConnectionChanged(refreshContext);
  commands.setOnLastFormChanged(refreshContext);

  // Auto-connect when the user opens a Frothy file, if enabled.
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(async (doc) => {
      const cfg = vscode.workspace.getConfiguration('frothy');
      if (!cfg.get<boolean>('autoConnect', false)) return;
      if (doc.languageId !== 'frothy') return;
      if (proc.isConnected()) return;
      await commands.connect();
    }),
  );

  // Refresh the status bar when the user changes frothy.port.
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('frothy.port')) refreshContext();
    }),
  );
}

export async function deactivate(): Promise<void> {
  await proc.teardown();
  disposeOutput();
  disposeStatusBar();
}

function refreshContext(): void {
  const connected = proc.isConnected();
  void vscode.commands.executeCommand('setContext', 'frothy.isConnected', connected);
  void vscode.commands.executeCommand('setContext', 'frothy.hasLastForm', commands.hasLastForm());
  updateStatusBar(connected, currentPortLabel());
}

function currentPortLabel(): string {
  const cfg = vscode.workspace.getConfiguration('frothy');
  return cfg.get<string>('port', '') || 'auto';
}
