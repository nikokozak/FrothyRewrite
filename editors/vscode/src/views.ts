import * as vscode from 'vscode';
import * as proc from './connect';
import { parseWords } from './words';

interface RowOptions {
  icon?: string;
  description?: string;
  tooltip?: string;
  command?: string;
  args?: unknown[];
}

function row(label: string, opts: RowOptions = {}): vscode.TreeItem {
  const item = new vscode.TreeItem(label, vscode.TreeItemCollapsibleState.None);
  if (opts.icon) item.iconPath = new vscode.ThemeIcon(opts.icon);
  if (opts.description) item.description = opts.description;
  if (opts.tooltip) item.tooltip = opts.tooltip;
  if (opts.command) {
    item.command = { command: opts.command, title: label, arguments: opts.args };
  }
  return item;
}

class DeviceProvider implements vscode.TreeDataProvider<vscode.TreeItem> {
  private emitter = new vscode.EventEmitter<void>();
  readonly onDidChangeTreeData = this.emitter.event;

  refresh(): void {
    this.emitter.fire();
  }

  getTreeItem(item: vscode.TreeItem): vscode.TreeItem {
    return item;
  }

  getChildren(): vscode.TreeItem[] {
    const snapshot = proc.sessionSnapshot();
    const connected = proc.isConnected();
    const busy = proc.isBusy();
    const port = vscode.workspace.getConfiguration('frothy').get<string>('port', '') || 'auto';

    const rows: vscode.TreeItem[] = [];
    if (connected && busy) {
      rows.push(row('Running…', {
        icon: 'loading~spin', command: 'frothy.interrupt', description: 'click to interrupt',
      }));
    } else if (connected) {
      rows.push(row('Connected', {
        icon: 'plug', command: 'frothy.disconnect', description: 'click to disconnect',
      }));
    } else if (snapshot.state === 'syncing') {
      rows.push(row('Connecting…', { icon: 'sync' }));
    } else if (snapshot.state === 'stale' || snapshot.state === 'error') {
      rows.push(row(snapshot.state === 'stale' ? 'Stale' : 'Error', {
        icon: 'warning', command: 'frothy.connect', description: 'click to reconnect',
        tooltip: snapshot.lastError?.message ?? snapshot.lastError?.text,
      }));
    } else {
      rows.push(row('Connect', {
        icon: 'debug-disconnect', command: 'frothy.connect', description: 'click to connect',
      }));
    }

    rows.push(row('Port', { icon: 'arrow-swap', description: port }));
    if (snapshot.profile) rows.push(row('Profile', { icon: 'circuit-board', description: snapshot.profile }));
    if (snapshot.mode) rows.push(row('Mode', { icon: 'gear', description: snapshot.mode }));

    rows.push(
      row('Show Status', { icon: 'info', command: 'frothy.status', tooltip: 'Run status on the device' }),
      row('Memory', { icon: 'graph', command: 'frothy.mem', tooltip: 'Run mem on the device' }),
      row('Save Overlay', { icon: 'save', command: 'frothy.save', tooltip: 'Persist definitions on the device' }),
      row('Restore Overlay', { icon: 'history', command: 'frothy.restore', tooltip: 'Restore persisted definitions' }),
    );
    return rows;
  }
}

class WordsProvider implements vscode.TreeDataProvider<vscode.TreeItem> {
  private emitter = new vscode.EventEmitter<void>();
  readonly onDidChangeTreeData = this.emitter.event;
  private words: string[] = [];
  private inFlight: Promise<void> | undefined;

  get live(): string[] {
    return this.words;
  }

  settled(): Promise<void> {
    return this.inFlight ?? Promise.resolve();
  }

  clear(): void {
    this.words = [];
    this.emitter.fire();
  }

  // Fetching is strictly user-initiated (refresh button, palette command,
  // the empty-state row). The fetch is quiet: no transcript echo, no reply
  // dump. The list is a snapshot from the last refresh, not a live mirror.
  refresh(): Promise<void> {
    if (this.inFlight) return this.inFlight;
    if (!proc.isConnected() || proc.isBusy()) return Promise.resolve();
    this.inFlight = (async () => {
      try {
        this.words = parseWords(await proc.request('words', { quiet: true }));
      } catch {
        // Device busy or session gone mid-request; keep the previous list.
      } finally {
        this.inFlight = undefined;
      }
      this.emitter.fire();
    })();
    return this.inFlight;
  }

  getTreeItem(item: vscode.TreeItem): vscode.TreeItem {
    return item;
  }

  getChildren(): vscode.TreeItem[] {
    if (!proc.isConnected()) {
      return [row('Connect to browse live words', { icon: 'circle-slash', command: 'frothy.connect' })];
    }
    if (this.words.length === 0) {
      return [row('No words yet', { icon: 'refresh', command: 'frothy.refreshWords', description: 'click to refresh' })];
    }
    return this.words.map((word) => row(word, {
      icon: 'symbol-function',
      command: 'frothy.inspectNamed',
      args: [word],
      tooltip: `see ${word}`,
    }));
  }
}

class ProjectProvider implements vscode.TreeDataProvider<vscode.TreeItem> {
  getTreeItem(item: vscode.TreeItem): vscode.TreeItem {
    return item;
  }

  getChildren(): vscode.TreeItem[] {
    return [
      row('Getting Started', {
        icon: 'rocket', command: 'workbench.action.openWalkthrough',
        args: ['NikolaiKozak.frothy#frothy.gettingStarted'],
      }),
      row('New Project', { icon: 'new-folder', command: 'frothy.initProject', tooltip: 'frothy init' }),
      row('Build Firmware', { icon: 'tools', command: 'frothy.build', tooltip: 'frothy build' }),
      row('Flash Firmware', { icon: 'zap', command: 'frothy.flash', tooltip: 'frothy flash <board>' }),
      row('Install Project Library', { icon: 'package', command: 'frothy.install', tooltip: 'frothy install' }),
      row('Open Example', { icon: 'book', command: 'frothy.openExample' }),
      row('Open REPL', { icon: 'terminal', command: 'frothy.openRepl', tooltip: 'frothy connect' }),
      row('Doctor', { icon: 'pulse', command: 'frothy.doctor', tooltip: 'frothy doctor' }),
      row('CLI Menu', { icon: 'list-selection', command: 'frothy.openMenu', tooltip: 'frothy menu' }),
      row('Stop Serial Sessions', { icon: 'stop-circle', command: 'frothy.stopSessions', tooltip: 'frothy stop' }),
      row('Wipe User Definitions', { icon: 'trash', command: 'frothy.wipeUser', tooltip: 'frothy wipe-user' }),
    ];
  }
}

let device: DeviceProvider | undefined;
let words: WordsProvider | undefined;

export function initViews(context: vscode.ExtensionContext): void {
  device = new DeviceProvider();
  words = new WordsProvider();
  context.subscriptions.push(
    vscode.window.registerTreeDataProvider('frothyDevice', device),
    vscode.window.registerTreeDataProvider('frothyWords', words),
    vscode.window.registerTreeDataProvider('frothyProject', new ProjectProvider()),
  );
}

export function refreshDeviceView(): void {
  device?.refresh();
}

export function refreshWords(): Promise<void> {
  return words?.refresh() ?? Promise.resolve();
}

// A manual words fetch briefly occupies the single-flight session slot.
// User actions await this so they never see its "still running" rejection.
export function wordsSettled(): Promise<void> {
  return words?.settled() ?? Promise.resolve();
}

export function clearWords(): void {
  words?.clear();
}

export function liveWords(): string[] {
  return words?.live ?? [];
}
