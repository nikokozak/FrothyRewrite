// mountEditor — assembles CodeMirror + transcript + storage + TR
// connector into a single web surface a stranger can use in five
// minutes. The host page passes a mount element; everything else has
// sane defaults.

import { EditorState } from "@codemirror/state";
import { EditorView, keymap, lineNumbers } from "@codemirror/view";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { createConnector, WebSerialTransport } from "@frothy/repl";
import type { ReplConnector, Status } from "@frothy/repl";

import { frothyLanguage, frothyHighlight } from "./highlight.js";
import { mountTranscript } from "./transcript.js";
import type { Transcript } from "./transcript.js";
import { makeStorage } from "./storage.js";
import type { SketchStorage } from "./storage.js";

export const DEFAULT_INITIAL_SOURCE = `-- Welcome to Frothy. Edit, then Send buffer (Cmd/Ctrl+Enter).
to greet [ "hello, world" ]
greet:
`;

export interface EditorOptions {
  mount: HTMLElement;
  initialSource?: string;
  storageKey?: string;
  onConnect?: (status: Status) => void;
  onError?: (err: Error) => void;
}

export interface EditorHandle {
  getSource(): string;
  setSource(src: string): void;
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  sendLine(): Promise<void>;
  sendBuffer(): Promise<void>;
  save(): void;
  download(): void;
  transcript(): readonly string[];
  destroy(): void;
}

