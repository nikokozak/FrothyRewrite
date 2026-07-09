// Transcript pane — a small vanilla-DOM list that classifies each
// device line as request / response / error / ok / prompt and styles
// accordingly. No framework, no virtual DOM.

const HOST_PREFIX = "> ";

export interface Transcript {
  element: HTMLElement;
  appendHost(line: string): void;
  appendDevice(line: string): void;
  /** A client-side system message (not device output). Ends any open
   *  diagnostic group and renders plainly, so it never reads as an error detail. */
  note(line: string): void;
  clear(): void;
}

function classifyDevice(line: string): string {
  if (line === "ok" || line.endsWith(" ok")) return "transcript-ok";
  if (line.startsWith("error:") || /^err \d/.test(line)) return "transcript-error";
  if (line.startsWith("frothy status v")) return "transcript-status";
  return "transcript-line";
}

export function mountTranscript(host: HTMLElement): Transcript {
  const root = host.ownerDocument.createElement("div");
  root.className = "frothy-transcript";
  host.appendChild(root);
  let inDiagnostic = false;

  function append(html: HTMLElement): void {
    root.appendChild(html);
    root.scrollTop = root.scrollHeight;
  }

  return {
    element: root,
    appendHost(line) {
      inDiagnostic = false;
      const row = root.ownerDocument.createElement("div");
      row.className = "transcript-host";
      row.textContent = HOST_PREFIX + line;
      append(row);
    },
    appendDevice(line) {
      const row = root.ownerDocument.createElement("div");
      const cls = classifyDevice(line);
      // Async event lines ("! ...") are never part of a diagnostic: an error's
      // detail block ends at a bare prompt (no line), so without this an event
      // firing afterward would latch onto the stale error's group.
      if (line.startsWith("! ")) {
        inDiagnostic = false;
        row.className = cls;
      } else if (cls === "transcript-error") {
        inDiagnostic = true;
        row.className = cls;
      } else if (inDiagnostic && cls === "transcript-line") {
        row.className = "transcript-detail";
      } else {
        inDiagnostic = false;
        row.className = cls;
      }
      row.textContent = line;
      append(row);
    },
    note(line) {
      inDiagnostic = false;
      const row = root.ownerDocument.createElement("div");
      row.className = "transcript-note";
      row.textContent = line;
      append(row);
    },
    clear() {
      inDiagnostic = false;
      root.replaceChildren();
    },
  };
}
