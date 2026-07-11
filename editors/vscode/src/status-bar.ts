import * as vscode from 'vscode';
import { SessionSnapshot } from './session-records';

let item: vscode.StatusBarItem | undefined;

export function initStatusBar(context: vscode.ExtensionContext): void {
  item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 0);
  item.command = 'frothy.connect';
  item.tooltip = 'Frothy';
  context.subscriptions.push(item);
}

export function updateStatusBar(
  snapshot: Readonly<SessionSnapshot>,
  connected: boolean,
  busy: boolean,
  visible: boolean,
  portLabel: string,
): void {
  if (!item) return;
  if (!visible) {
    item.hide();
    return;
  }

  const profile = snapshot.profile ?? portLabel;
  const details = snapshot.mode ? `${profile}, ${snapshot.mode}` : profile;
  if (connected && busy && snapshot.state === 'interrupting') {
    item.text = '$(debug-stop) Frothy: Interrupting';
    item.backgroundColor = undefined;
    item.command = 'frothy.interrupt';
    item.tooltip = `Frothy is interrupting ${details}`;
  } else if (connected && busy) {
    item.text = '$(loading~spin) Frothy: Running';
    item.backgroundColor = undefined;
    item.command = 'frothy.interrupt';
    item.tooltip = `Frothy is running on ${details} — click to interrupt`;
  } else if (connected) {
    item.text = `$(plug) Frothy: ${profile}`;
    item.backgroundColor = undefined;
    item.command = 'frothy.disconnect';
    item.tooltip = `Frothy connected (${details}) — click to disconnect`;
  } else if (snapshot.state === 'syncing') {
    item.text = '$(sync~spin) Frothy: Connecting';
    item.backgroundColor = undefined;
    item.command = 'frothy.connect';
    item.tooltip = `Frothy is connecting (${portLabel})`;
  } else if (snapshot.state === 'stale' || snapshot.state === 'error') {
    item.text = `$(warning) Frothy: ${snapshot.state === 'stale' ? 'Stale' : 'Error'}`;
    item.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
    item.command = 'frothy.connect';
    item.tooltip = errorTooltip(snapshot);
  } else {
    item.text = '$(circle-slash) Frothy';
    item.backgroundColor = undefined;
    item.command = 'frothy.connect';
    item.tooltip = `Frothy not connected (${portLabel}) — click to connect`;
  }
  item.show();
}

export function disposeStatusBar(): void {
  item?.dispose();
  item = undefined;
}

function errorTooltip(snapshot: Readonly<SessionSnapshot>): string {
  const issue = snapshot.lastError;
  const detail = issue?.message ?? issue?.status ?? issue?.text ?? issue?.code;
  return detail ? `Frothy ${snapshot.state}: ${detail} — click to reconnect` :
    `Frothy ${snapshot.state} — click to reconnect`;
}
