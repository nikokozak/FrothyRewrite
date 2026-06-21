// Transcript pane — a small vanilla-DOM list that classifies each
// device line as request / response / error / ok / prompt and styles
// accordingly. No framework, no virtual DOM.

const HOST_PREFIX = "> ";

export interface Transcript {
  element: HTMLElement;
  appendHost(line: string): void;
  appendDevice(line: string): void;
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

  function append(html: HTMLElement): void {
    root.appendChild(html);
    root.scrollTop = root.scrollHeight;
  }

  return {
    element: root,
    appendHost(line) {
      const row = root.ownerDocument.createElement("div");
      row.className = "transcript-host";
      row.textContent = HOST_PREFIX + line;
      append(row);
    },
    appendDevice(line) {
      const row = root.ownerDocument.createElement("div");
      row.className = classifyDevice(line);
      row.textContent = line;
      append(row);
    },
    clear() {
      root.replaceChildren();
    },
  };
}
