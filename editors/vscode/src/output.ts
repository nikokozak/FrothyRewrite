import * as vscode from 'vscode';

let channel: vscode.OutputChannel | undefined;
let partial = '';

export function getChannel(): vscode.OutputChannel {
  if (!channel) {
    channel = vscode.window.createOutputChannel('Frothy');
  }
  return channel;
}

export function show(): void {
  getChannel().show(true);
}

export function appendLine(text: string): void {
  getChannel().appendLine(text);
}

// Split incoming child-process bytes on '\n' so each device line lands as
// its own OutputChannel line. Anything past the last '\n' stays held until
// the next chunk or an explicit flush.
export function appendChunk(chunk: string): void {
  const buf = partial + chunk;
  const lines = buf.split('\n');
  partial = lines.pop() ?? '';
  const out = getChannel();
  for (const line of lines) {
    out.appendLine(stripCR(line));
  }
}

export function flushPartial(): void {
  if (partial.length === 0) return;
  getChannel().appendLine(stripCR(partial));
  partial = '';
}

export function dispose(): void {
  flushPartial();
  channel?.dispose();
  channel = undefined;
}

function stripCR(s: string): string {
  return s.endsWith('\r') ? s.slice(0, -1) : s;
}
