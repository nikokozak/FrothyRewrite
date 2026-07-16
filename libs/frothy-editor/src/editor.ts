// mountEditor — assembles CodeMirror + transcript + storage + TR
// connector into a single web surface a stranger can use in five
// minutes. The host page passes a mount element; everything else has
// sane defaults.

import { EditorState } from "@codemirror/state";
import { EditorView, keymap, lineNumbers } from "@codemirror/view";
import {
  defaultKeymap,
  history,
  historyKeymap,
  indentWithTab,
} from "@codemirror/commands";
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
  documentId?: string;
  storageKey?: string;
  storage?: SketchStorage;
  resolveProject?: (currentSource: string) => Promise<ResolvedSource[]>;
  onConnect?: (status: Status) => void;
  onError?: (err: Error) => void;
}

export interface ResolvedSource {
  path: string;
  source: string;
}

export interface EditorHandle {
  getSource(): string;
  setSource(src: string): void;
  openDocument(documentId: string, source: string): void;
  renameDocument(from: string, to: string): void;
  forgetDocument(documentId: string): void;
  resetDocument(documentId: string, source: string): void;
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  runForm(): Promise<void>;
  runFile(): Promise<void>;
  runProject(): Promise<void>;
  save(): void;
  download(): void;
  transcript(): readonly string[];
  destroy(): void;
}

type EditorSessionState = "disconnected" | "connecting" | "idle" | "running";

const CONNECT_TIMEOUT_MS = 8000;

