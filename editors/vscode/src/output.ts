import * as vscode from 'vscode';

let channel: vscode.OutputChannel | undefined;
let partial = '';
let flushTimer: NodeJS.Timeout | undefined;

// How long to wait for the device to send a '\n' before flushing what we've
// got. The CLI's `frothy> ` prompt and the device's `> ` prompt both arrive
// without a trailing newline; without this, the user sees no output after a
// successful Connect (or after a command that leaves bytes past the last \n).
const FLUSH_DEBOUNCE_MS = 150;

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
// the next chunk, an explicit flush, or the debounce fires.
export function appendChunk(chunk: string): void {
  const buf = partial + chunk;
  const lines = buf.split('\n');
  partial = lines.pop() ?? '';
  const out = getChannel();
  for (const line of lines) {
    out.appendLine(stripCR(line));
  }
  if (flushTimer) {
    clearTimeout(flushTimer);
    flushTimer = undefined;
  }
  if (partial.length > 0) {
    flushTimer = setTimeout(() => {
      flushTimer = undefined;
      flushPartial();
    }, FLUSH_DEBOUNCE_MS);
  }
}

export function flushPartial(): void {
  if (flushTimer) {
    clearTimeout(flushTimer);
    flushTimer = undefined;
  }
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
