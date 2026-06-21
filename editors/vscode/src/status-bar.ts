import * as vscode from 'vscode';

let item: vscode.StatusBarItem | undefined;

export function initStatusBar(context: vscode.ExtensionContext): void {
  item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 0);
  item.command = 'frothy.connect';
  item.tooltip = 'Frothy — click to connect / disconnect';
  context.subscriptions.push(item);
  updateStatusBar(false, '');
}

export function updateStatusBar(connected: boolean, portLabel: string): void {
  if (!item) return;
  if (connected) {
    item.text = `$(plug) Frothy: ${portLabel}`;
    item.backgroundColor = undefined;
    item.command = 'frothy.disconnect';
    item.tooltip = `Frothy connected (${portLabel}) — click to disconnect`;
  } else {
    item.text = '$(circle-slash) Frothy';
    item.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
    item.command = 'frothy.connect';
    item.tooltip = 'Frothy not connected — click to connect';
  }
  item.show();
}

export function disposeStatusBar(): void {
  item?.dispose();
  item = undefined;
}