export function mountEditor(opts: EditorOptions): EditorHandle {
  const doc = opts.mount.ownerDocument;
  const unloadTarget = (doc.defaultView ?? globalThis) as Pick<
    Window,
    "addEventListener" | "removeEventListener"
  >;
  const storage: SketchStorage = makeStorage(opts.storageKey);
  const initial = storage.load() ?? opts.initialSource ?? DEFAULT_INITIAL_SOURCE;

  const root = doc.createElement("div");
  root.className = "frothy-editor";

  const header = doc.createElement("div");
  header.className = "frothy-editor-header";
  const title = doc.createElement("div");
  title.className = "frothy-editor-title";
  title.textContent = "Frothy editor";
  const connectBtn = doc.createElement("button");
  connectBtn.className = "frothy-btn frothy-btn-primary";
  connectBtn.textContent = "Connect";
  header.append(title, connectBtn);

  const editorHost = doc.createElement("div");
  editorHost.className = "frothy-editor-pane";

  const commandBar = doc.createElement("div");
  commandBar.className = "frothy-editor-cmdbar";
  const sendLineBtn = mkBtn(doc, "Send line", "frothy-btn");
  const sendBufBtn = mkBtn(doc, "Send buffer", "frothy-btn");
  const saveBtn = mkBtn(doc, "Save", "frothy-btn");
  const downloadBtn = mkBtn(doc, "Download .fr", "frothy-btn");
  const echoToggle = doc.createElement("label");
  echoToggle.className = "frothy-echo-toggle";
  const echoBox = doc.createElement("input");
  echoBox.type = "checkbox";
  echoBox.checked = true;
  echoBox.addEventListener("change", () => {
    suppressEcho = echoBox.checked;
  });
  echoToggle.append(echoBox, doc.createTextNode(" Hide echo"));
  commandBar.append(sendLineBtn, sendBufBtn, saveBtn, downloadBtn, echoToggle);

  const transcriptHost = doc.createElement("div");
  transcriptHost.className = "frothy-transcript-host";
  const transcript: Transcript = mountTranscript(transcriptHost);

  root.append(header, editorHost, commandBar, transcriptHost);
  opts.mount.appendChild(root);

  const view = new EditorView({
    state: EditorState.create({
      doc: initial,
      extensions: [
        lineNumbers(),
        history(),
        frothyLanguage(),
        frothyHighlight(),
        keymap.of([...defaultKeymap, ...historyKeymap]),
      ],
    }),
    parent: editorHost,
  });

  let repl: ReplConnector | null = null;
  // The raw serial port, retained only so releaseBeforeUnload can close it in
  // a single synchronous hop (see there). The connector/transport own the
  // normal teardown path.
  let currentPort: { close(): Promise<void> } | null = null;
  let unsubscribeLines: (() => void) | null = null;
  let unsubscribeClose: (() => void) | null = null;
  // The device runs an interactive line editor that echoes each input line
  // back as the first response line. With "Hide echo" on (default), we swallow
  // exactly one echoed line per request — set to the line we just sent —
  // so sketch input isn't shown twice. Matching one line means a genuine value
  // equal to the input (send `7` → echo `7` → result `7`) still shows.
  let suppressEcho = true;
  let pendingEcho: string | null = null;

  function setConnected(label: string, connected: boolean) {
    connectBtn.textContent = label;
    connectBtn.classList.toggle("frothy-btn-connected", connected);
    sendLineBtn.disabled = !connected;
    sendBufBtn.disabled = !connected;
  }

  function reportError(err: unknown) {
    const e = err instanceof Error ? err : new Error(String(err));
    transcript.appendDevice(`error: ${e.message}`);
    if (opts.onError) opts.onError(e);
  }

  function unsubscribeReplEvents() {
    if (unsubscribeLines) {
      unsubscribeLines();
      unsubscribeLines = null;
    }
    if (unsubscribeClose) {
      unsubscribeClose();
      unsubscribeClose = null;
    }
  }

  function handleConnectorClose(closedRepl: ReplConnector) {
    if (repl !== closedRepl) return;
    unsubscribeReplEvents();
    repl = null;
    currentPort = null;
    pendingEcho = null;
    setConnected("Connect", false);
    transcript.appendDevice("device disconnected");
  }

  function releaseBeforeUnload() {
    // beforeunload cannot await, so go straight to the port's synchronous
    // close() — the single hop the flasher uses (frothy-site flash/app.js).
    // Routing through repl.close() would gate the real port.close() behind two
    // awaited hops (reader.cancel → released → port.close) the browser does not
    // guarantee to run on unload, leaving the OS serial FD held and breaking
    // the next connect.
    if (!currentPort) return;
    try {
      void currentPort.close();
    } catch {
      // best effort only
    }
  }

  setConnected("Connect", false);
  unloadTarget.addEventListener("beforeunload", releaseBeforeUnload);

  async function connect(): Promise<void> {
    const nav = globalThis.navigator;
    if (!nav?.serial) {
      reportError(new Error("Web Serial requires Chrome or Edge"));
      return;
    }
    try {
      const port = await nav.serial.requestPort();
      await port.open({ baudRate: 115200 });
      currentPort = port;
      repl = await createConnector(new WebSerialTransport(port));
      const connectedRepl = repl;
      unsubscribeLines = repl.onLine((line) => {
        if (suppressEcho && pendingEcho !== null && line === pendingEcho) {
          pendingEcho = null;
          return;
        }
        transcript.appendDevice(line);
      });
      unsubscribeClose = connectedRepl.onClose(() => handleConnectorClose(connectedRepl));
      pendingEcho = "status";
      const status = await repl.status();
      setConnected(`connected · ${status.profile}`, true);
      if (opts.onConnect) opts.onConnect(status);
    } catch (err) {
      if (repl) await disconnect();
      reportError(err);
      setConnected("Connect", false);
    }
  }

  async function disconnect(): Promise<void> {
    const closing = repl;
    unsubscribeReplEvents();
    repl = null;
    currentPort = null;
    pendingEcho = null;
    if (closing) await closing.close().catch(() => undefined);
    setConnected("Connect", false);
  }

  function currentLine(): string {
    const head = view.state.selection.main.head;
    const line = view.state.doc.lineAt(head);
    return line.text;
  }

  async function sendLine(): Promise<void> {
    if (!repl) return;
    const [line] = sendableLines(currentLine());
    if (!line) return;
    transcript.appendHost(line);
    pendingEcho = line;
    try {
      await repl.sendLine(line);
    } catch (err) {
      reportError(err);
    }
  }

  async function sendBuffer(): Promise<void> {
    if (!repl) return;
    const lines = sendableLines(view.state.doc.toString());
    if (lines.length === 0) {
      transcript.appendDevice("(empty buffer)");
      return;
    }
    for (const line of lines) {
      transcript.appendHost(line);
      pendingEcho = line;
      try {
        await repl.sendLine(line);
      } catch (err) {
        reportError(err);
        return;
      }
    }
  }

  function save() {
    storage.save(view.state.doc.toString());
  }

  function download() {
    storage.download(view.state.doc.toString(), "sketch.fr");
  }

  connectBtn.addEventListener("click", () => {
    if (repl) {
      void disconnect();
    } else {
      void connect();
    }
  });
  sendLineBtn.addEventListener("click", () => void sendLine());
  sendBufBtn.addEventListener("click", () => void sendBuffer());
  saveBtn.addEventListener("click", () => save());
  downloadBtn.addEventListener("click", () => download());

  // Keyboard: Shift+Enter sends current line; Cmd/Ctrl+Enter sends buffer.
  editorHost.addEventListener(
    "keydown",
    (ev) => {
      if (ev.key === "Enter" && ev.shiftKey) {
        ev.preventDefault();
        void sendLine();
        return;
      }
      if (ev.key === "Enter" && (ev.metaKey || ev.ctrlKey)) {
        ev.preventDefault();
        void sendBuffer();
      }
    },
    { capture: true },
  );

  return {
    getSource() {
      return view.state.doc.toString();
    },
    setSource(src: string) {
      view.dispatch({
        changes: { from: 0, to: view.state.doc.length, insert: src },
      });
    },
    connect,
    disconnect,
    sendLine,
    sendBuffer,
    save,
    download,
    transcript() {
      return repl?.transcript() ?? [];
    },
    destroy() {
      unloadTarget.removeEventListener("beforeunload", releaseBeforeUnload);
      void disconnect();
      view.destroy();
      opts.mount.removeChild(root);
    },
  };
}

function mkBtn(doc: Document, label: string, cls: string): HTMLButtonElement {
  const b = doc.createElement("button");
  b.className = cls;
  b.textContent = label;
  return b;
}

export function sendableLines(text: string): string[] {
  const lines: string[] = [];
  for (const raw of text.split("\n")) {
    const line = raw.trim();
    if (line.length === 0 || line.startsWith("--")) continue;
    lines.push(line);
  }
  return lines;
}
