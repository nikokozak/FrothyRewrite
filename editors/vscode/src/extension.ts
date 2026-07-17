import * as vscode from 'vscode';
import * as commands from './commands';
import * as proc from './connect';
import { initCodeLens } from './codelens';
import { initCompletion } from './completion';
import { clearDiagnostics, initDiagnostics } from './diagnostics';
import { dispose as disposeOutput, show as showOutput } from './output';
import { setOnConnectionChanged } from './connect';
import { initStatusBar, updateStatusBar, disposeStatusBar } from './status-bar';
import * as terminal from './terminal';
import * as views from './views';

// Single source of truth for command registration; package.json maps
// IDs to titles + icons, extension.ts wires them to handlers. Commands
// reachable only from tree items or lenses (runFormAt, inspectNamed)
// need no package.json entry.
const HANDLERS: Array<[string, (...args: unknown[]) => unknown]> = [
  ['frothy.connect',       () => connectWithGuidance()],
  ['frothy.disconnect',    () => commands.disconnect()],
  ['frothy.runForm',       () => commands.runForm()],
  ['frothy.runFile',       () => commands.runFile()],
  ['frothy.rerun',         () => commands.rerun()],
  ['frothy.openExample',   () => commands.openExample()],
  ['frothy.browseWords',   () => commands.browseWords()],
  ['frothy.inspectWord',   () => commands.inspectWord()],
  ['frothy.status',        () => commands.status()],
  ['frothy.mem',           () => commands.mem()],
  ['frothy.save',          () => commands.save()],
  ['frothy.restore',       () => commands.restore()],
  ['frothy.interrupt',     () => commands.interrupt()],
  ['frothy.showOutput',    () => showOutput()],
  ['frothy.flash',         () => terminal.flash()],
  ['frothy.build',         () => terminal.runVerb('build')],
  ['frothy.install',       () => terminal.runVerb('install')],
  ['frothy.doctor',        () => terminal.runVerb('doctor')],
  ['frothy.initProject',   () => terminal.runVerb('init')],
  ['frothy.wipeUser',      () => terminal.runVerb('wipe-user')],
  ['frothy.stopSessions',  () => terminal.runVerb('stop')],
  ['frothy.openRepl',      () => terminal.runVerb('connect')],
  ['frothy.openMenu',      () => terminal.runVerb('menu')],
  ['frothy.refreshWords',  () => views.refreshWords()],
  ['frothy.runFormAt',     (uri, line) => commands.runFormAt(uri as vscode.Uri, line as number)],
  ['frothy.inspectNamed',  (word) => commands.inspectNamedWord(word as string)],
];

let prevConnected = false;

export function activate(context: vscode.ExtensionContext): void {
  for (const [id, handler] of HANDLERS) {
    context.subscriptions.push(vscode.commands.registerCommand(id, handler));
  }

  initStatusBar(context);
  initDiagnostics(context);
  initCodeLens(context);
  views.initViews(context);
  initCompletion(context);
  terminal.initTerminal(context);
  refreshContext();

  // Keep both the status bar and the when-clause context in sync with
  // the subprocess lifecycle. proc.connect / proc.teardown fire this;
  // setLastForm in commands.ts fires its own refresh.
  setOnConnectionChanged(refreshContext);
  commands.setOnLastFormChanged(refreshContext);

  // Auto-connect when the user opens a Frothy file, if enabled.
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => void autoConnect(doc)),
    vscode.window.onDidChangeActiveTextEditor(refreshContext),
    vscode.workspace.onDidChangeTextDocument((event) => clearDiagnostics(event.document)),
    vscode.workspace.onDidCloseTextDocument(clearDiagnostics),
  );
  void autoConnect(vscode.window.activeTextEditor?.document);

  // Refresh the status bar when the user changes Frothy settings.
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('frothy')) refreshContext();
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
  const snapshot = proc.sessionSnapshot();
  const busy = proc.isBusy();
  const frothyEditor = vscode.window.activeTextEditor?.document.languageId === 'frothy';
  void vscode.commands.executeCommand('setContext', 'frothy.isConnected', connected);
  void vscode.commands.executeCommand('setContext', 'frothy.isBusy', busy);
  void vscode.commands.executeCommand('setContext', 'frothy.hasLastForm', commands.hasLastForm());
  updateStatusBar(snapshot, connected, busy, frothyEditor || snapshot.state !== 'closed', currentPortLabel());
  views.refreshDeviceView();

  // Words are fetched only when the user asks (refresh button, palette,
  // the empty-state row); a dropped connection just clears the cache.
  if (!connected && prevConnected) views.clearWords();
  prevConnected = connected;
}

function currentPortLabel(): string {
  const cfg = vscode.workspace.getConfiguration('frothy');
  return cfg.get<string>('port', '') || 'auto';
}

async function autoConnect(doc: vscode.TextDocument | undefined): Promise<void> {
  if (!doc || doc.languageId !== 'frothy' || proc.isConnected()) return;
  const cfg = vscode.workspace.getConfiguration('frothy');
  if (!cfg.get<boolean>('autoConnect', false)) return;
  await connectWithGuidance();
}

async function connectWithGuidance(): Promise<void> {
  try {
    await commands.connect();
  } catch (err) {
    const error = err as NodeJS.ErrnoException;
    if (error.code === 'ENOENT') {
      const choice = await vscode.window.showErrorMessage(
        'Frothy could not find the CLI. Install it or configure frothy.binaryPath.',
        'Configure CLI Path',
        'Install CLI',
      );
      if (choice === 'Configure CLI Path') {
        await vscode.commands.executeCommand('workbench.action.openSettings', 'frothy.binaryPath');
      } else if (choice === 'Install CLI') {
        await vscode.env.openExternal(
          vscode.Uri.parse('https://github.com/nikokozak/FrothyRewrite#develop-on-your-machine'),
        );
      }
      return;
    }

    const choice = await vscode.window.showErrorMessage(
      `Frothy could not connect: ${error.message}`,
      'Show Output',
    );
    if (choice === 'Show Output') showOutput();
  }
}
