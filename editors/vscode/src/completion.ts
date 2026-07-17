import * as vscode from 'vscode';
import { liveWords } from './views';

// Completion offers what is actually defined on the connected chip right now —
// the Words view's cache, no LSP, no static index.
export function initCompletion(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider('frothy', {
      provideCompletionItems() {
        return liveWords().map((word) => {
          const item = new vscode.CompletionItem(word, vscode.CompletionItemKind.Function);
          item.detail = 'live on device';
          return item;
        });
      },
    }),
  );
}
