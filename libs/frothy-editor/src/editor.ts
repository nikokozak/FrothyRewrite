// mountEditor — assembles CodeMirror + transcript + storage + TR
// connector into a single web surface a stranger can use in five
// minutes. The host page passes a mount element; everything else has
// sane defaults.

import { EditorState } from "@codemirror/state";
import { EditorView, keymap, lineNumbers } from "@codemirror/view";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { createConnector, WebSerialTransport } from "@frothy/repl";
import type { ReplConnector, Response, Status } from "@frothy/repl";

import { FROTHY_EXAMPLES } from "./examples.generated.js";
import { formAt, splitForms } from "./forms.js";
import { frothyLanguage, frothyHighlight } from "./highlight.js";
import { mountTranscript } from "./transcript.js";
import type { Transcript } from "./transcript.js";
import { makeStorage } from "./storage.js";
import type { SketchStorage } from "./storage.js";

export const DEFAULT_INITIAL_SOURCE = `-- Welcome to Frothy. Edit, then Run File (Cmd/Ctrl+Shift+Enter).
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
  runForm(): Promise<void>;
  runFile(): Promise<void>;
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
  > & { confirm?: (message?: string) => boolean };
  const timerTarget = (doc.defaultView ?? globalThis) as Pick<
    Window,
    "setTimeout" | "clearTimeout"
  >;
  const storage: SketchStorage = makeStorage(opts.storageKey);
  const initial = storage.load() ?? opts.initialSource ?? DEFAULT_INITIAL_SOURCE;
  let saveTimer: ReturnType<Window["setTimeout"]> | null = null;
  let sketchFilename = "sketch.fr";

  const root = doc.createElement("div");
  root.className = "frothy-editor";

  const header = doc.createElement("div");
  header.className = "frothy-editor-header";
  const title = doc.createElement("div");
  title.className = "frothy-editor-title";
  title.textContent = "Frothy editor";
  const examplesLabel = doc.createElement("label");
  examplesLabel.className = "frothy-example-picker";
  examplesLabel.textContent = "Example ";
  const examplesSelect = doc.createElement("select");
  const examplesPlaceholder = doc.createElement("option");
  examplesPlaceholder.value = "";
  examplesPlaceholder.textContent = "Examples...";
  examplesPlaceholder.disabled = true;
  examplesPlaceholder.selected = true;
  examplesSelect.append(examplesPlaceholder);
  for (const example of FROTHY_EXAMPLES) {
    const option = doc.createElement("option");
    option.value = example.name;
    option.textContent = example.title;
    examplesSelect.append(option);
  }
  examplesLabel.append(examplesSelect);
  const splitBtn = mkBtn(doc, "Split", "frothy-btn frothy-split-toggle");
  const connectBtn = doc.createElement("button");
  connectBtn.className = "frothy-btn frothy-btn-primary";
  connectBtn.textContent = "Connect";
  header.append(title, examplesLabel, splitBtn, connectBtn);

  const editorHost = doc.createElement("div");
  editorHost.className = "frothy-editor-pane";

  const commandBar = doc.createElement("div");
  commandBar.className = "frothy-editor-cmdbar";
  const runFormBtn = mkBtn(doc, "Run Form", "frothy-btn");
  const runFileBtn = mkBtn(doc, "Run File", "frothy-btn");
  const interruptBtn = mkBtn(doc, "Interrupt", "frothy-btn");
  const clearOutputBtn = mkBtn(doc, "Clear output", "frothy-btn");
  const openBtn = mkBtn(doc, "Open .fr", "frothy-btn");
  const downloadBtn = mkBtn(doc, "Download .fr", "frothy-btn");
  const fileInput = doc.createElement("input");
  fileInput.type = "file";
  fileInput.accept = ".fr,.frothy,text/plain";
  fileInput.hidden = true;
  const saveStatus = doc.createElement("span");
  saveStatus.className = "frothy-save-status";
  saveStatus.setAttribute("role", "status");
  saveStatus.setAttribute("aria-live", "polite");
  saveStatus.textContent = "not saved locally";
  const echoToggle = doc.createElement("label");
  echoToggle.className = "frothy-echo-toggle";
  const echoBox = doc.createElement("input");
  echoBox.type = "checkbox";
  echoBox.checked = true;
  echoBox.addEventListener("change", () => {
    suppressEcho = echoBox.checked;
  });
  echoToggle.append(echoBox, doc.createTextNode(" Hide echo"));
  commandBar.append(
    runFormBtn,
    runFileBtn,
    interruptBtn,
    clearOutputBtn,
    openBtn,
    downloadBtn,
    fileInput,
    saveStatus,
    echoToggle,
  );

  const transcriptHost = doc.createElement("div");
  transcriptHost.className = "frothy-transcript-host";
  const transcript: Transcript = mountTranscript(transcriptHost);

  // The editor + its command bar form one column; the transcript (MCU output)
  // sits beside or below it. The body flips between the two via a root class.
  const editorMain = doc.createElement("div");
  editorMain.className = "frothy-editor-main";
  editorMain.append(editorHost, commandBar);

  const body = doc.createElement("div");
  body.className = "frothy-editor-body";
  body.append(editorMain, transcriptHost);

  let layout = loadLayout();
  function applyLayout(): void {
    const horizontal = layout === "horizontal";
    root.classList.toggle("frothy-editor--horizontal", horizontal);
    splitBtn.textContent = horizontal ? "Stack" : "Split";
    splitBtn.title = horizontal
      ? "Stack the output below the editor"
      : "Put the output beside the editor";
  }
  splitBtn.addEventListener("click", () => {
    layout = layout === "horizontal" ? "vertical" : "horizontal";
    saveLayout(layout);
    applyLayout();
  });
  applyLayout();

  root.append(header, body);
  opts.mount.appendChild(root);

  const view = new EditorView({
    state: EditorState.create({
      doc: initial,
      extensions: [
        lineNumbers(),
        history(),
        frothyLanguage(),
        frothyHighlight(),
        EditorView.updateListener.of((update) => {
          if (update.docChanged) scheduleSave();
        }),
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
    runFormBtn.disabled = !connected;
    runFileBtn.disabled = !connected;
    interruptBtn.disabled = !connected;
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
    transcript.note("device disconnected");
  }

  function releaseBeforeUnload() {
    save();
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
        if (isDeviceErrorLine(line)) pendingEcho = null;
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

  function currentSource(): string {
    return view.state.doc.toString();
  }

  function replaceSource(src: string): void {
    view.dispatch({
      changes: { from: 0, to: view.state.doc.length, insert: src },
    });
  }

  async function runForm(): Promise<void> {
    if (!repl) return;
    const selection = view.state.selection.main;
    let source: string | undefined;
    if (!selection.empty) {
      const forms = splitForms(view.state.sliceDoc(selection.from, selection.to));
      if (forms.length === 1 && forms[0].complete) source = forms[0].source;
    } else {
      const line = view.state.doc.lineAt(selection.head).number - 1;
      const form = formAt(currentSource(), line);
      if (form?.complete) source = form.source;
    }
    if (!source) {
      transcript.note("select one complete form or Run File");
      return;
    }
    await sendForm(source);
  }

  async function runFile(): Promise<void> {
    if (!repl) return;
    const forms = splitForms(currentSource());
    if (forms.length === 0) {
      transcript.note("(empty file)");
      return;
    }
    if (forms.some((form) => !form.complete)) {
      transcript.note("finish the incomplete form before running this file");
      return;
    }
    for (const form of forms) {
      const response = await sendForm(form.source);
      if (!response) return;
      if (response.kind === "error") {
        transcript.note("stopped: form errored");
        return;
      }
    }
  }

  async function sendForm(source: string): Promise<Response | null> {
    if (!repl) return null;
    transcript.appendHost(source);
    pendingEcho = source;
    try {
      return await repl.sendLine(source);
    } catch (err) {
      reportError(err);
      return null;
    }
  }

  async function interrupt(): Promise<void> {
    try {
      await repl?.interrupt();
    } catch (err) {
      reportError(err);
    }
  }

  function clearSaveTimer(): void {
    if (saveTimer === null) return;
    timerTarget.clearTimeout(saveTimer);
    saveTimer = null;
  }

  function scheduleSave(): void {
    saveStatus.textContent = "editing...";
    clearSaveTimer();
    saveTimer = timerTarget.setTimeout(() => {
      saveTimer = null;
      save();
    }, 700);
  }

  function save() {
    clearSaveTimer();
    saveStatus.textContent = storage.save(currentSource())
      ? "saved locally"
      : "not saved—download .fr";
  }

  function download() {
    storage.download(currentSource(), sketchFilename);
  }

  async function openSelectedFile(): Promise<void> {
    const file = fileInput.files?.[0];
    fileInput.value = "";
    if (!file) return;
    try {
      const source = await file.text();
      replaceSource(source);
      sketchFilename = basename(file.name);
      view.focus();
    } catch (err) {
      reportError(err);
    }
  }

  connectBtn.addEventListener("click", () => {
    if (repl) {
      void disconnect();
    } else {
      void connect();
    }
  });
  runFormBtn.addEventListener("click", () => void runForm());
  runFileBtn.addEventListener("click", () => void runFile());
  interruptBtn.addEventListener("click", () => void interrupt());
  clearOutputBtn.addEventListener("click", () => transcript.clear());
  openBtn.addEventListener("click", () => fileInput.click());
  fileInput.addEventListener("change", () => void openSelectedFile());
  downloadBtn.addEventListener("click", () => download());
  examplesSelect.addEventListener("change", () => {
    const example = FROTHY_EXAMPLES.find((entry) => entry.name === examplesSelect.value);
    if (!example) {
      examplesSelect.value = "";
      return;
    }
    const allowed = !shouldConfirmReplace(currentSource(), example.source)
      || (unloadTarget.confirm?.("Replace your current sketch?") ?? true);
    if (!allowed) {
      examplesSelect.value = "";
      view.focus();
      return;
    }
    replaceSource(example.source);
    sketchFilename = `${example.name}.fr`;
    examplesSelect.value = "";
    view.focus();
  });

  // Keyboard: Cmd/Ctrl+Enter runs one form; add Shift to run the file.
  editorHost.addEventListener(
    "keydown",
    (ev) => {
      if (ev.key === "Enter" && (ev.metaKey || ev.ctrlKey)) {
        ev.preventDefault();
        if (ev.shiftKey) void runFile();
        else void runForm();
      }
    },
    { capture: true },
  );

  return {
    getSource() {
      return view.state.doc.toString();
    },
    setSource(src: string) {
      replaceSource(src);
    },
    connect,
    disconnect,
    runForm,
    runFile,
    save,
    download,
    transcript() {
      return repl?.transcript() ?? [];
    },
    destroy() {
      unloadTarget.removeEventListener("beforeunload", releaseBeforeUnload);
      // Flush any debounced edit before teardown — destroy() also fires from the
      // web component's disconnectedCallback on ordinary DOM removal, not just
      // page unload, so a pending save would otherwise be silently dropped.
      if (saveTimer !== null) save();
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

function isDeviceErrorLine(line: string): boolean {
  return line.startsWith("error:") || /^err \d/.test(line);
}

function basename(path: string): string {
  return path.split(/[/\\]/).pop() || "sketch.fr";
}

const LAYOUT_KEY = "frothy-editor:layout";
type Layout = "vertical" | "horizontal";

function loadLayout(): Layout {
  try {
    return globalThis.localStorage?.getItem(LAYOUT_KEY) === "horizontal"
      ? "horizontal"
      : "vertical";
  } catch {
    return "vertical";
  }
}

function saveLayout(layout: Layout): void {
  try {
    globalThis.localStorage?.setItem(LAYOUT_KEY, layout);
  } catch {
    // localStorage may be unavailable (private mode, no DOM). Non-fatal.
  }
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

export function shouldConfirmReplace(current: string, incoming: string): boolean {
  const trimmedCurrent = current.trim();
  return trimmedCurrent.length > 0
    && trimmedCurrent !== incoming.trim()
    && trimmedCurrent !== DEFAULT_INITIAL_SOURCE.trim();
}