export function displayProfileName(profile: string): string {
  if (profile === "esp32_plain") return "ESP32 Default";
  return profile;
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
  const storage: SketchStorage = opts.storage ?? makeStorage(opts.storageKey);
  const initial = storage.load() ?? opts.initialSource ?? DEFAULT_INITIAL_SOURCE;
  let saveTimer: ReturnType<Window["setTimeout"]> | null = null;
  let currentDocumentId = opts.documentId ?? "sketch.fr";
  let sketchFilename = basename(currentDocumentId);
  const documentStates = new Map<string, EditorState>();

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

  const connectionStatus = doc.createElement("div");
  connectionStatus.className = "frothy-connection-status";
  connectionStatus.setAttribute("role", "status");
  connectionStatus.setAttribute("aria-live", "polite");

  const editorHost = doc.createElement("div");
  editorHost.className = "frothy-editor-pane";

  const commandBar = doc.createElement("div");
  commandBar.className = "frothy-editor-cmdbar";
  const runFormBtn = mkBtn(doc, "Run Form", "frothy-btn");
  runFormBtn.title = "Run the selection, or the complete form at the cursor";
  const runFileBtn = mkBtn(doc, "Run File", "frothy-btn");
  const runProjectBtn = opts.resolveProject
    ? mkBtn(doc, "Run Project", "frothy-btn frothy-btn-primary")
    : null;
  const interruptBtn = mkBtn(doc, "Interrupt", "frothy-btn");
  const browseWordsBtn = mkBtn(doc, "Browse Words", "frothy-btn");
  const clearOutputBtn = mkBtn(doc, "Clear output", "frothy-btn");
  const openBtn = mkBtn(doc, "Open .fr", "frothy-btn");
  const downloadBtn = mkBtn(doc, "Download file", "frothy-btn");
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
  commandBar.append(runFormBtn, runFileBtn);
  if (runProjectBtn) commandBar.append(runProjectBtn);
  commandBar.append(
    interruptBtn,
    browseWordsBtn,
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

  const wordsDialog = doc.createElement("dialog");
  wordsDialog.className = "frothy-word-dialog";
  wordsDialog.setAttribute("aria-label", "Live Frothy words");
  const wordsTitle = doc.createElement("h2");
  wordsTitle.textContent = "Live words";
  const wordsSearchLabel = doc.createElement("label");
  wordsSearchLabel.textContent = "Search ";
  const wordsSearch = doc.createElement("input");
  wordsSearch.type = "search";
  wordsSearch.autocapitalize = "none";
  wordsSearch.spellcheck = false;
  wordsSearchLabel.append(wordsSearch);
  const wordsList = doc.createElement("div");
  wordsList.className = "frothy-word-list";
  const closeWordsBtn = mkBtn(doc, "Close", "frothy-btn");
  wordsDialog.append(wordsTitle, wordsSearchLabel, wordsList, closeWordsBtn);

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

  root.append(header, connectionStatus, body, wordsDialog);
  opts.mount.appendChild(root);

  function createEditorState(source: string): EditorState {
    return EditorState.create({
      doc: source,
      extensions: [
        lineNumbers(),
        history(),
        frothyLanguage(),
        frothyHighlight(),
        EditorView.updateListener.of((update) => {
          if (update.docChanged) scheduleSave();
        }),
        keymap.of([indentWithTab, ...defaultKeymap, ...historyKeymap]),
      ],
    });
  }

  const view = new EditorView({
    state: createEditorState(initial),
    parent: editorHost,
  });

  let repl: ReplConnector | null = null;
  // The raw serial port, retained only so releaseBeforeUnload can close it in
  // a single synchronous hop (see there). The connector/transport own the
  // normal teardown path.
  let currentPort: { close(): Promise<void> } | null = null;
  let unsubscribeLines: (() => void) | null = null;
  let unsubscribeClose: (() => void) | null = null;
  // The device echoes ordinary one-line requests such as status. With
  // "Hide echo" on (default), swallow exactly one matching line so those
  // requests are not shown twice. The connector hides source-form's escaped
  // wire echo itself, so transport framing never appears as sketch output. A
  // genuine value equal to the input (send `7` → echo `7` → result `7`) still
  // shows.
  let suppressEcho = true;
  let pendingEcho: string | null = null;
  let sessionState: EditorSessionState = "disconnected";
  let connectedProfile = "";

  function setSessionState(
    next: EditorSessionState,
    profile?: string,
    message?: string,
  ): void {
    sessionState = next;
    if (profile) connectedProfile = profile;
    if (next === "disconnected") connectedProfile = "";
    const connected = next === "idle" || next === "running";
    root.dataset.state = next;
    root.setAttribute("aria-busy", String(next === "connecting" || next === "running"));
    connectBtn.textContent = next === "connecting"
      ? "Connecting…"
      : connected ? `Connected · ${displayProfileName(connectedProfile)}` : "Connect";
    connectBtn.disabled = next === "connecting";
    connectBtn.classList.toggle("frothy-btn-connected", connected);
    interruptBtn.disabled = next !== "running";
    interruptBtn.classList.toggle("frothy-btn-primary", next === "running");
    browseWordsBtn.hidden = !connected;
    browseWordsBtn.disabled = next !== "idle";
    updateDocumentControls();
    connectionStatus.textContent = message ?? (
      next === "connecting"
        ? "Opening the board and checking for Frothy…"
        : next === "running"
          ? "Frothy is running. Use Interrupt to stop a long-running form."
          : next === "idle"
            ? `Connected to ${displayProfileName(connectedProfile)}.`
            : "Not connected."
    );
    connectionStatus.classList.toggle(
      "frothy-connection-status--error",
      Boolean(message),
    );
    if (!connected && wordsDialog.open) wordsDialog.close();
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
    setSessionState("disconnected");
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

  setSessionState("disconnected");
  unloadTarget.addEventListener("beforeunload", releaseBeforeUnload);

  async function connect(): Promise<void> {
    if (sessionState !== "disconnected") return;
    setSessionState("connecting");
    const nav = globalThis.navigator;
    if (!nav?.serial) {
      reportError(new Error("Web Serial requires Chrome or Edge"));
      setSessionState("disconnected");
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
      let timeoutId: number | undefined;
      const status = await Promise.race([
        repl.status(),
        new Promise<never>((_, reject) => {
          timeoutId = timerTarget.setTimeout(
            () => reject(new Error("Frothy did not answer the status check")),
            CONNECT_TIMEOUT_MS,
          );
        }),
      ]).finally(() => {
        if (timeoutId !== undefined) timerTarget.clearTimeout(timeoutId);
      });
      if (repl !== connectedRepl) return;
      setSessionState("idle", status.profile);
      if (opts.onConnect) opts.onConnect(status);
    } catch (err) {
      if (repl) {
        await disconnect();
      } else if (currentPort) {
        const openPort = currentPort;
        currentPort = null;
        await openPort.close().catch(() => undefined);
      }
      const cancelled = err instanceof Error && err.name === "NotFoundError";
      if (cancelled) return;
      reportError(err);
      setSessionState(
        "disconnected",
        undefined,
        "Couldn’t reach Frothy. Close other apps using the board, press EN " +
          "(or unplug and reconnect it), then try Connect again.",
      );
    }
  }

  async function disconnect(): Promise<void> {
    const closing = repl;
    unsubscribeReplEvents();
    repl = null;
    currentPort = null;
    pendingEcho = null;
    setSessionState("disconnected");
    if (closing) await closing.close().catch(() => undefined);
  }

  function currentSource(): string {
    return view.state.doc.toString();
  }

  function replaceSource(src: string): void {
    view.dispatch({
      changes: { from: 0, to: view.state.doc.length, insert: src },
    });
  }

  function updateDocumentControls(): void {
    const runnable = currentDocumentId.endsWith(".fr");
    runFormBtn.disabled = sessionState !== "idle" || !runnable;
    runFileBtn.disabled = sessionState !== "idle" || !runnable;
    if (runProjectBtn) runProjectBtn.disabled = sessionState !== "idle";
    examplesSelect.disabled = !runnable;
    openBtn.disabled = !runnable;
  }

  function openDocument(documentId: string, source: string): void {
    if (documentId === currentDocumentId) return;
    clearSaveTimer();
    documentStates.set(currentDocumentId, view.state);
    currentDocumentId = documentId;
    const state = documentStates.get(documentId) ?? createEditorState(source);
    documentStates.delete(documentId);
    view.setState(state);
    sketchFilename = basename(documentId);
    saveStatus.textContent = "saved locally";
    updateDocumentControls();
    view.focus();
  }

  function renameDocument(from: string, to: string): void {
    if (from === to) return;
    if (currentDocumentId === from) {
      currentDocumentId = to;
      sketchFilename = basename(to);
      updateDocumentControls();
      return;
    }
    const state = documentStates.get(from);
    if (!state) return;
    documentStates.delete(from);
    documentStates.set(to, state);
  }

  function forgetDocument(documentId: string): void {
    if (documentId === currentDocumentId) {
      throw new Error("cannot forget the active document");
    }
    documentStates.delete(documentId);
  }

  function resetDocument(documentId: string, source: string): void {
    clearSaveTimer();
    documentStates.clear();
    currentDocumentId = documentId;
    sketchFilename = basename(documentId);
    view.setState(createEditorState(source));
    saveStatus.textContent = "saved locally";
    updateDocumentControls();
    view.focus();
  }

  async function runForm(): Promise<void> {
    if (!repl || sessionState !== "idle" || !currentDocumentId.endsWith(".fr")) return;
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
    const runningRepl = repl;
    setSessionState("running");
    try {
      await sendForm(source);
    } finally {
      if (repl === runningRepl) setSessionState("idle");
    }
  }

  async function runFile(): Promise<void> {
    if (!repl || sessionState !== "idle" || !currentDocumentId.endsWith(".fr")) return;
    const forms = splitForms(currentSource());
    if (forms.length === 0) {
      transcript.note("(empty file)");
      return;
    }
    if (forms.some((form) => !form.complete)) {
      transcript.note("finish the incomplete form before running this file");
      return;
    }
    const runningRepl = repl;
    setSessionState("running");
    try {
      for (const form of forms) {
        const response = await sendForm(form.source);
        if (!response) return;
        if (response.kind === "error") {
          transcript.note("stopped: form errored");
          return;
        }
      }
    } finally {
      if (repl === runningRepl) setSessionState("idle");
    }
  }

  async function runProject(): Promise<void> {
    if (!repl || sessionState !== "idle" || !opts.resolveProject) return;
    const runningRepl = repl;
    setSessionState("running");
    try {
      const sources = await opts.resolveProject(currentSource());
      if (repl !== runningRepl) return;
      if (sources.length === 0) {
        transcript.note("(empty project)");
        return;
      }
      for (const source of sources) {
        const forms = splitForms(source.source);
        if (forms.some((form) => !form.complete)) {
          transcript.note(`${source.path}: finish the incomplete form before running the project`);
          return;
        }
        for (let index = 0; index < forms.length; index += 1) {
          const response = await sendForm(forms[index].source);
          if (!response) return;
          if (response.kind === "error") {
            transcript.note(`stopped: ${source.path} form ${index + 1} errored`);
            return;
          }
        }
      }
    } catch (err) {
      reportError(err);
    } finally {
      if (repl === runningRepl) setSessionState("idle");
    }
  }

  async function sendForm(source: string): Promise<Response | null> {
    if (!repl) return null;
    transcript.appendHost(source);
    pendingEcho = null;
    try {
      return await repl.sendForm(source);
    } catch (err) {
      reportError(err);
      return null;
    }
  }

  async function interrupt(): Promise<void> {
    if (sessionState !== "running") return;
    try {
      await repl?.interrupt();
    } catch (err) {
      reportError(err);
    }
  }

  function showWords(words: string[]): void {
    function render(): void {
      const query = wordsSearch.value.toLowerCase();
      const matches = words.filter((word) => word.toLowerCase().includes(query));
      wordsList.replaceChildren();
      if (matches.length === 0) {
        const empty = doc.createElement("p");
        empty.textContent = "No matching words";
        wordsList.append(empty);
        return;
      }
      for (const word of matches) {
        const button = mkBtn(doc, word, "frothy-btn");
        button.addEventListener("click", () => void inspectWord(word));
        wordsList.append(button);
      }
    }

    wordsSearch.value = "";
    render();
    wordsDialog.showModal();
    wordsSearch.focus();
    wordsSearch.oninput = render;
  }

  async function browseWords(): Promise<void> {
    if (!repl || sessionState !== "idle") return;
    const runningRepl = repl;
    setSessionState("running");
    try {
      const response = await sendForm("words");
      if (response?.kind !== "value") return;
      const lines = response.lines[0] === "words" ? response.lines.slice(1) : response.lines;
      const words = [...new Set(
        lines
          .flatMap((line) => line.split(/\s+/))
          .filter((word) => word && word !== "ok"),
      )];
      showWords(words);
    } finally {
      if (repl === runningRepl) setSessionState("idle");
    }
  }

  async function inspectWord(word: string): Promise<void> {
    wordsDialog.close();
    if (!repl || sessionState !== "idle") return;
    const runningRepl = repl;
    setSessionState("running");
    try {
      await sendForm(`see ${word}`);
    } finally {
      if (repl === runningRepl) setSessionState("idle");
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
    const source = currentSource();
    saveStatus.textContent = "saving...";

    const showResult = (saved: boolean) => {
      if (source !== currentSource() || saveTimer !== null) return;
      saveStatus.textContent = saved ? "saved locally" : "not saved—download .fr";
    };

    let result: boolean | Promise<boolean>;
    try {
      result = storage.save(source);
    } catch {
      showResult(false);
      return;
    }

    if (typeof result === "boolean") {
      showResult(result);
    } else {
      void result.then(showResult, () => showResult(false));
    }
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
      if (!opts.documentId) sketchFilename = basename(file.name);
      view.focus();
    } catch (err) {
      reportError(err);
    }
  }

  connectBtn.addEventListener("click", () => {
    if (sessionState === "idle" || sessionState === "running") {
      void disconnect();
    } else if (sessionState === "disconnected") {
      void connect();
    }
  });
  runFormBtn.addEventListener("click", () => void runForm());
  runFileBtn.addEventListener("click", () => void runFile());
  runProjectBtn?.addEventListener("click", () => void runProject());
  interruptBtn.addEventListener("click", () => void interrupt());
  browseWordsBtn.addEventListener("click", () => void browseWords());
  closeWordsBtn.addEventListener("click", () => wordsDialog.close());
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
    if (!opts.documentId) sketchFilename = `${example.name}.fr`;
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
    openDocument,
    renameDocument,
    forgetDocument,
    resetDocument,
    connect,
    disconnect,
    runForm,
    runFile,
    runProject,
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

export function shouldConfirmReplace(current: string, incoming: string): boolean {
  const trimmedCurrent = current.trim();
  return trimmedCurrent.length > 0
    && trimmedCurrent !== incoming.trim()
    && trimmedCurrent !== DEFAULT_INITIAL_SOURCE.trim();
}
