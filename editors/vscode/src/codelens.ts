import * as vscode from 'vscode';
import { splitForms } from './forms';

export function initCodeLens(context: vscode.ExtensionContext): void {
  const changed = new vscode.EventEmitter<void>();
  context.subscriptions.push(
    changed,
    vscode.languages.registerCodeLensProvider({ language: 'frothy' }, {
      onDidChangeCodeLenses: changed.event,
      provideCodeLenses(document) {
        if (!vscode.workspace.getConfiguration('frothy').get<boolean>('codeLens', true)) return [];
        return splitForms(document.getText())
          .filter((form) => form.complete)
          .map((form) => new vscode.CodeLens(
            new vscode.Range(
              document.positionAt(form.startOffset),
              document.positionAt(form.endOffset),
            ),
            {
              title: '▷ Run',
              command: 'frothy.runFormAt',
              arguments: [document.uri, form.startLine],
            },
          ));
      },
    }),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('frothy.codeLens')) changed.fire();
    }),
  );
}
