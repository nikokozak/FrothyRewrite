import * as vscode from 'vscode';
import { liveWords } from './views';

// Completion offers the device vocabulary from the last manual Words
// refresh — the Words view's cache, no LSP, no static index.
export function initCompletion(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider('frothy', {
      provideCompletionItems() {
        return liveWords().map((word) => {
          const item = new vscode.CompletionItem(word, vscode.CompletionItemKind.Function);
          item.detail = 'on device (last refresh)';
          return item;
        });
      },
    }),
  );
}
