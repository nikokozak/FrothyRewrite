import {
  DEFAULT_INITIAL_SOURCE,
  mountEditor,
} from "./editor.js";
import type {
  EditorHandle,
  EditorOptions,
  ResolvedSource,
} from "./editor.js";
import { makeStorage } from "./storage.js";
import type { SketchStorage } from "./storage.js";

export const PROJECT_LIMITS = Object.freeze({
  files: 32,
  fileBytes: 64 * 1024,
  totalSourceBytes: 256 * 1024,
  instruments: 24,
  pathBytes: 160,
  metadataBytes: 160,
});

export const DEFAULT_PROJECT_MANIFEST = `name = "sketch"
board = "esp32_devkit_v1"
`;

const DATABASE_NAME = "frothy-editor";
const DATABASE_VERSION = 1;
const DRAFT_STORE = "drafts";
const LOCAL_DRAFT_ID = "local";
const LEGACY_SKETCH_KEY = "frothy-editor:sketch";
const PROJECT_JOURNAL_KEY = "frothy-editor:project-journal";
const utf8 = new TextEncoder();

export interface ControlInstrument {
  id: string;
  kind: "control";
  binding: string;
  value_type: "int";
  min: number;
  max: number;
  step: number;
}

export interface PlotInstrument {
  id: string;
  kind: "plot";
  signal: string;
  unit?: string;
  scale?: number;
}

export type Instrument = ControlInstrument | PlotInstrument;

export interface ProjectDocumentV1 {
  schema: 1;
  files: Record<string, string>;
  instruments: Instrument[];
}

export interface BrowserDraft {
  draftId: string;
  cloudProjectId: string | null;
  cloudProjectTitle: string | null;
  baseLockVersion: number | null;
  activePath: string;
  localRevision: number;
  document: ProjectDocumentV1;
  pendingCloudSave: boolean;
}

export interface ProjectEditorOptions {
  mount: HTMLElement;
  resolveProject?: (document: ProjectDocumentV1) => Promise<ResolvedSource[]>;
  onDraftSaved?: (draft: BrowserDraft) => void;
  onConnect?: EditorOptions["onConnect"];
  onError?: EditorOptions["onError"];
}

export interface ProjectEditorHandle extends EditorHandle {
  getDraft(): BrowserDraft;
  setProjectTitle(title: string): Promise<void>;
  hasSameProjectDocument(document: ProjectDocumentV1): boolean;
  openCloudProject(
    projectId: string,
    projectTitle: string,
    lockVersion: number,
    document: ProjectDocumentV1,
  ): Promise<void>;
  startNewProject(): Promise<void>;
  acknowledgeCloudSave(
    projectId: string,
    projectTitle: string,
    lockVersion: number,
    savedDocument: ProjectDocumentV1,
  ): Promise<void>;
}

export function createProjectDocument(source: string): ProjectDocumentV1 {
  return {
    schema: 1,
    files: {
      "frothy.toml": DEFAULT_PROJECT_MANIFEST,
      "main.fr": source,
    },
    instruments: [],
  };
}

export function createBrowserDraft(source: string): BrowserDraft {
  return {
    draftId: LOCAL_DRAFT_ID,
    cloudProjectId: null,
    cloudProjectTitle: null,
    baseLockVersion: null,
    activePath: "main.fr",
    localRevision: 0,
    document: createProjectDocument(source),
    pendingCloudSave: false,
  };
}

export function validateProjectDocument(value: unknown): string[] {
  if (!isRecord(value)) return ["project must be an object"];

  const errors: string[] = [];
  rejectExtraKeys(value, ["schema", "files", "instruments"], "project", errors);
  if (value.schema !== 1) errors.push("project.schema must be 1");

  if (!isRecord(value.files)) {
    errors.push("project.files must be an object");
  } else {
    // Valid paths are ASCII; keep this ordering independent of browser locale.
    const files = Object.entries(value.files)
      .sort(([left], [right]) => left < right ? -1 : left > right ? 1 : 0);
    if (files.length > PROJECT_LIMITS.files) {
      errors.push(`project.files exceeds ${PROJECT_LIMITS.files} files`);
    }
    if (!Object.hasOwn(value.files, "frothy.toml")) errors.push("project.files needs frothy.toml");
    if (!Object.hasOwn(value.files, "main.fr")) errors.push("project.files needs main.fr");

    let totalBytes = 0;
    for (const [path, source] of files) {
      const pathError = validateProjectPath(path);
      if (pathError) errors.push(pathError);
      if (path !== "frothy.toml" && !path.endsWith(".fr")) {
        errors.push(`${path} is not frothy.toml or a .fr file`);
      }
      if (typeof source !== "string") {
        errors.push(`${path} source must be text`);
        continue;
      }
      const sourceBytes = byteLength(source);
      totalBytes += sourceBytes;
      if (sourceBytes > PROJECT_LIMITS.fileBytes) {
        errors.push(`${path} exceeds ${PROJECT_LIMITS.fileBytes} bytes`);
      }
    }
    if (totalBytes > PROJECT_LIMITS.totalSourceBytes) {
      errors.push(`project source exceeds ${PROJECT_LIMITS.totalSourceBytes} bytes`);
    }
  }

  if (!Array.isArray(value.instruments)) {
    errors.push("project.instruments must be an array");
  } else {
    if (value.instruments.length > PROJECT_LIMITS.instruments) {
      errors.push(`project.instruments exceeds ${PROJECT_LIMITS.instruments} entries`);
    }
    const ids = new Set<string>();
    value.instruments.forEach((instrument, index) => {
      validateInstrument(instrument, index, ids, errors);
    });
  }

  return errors;
}

export function validateBrowserDraft(value: unknown): string[] {
  if (!isRecord(value)) return ["draft must be an object"];

  const errors: string[] = [];
  rejectExtraKeys(
    value,
    [
      "draftId",
      "cloudProjectId",
      "cloudProjectTitle",
      "baseLockVersion",
      "activePath",
      "localRevision",
      "document",
      "pendingCloudSave",
    ],
    "draft",
    errors,
  );
  if (!boundedText(value.draftId)) errors.push("draft.draftId must be bounded text");
  if (value.cloudProjectId !== null && !boundedText(value.cloudProjectId)) {
    errors.push("draft.cloudProjectId must be null or bounded text");
  }
  if (value.cloudProjectTitle !== null && !boundedText(value.cloudProjectTitle)) {
    errors.push("draft.cloudProjectTitle must be null or bounded text");
  }
  if (
    value.baseLockVersion !== null
    && (!Number.isSafeInteger(value.baseLockVersion) || (value.baseLockVersion as number) < 0)
  ) {
    errors.push("draft.baseLockVersion must be null or a non-negative integer");
  }
  if (typeof value.activePath !== "string") {
    errors.push("draft.activePath must be a project path");
  } else {
    const pathError = validateProjectPath(value.activePath);
    if (pathError) errors.push(`draft.activePath: ${pathError}`);
    if (
      isRecord(value.document)
      && isRecord(value.document.files)
      && !Object.hasOwn(value.document.files, value.activePath)
    ) {
      errors.push("draft.activePath must name a project file");
    }
  }
  if (!Number.isSafeInteger(value.localRevision) || (value.localRevision as number) < 0) {
    errors.push("draft.localRevision must be a non-negative integer");
  }
  if (typeof value.pendingCloudSave !== "boolean") {
    errors.push("draft.pendingCloudSave must be boolean");
  }
  for (const error of validateProjectDocument(value.document)) {
    errors.push(`draft.document: ${error}`);
  }
  return errors;
}

export async function mountProjectEditor(opts: ProjectEditorOptions): Promise<ProjectEditorHandle> {
  const database = await openDraftDatabase();
  const legacyStorage = makeStorage(LEGACY_SKETCH_KEY);
  let workspaceRoot: HTMLElement | null = null;

  try {
    let draft = await loadOrCreateDraft(database, legacyStorage);
    const doc = opts.mount.ownerDocument;
    workspaceRoot = doc.createElement("div");
    workspaceRoot.className = "frothy-project-workspace";

    const filePanel = doc.createElement("aside");
    filePanel.className = "frothy-file-panel";
    filePanel.setAttribute("aria-label", "Project files");
    const fileHeader = doc.createElement("div");
    fileHeader.className = "frothy-file-header";
    const fileTitle = doc.createElement("h2");
    fileTitle.textContent = "Files";
    const newFileBtn = doc.createElement("button");
    newFileBtn.type = "button";
    newFileBtn.className = "frothy-file-add";
    newFileBtn.textContent = "+";
    newFileBtn.title = "New file";
    newFileBtn.setAttribute("aria-label", "New file");
    fileHeader.append(fileTitle, newFileBtn);
    const fileList = doc.createElement("div");
    fileList.className = "frothy-file-list";
    const fileStatus = doc.createElement("p");
    fileStatus.className = "frothy-file-status";
    fileStatus.setAttribute("role", "status");
    fileStatus.setAttribute("aria-live", "polite");
    filePanel.append(fileHeader, fileList, fileStatus);

    const editorMount = doc.createElement("div");
    editorMount.className = "frothy-project-editor";
    workspaceRoot.append(filePanel, editorMount);
    opts.mount.append(workspaceRoot);

    async function persistDraft(next: BrowserDraft): Promise<boolean> {
      if (validateBrowserDraft(next).length > 0) return false;

      const previous = draft;
      const journalSaved = writeProjectJournal(next);
      draft = next;
      void legacyStorage.save(next.document.files["main.fr"] ?? "");
      try {
        await putDraft(database, next);
      } catch {
        if (!journalSaved) {
          if (draft === next) draft = previous;
          return false;
        }
      }
      opts.onDraftSaved?.(copyDraft(next));
      return true;
    }

    const storage: SketchStorage = {
      load: () => draft.document.files[draft.activePath] ?? null,
      save: (source) => persistDraft(withFileSource(draft, draft.activePath, source)),
      download: (source, filename) => legacyStorage.download(source, filename),
    };

    const editorOptions: EditorOptions = {
      mount: editorMount,
      storage,
      documentId: draft.activePath,
    };
    if (opts.onConnect) editorOptions.onConnect = opts.onConnect;
    if (opts.onError) editorOptions.onError = opts.onError;
    if (opts.resolveProject) {
      editorOptions.resolveProject = (source) => opts.resolveProject!(
        copyProjectDocument(withFileSource(draft, draft.activePath, source).document),
      );
    }
    const editor = mountEditor(editorOptions);

    function showFileError(error: unknown): void {
      fileStatus.textContent = error instanceof Error ? error.message : String(error);
      fileStatus.dataset.kind = "error";
    }

    function clearFileStatus(): void {
      fileStatus.textContent = "";
      delete fileStatus.dataset.kind;
    }

    function renderFiles(): void {
      fileList.replaceChildren();
      for (const path of projectFilePaths(draft.document)) {
        const row = doc.createElement("div");
        row.className = "frothy-file-row";
        if (path === draft.activePath) row.classList.add("is-active");

        const openBtn = doc.createElement("button");
        openBtn.type = "button";
        openBtn.className = "frothy-file-name";
        openBtn.textContent = path;
        openBtn.title = path;
        if (path === draft.activePath) openBtn.setAttribute("aria-current", "page");
        openBtn.addEventListener("click", () => void selectFile(path));
        row.append(openBtn);

        if (!isRequiredProjectFile(path)) {
          const actions = doc.createElement("span");
          actions.className = "frothy-file-actions";
          const renameBtn = doc.createElement("button");
          renameBtn.type = "button";
          renameBtn.textContent = "✎";
          renameBtn.title = `Rename ${path}`;
          renameBtn.setAttribute("aria-label", `Rename ${path}`);
          renameBtn.addEventListener("click", () => void renameFile(path));
          const deleteBtn = doc.createElement("button");
          deleteBtn.type = "button";
          deleteBtn.textContent = "×";
          deleteBtn.title = `Delete ${path}`;
          deleteBtn.setAttribute("aria-label", `Delete ${path}`);
          deleteBtn.addEventListener("click", () => void deleteFile(path));
          actions.append(renameBtn, deleteBtn);
          row.append(actions);
        }
        fileList.append(row);
      }
    }

    async function selectFile(path: string): Promise<void> {
      if (path === draft.activePath) return;
      clearFileStatus();
      editor.save();
      const next = selectProjectFile(draft, path);
      if (!await persistDraft(next)) {
        showFileError("Could not save the browser project.");
        return;
      }
      editor.openDocument(path, next.document.files[path] ?? "");
      renderFiles();
    }

    async function createFile(): Promise<void> {
      const path = doc.defaultView?.prompt("New Frothy file path", "src/new.fr");
      if (path === null || path === undefined) return;
      clearFileStatus();
      editor.save();
      try {
        const next = addProjectFile(draft, path);
        if (!await persistDraft(next)) {
          showFileError("Could not save the new file.");
          return;
        }
        editor.openDocument(path, "");
        renderFiles();
      } catch (error) {
        showFileError(error);
      }
    }

    async function renameFile(path: string): Promise<void> {
      const nextPath = doc.defaultView?.prompt(`Rename ${path}`, path);
      if (nextPath === null || nextPath === undefined) return;
      clearFileStatus();
      editor.save();
      try {
        const next = renameProjectFile(draft, path, nextPath);
        if (!await persistDraft(next)) {
          showFileError("Could not save the renamed file.");
          return;
        }
        editor.renameDocument(path, nextPath);
        renderFiles();
      } catch (error) {
        showFileError(error);
      }
    }

    async function deleteFile(path: string): Promise<void> {
      if (!(doc.defaultView?.confirm(`Delete "${path}"?`) ?? false)) return;
      clearFileStatus();
      editor.save();
      try {
        const wasActive = path === draft.activePath;
        const next = deleteProjectFile(draft, path);
        if (!await persistDraft(next)) {
          showFileError("Could not delete the file.");
          return;
        }
        if (wasActive) {
          editor.openDocument(next.activePath, next.document.files[next.activePath] ?? "");
        }
        editor.forgetDocument(path);
        renderFiles();
      } catch (error) {
        showFileError(error);
      }
    }

    async function installDraft(next: BrowserDraft, replaceWorkspace = false): Promise<void> {
      const previousPath = draft.activePath;
      if (!await persistDraft(next)) throw new Error("could not save the browser project");
      const source = next.document.files[next.activePath] ?? "";
      if (replaceWorkspace) {
        editor.resetDocument(next.activePath, source);
      } else if (next.activePath !== previousPath) {
        editor.openDocument(next.activePath, source);
      }
      renderFiles();
    }

    newFileBtn.addEventListener("click", () => void createFile());
    renderFiles();

    return {
      ...editor,
      getDraft() {
        return copyDraft(withFileSource(draft, draft.activePath, editor.getSource()));
      },
      async setProjectTitle(title) {
        const current = withFileSource(draft, draft.activePath, editor.getSource());
        if (!await persistDraft(setBrowserDraftTitle(current, title))) {
          throw new Error("could not save the project title");
        }
      },
      hasSameProjectDocument(document) {
        const current = withFileSource(draft, draft.activePath, editor.getSource());
        return sameProjectDocument(current.document, document);
      },
      async openCloudProject(projectId, projectTitle, lockVersion, document) {
        const current = withFileSource(draft, draft.activePath, editor.getSource());
        await installDraft(
          openCloudProjectDraft(current, projectId, projectTitle, lockVersion, document),
          true,
        );
      },
      async startNewProject() {
        const current = withFileSource(draft, draft.activePath, editor.getSource());
        await installDraft(startNewProjectDraft(current));
      },
      async acknowledgeCloudSave(projectId, projectTitle, lockVersion, savedDocument) {
        const current = withFileSource(draft, draft.activePath, editor.getSource());
        const next = acknowledgeBrowserDraft(
          current,
          projectId,
          projectTitle,
          lockVersion,
          savedDocument,
        );
        if (!await persistDraft(next)) throw new Error("could not save cloud acknowledgement");
      },
      destroy() {
        editor.destroy();
        database.close();
        workspaceRoot?.remove();
      },
    };
  } catch (error) {
    workspaceRoot?.remove();
    database.close();
    throw error;
  }
}

function withFileSource(draft: BrowserDraft, path: string, source: string): BrowserDraft {
  if (source === draft.document.files[path]) return draft;
  return withProjectDocument(draft, {
    ...draft.document,
    files: { ...draft.document.files, [path]: source },
  });
}

function withProjectDocument(
  draft: BrowserDraft,
  document: ProjectDocumentV1,
  activePath = draft.activePath,
): BrowserDraft {
  return parseBrowserDraft({
    ...draft,
    activePath,
    localRevision: draft.localRevision + 1,
    document,
    pendingCloudSave: true,
  });
}

export function selectProjectFile(draft: BrowserDraft, path: string): BrowserDraft {
  assertProjectFile(draft.document, path);
  if (path === draft.activePath) return draft;
  return parseBrowserDraft({
    ...draft,
    activePath: path,
    localRevision: draft.localRevision + 1,
  });
}

export function addProjectFile(draft: BrowserDraft, path: string): BrowserDraft {
  assertNewFrothyPath(path);
  if (Object.hasOwn(draft.document.files, path)) throw new Error(`${path} already exists`);
  return withProjectDocument(
    draft,
    {
      ...draft.document,
      files: { ...draft.document.files, [path]: "" },
    },
    path,
  );
}

export function renameProjectFile(
  draft: BrowserDraft,
  from: string,
  to: string,
): BrowserDraft {
  assertProjectFile(draft.document, from);
  if (from === to) return draft;
  if (isRequiredProjectFile(from)) throw new Error(`${from} cannot be renamed`);
  assertNewFrothyPath(to);
  if (Object.hasOwn(draft.document.files, to)) throw new Error(`${to} already exists`);

  const files = { ...draft.document.files, [to]: draft.document.files[from] ?? "" };
  delete files[from];
  return withProjectDocument(
    draft,
    { ...draft.document, files },
    draft.activePath === from ? to : draft.activePath,
  );
}

export function deleteProjectFile(draft: BrowserDraft, path: string): BrowserDraft {
  assertProjectFile(draft.document, path);
  if (isRequiredProjectFile(path)) throw new Error(`${path} cannot be deleted`);

  const files = { ...draft.document.files };
  delete files[path];
  return withProjectDocument(
    draft,
    { ...draft.document, files },
    draft.activePath === path ? "main.fr" : draft.activePath,
  );
}

function assertProjectFile(document: ProjectDocumentV1, path: string): void {
  if (!Object.hasOwn(document.files, path)) throw new Error(`${path} does not exist`);
}

function assertNewFrothyPath(path: string): void {
  const pathError = validateProjectPath(path);
  if (pathError) throw new Error(pathError);
  if (!path.endsWith(".fr")) throw new Error(`${path} must end in .fr`);
}

function isRequiredProjectFile(path: string): boolean {
  return path === "frothy.toml" || path === "main.fr";
}

function projectFilePaths(document: ProjectDocumentV1): string[] {
  return Object.keys(document.files).sort((left, right) => {
    const rank = (path: string) => path === "frothy.toml" ? 0 : path === "main.fr" ? 1 : 2;
    return rank(left) - rank(right) || (left < right ? -1 : left > right ? 1 : 0);
  });
}

export function openCloudProjectDraft(
  draft: BrowserDraft,
  projectId: string,
  projectTitle: string,
  lockVersion: number,
  document: ProjectDocumentV1,
): BrowserDraft {
  assertCloudProject(projectId, projectTitle, lockVersion, document);

  return parseBrowserDraft({
    ...draft,
    cloudProjectId: projectId,
    cloudProjectTitle: projectTitle,
    baseLockVersion: lockVersion,
    activePath: Object.hasOwn(document.files, draft.activePath) ? draft.activePath : "main.fr",
    localRevision: draft.localRevision + 1,
    document: copyProjectDocument(document),
    pendingCloudSave: false,
  });
}

export function startNewProjectDraft(draft: BrowserDraft): BrowserDraft {
  return parseBrowserDraft({
    ...draft,
    cloudProjectId: null,
    cloudProjectTitle: null,
    baseLockVersion: null,
    activePath: "main.fr",
    localRevision: draft.localRevision + 1,
    document: createProjectDocument(DEFAULT_INITIAL_SOURCE),
    pendingCloudSave: false,
  });
}

export function setBrowserDraftTitle(draft: BrowserDraft, title: string): BrowserDraft {
  if (!boundedText(title)) throw new Error("project title must be bounded text");
  if (draft.cloudProjectTitle === title) return draft;

  return parseBrowserDraft({
    ...draft,
    cloudProjectTitle: title,
    localRevision: draft.localRevision + 1,
    pendingCloudSave: true,
  });
}

export function acknowledgeBrowserDraft(
  draft: BrowserDraft,
  projectId: string,
  projectTitle: string,
  lockVersion: number,
  savedDocument: ProjectDocumentV1,
): BrowserDraft {
  assertCloudProject(projectId, projectTitle, lockVersion, savedDocument);

  const next = {
    ...draft,
    cloudProjectId: projectId,
    cloudProjectTitle: projectTitle,
    baseLockVersion: lockVersion,
    localRevision: draft.localRevision + 1,
    pendingCloudSave: !sameProjectDocument(draft.document, savedDocument),
  };
  const draftErrors = validateBrowserDraft(next);
  if (draftErrors.length > 0) throw new Error(draftErrors[0]);
  return next;
}

export function sameProjectDocument(
  left: ProjectDocumentV1,
  right: ProjectDocumentV1,
): boolean {
  return JSON.stringify(canonicalJson(left)) === JSON.stringify(canonicalJson(right));
}

function assertCloudProject(
  projectId: string,
  projectTitle: string,
  lockVersion: number,
  document: ProjectDocumentV1,
): void {
  if (!boundedText(projectId)) throw new Error("project id must be bounded text");
  if (!boundedText(projectTitle)) throw new Error("project title must be bounded text");
  if (!Number.isSafeInteger(lockVersion) || lockVersion < 1) {
    throw new Error("lock version must be a positive integer");
  }
  const documentErrors = validateProjectDocument(document);
  if (documentErrors.length > 0) throw new Error(documentErrors[0]);
}

function canonicalJson(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonicalJson);
  if (!isRecord(value)) return value;

  return Object.fromEntries(
    Object.keys(value)
      .sort()
      .map((key) => [key, canonicalJson(value[key])]),
  );
}

function copyDraft(draft: BrowserDraft): BrowserDraft {
  return {
    ...draft,
    document: copyProjectDocument(draft.document),
  };
}

function copyProjectDocument(document: ProjectDocumentV1): ProjectDocumentV1 {
  return {
    ...document,
    files: { ...document.files },
    instruments: document.instruments.map((instrument) => ({ ...instrument })),
  };
}

async function loadOrCreateDraft(
  database: IDBDatabase,
  legacyStorage: SketchStorage,
): Promise<BrowserDraft> {
  const stored = await getDraft(database, LOCAL_DRAFT_ID);
  const legacySource = legacyStorage.load();
  const storedDraft = stored === undefined
    ? null
    : tryParseBrowserDraft(migrateBrowserDraft(stored));
  const journalDraft = loadProjectJournal();

  if (stored !== undefined && storedDraft === null && journalDraft === null) {
    return parseBrowserDraft(migrateBrowserDraft(stored));
  }

  let draft = newestBrowserDraft(storedDraft, journalDraft);

  if (draft !== null) {
    if (
      journalDraft === null
      && legacySource !== null
      && legacySource !== draft.document.files["main.fr"]
    ) {
      draft = withFileSource(draft, "main.fr", legacySource);
    }
    writeProjectJournal(draft);
    if (storedDraft !== draft) await putDraft(database, draft);
    return draft;
  }

  draft = createBrowserDraft(legacySource ?? DEFAULT_INITIAL_SOURCE);
  writeProjectJournal(draft);
  await putDraft(database, draft);
  return draft;
}

export function migrateBrowserDraft(value: unknown): unknown {
  if (!isRecord(value)) return value;
  const next = { ...value };
  if (!Object.hasOwn(next, "cloudProjectTitle")) next.cloudProjectTitle = null;
  if (!Object.hasOwn(next, "activePath")) next.activePath = "main.fr";
  if (!Object.hasOwn(next, "localRevision")) next.localRevision = 0;
  return next;
}

export function newestBrowserDraft(
  storedDraft: BrowserDraft | null,
  journalDraft: BrowserDraft | null,
): BrowserDraft | null {
  if (journalDraft === null) return storedDraft;
  if (storedDraft === null) return journalDraft;
  return journalDraft.localRevision >= storedDraft.localRevision
    ? journalDraft
    : storedDraft;
}

function parseBrowserDraft(value: unknown): BrowserDraft {
  const errors = validateBrowserDraft(value);
  if (errors.length > 0) throw new Error(errors[0]);
  return value as BrowserDraft;
}

function tryParseBrowserDraft(value: unknown): BrowserDraft | null {
  try {
    return parseBrowserDraft(value);
  } catch {
    return null;
  }
}

function loadProjectJournal(): BrowserDraft | null {
  try {
    const value = globalThis.localStorage?.getItem(PROJECT_JOURNAL_KEY);
    return value === null || value === undefined
      ? null
      : tryParseBrowserDraft(migrateBrowserDraft(JSON.parse(value)));
  } catch {
    return null;
  }
}

function writeProjectJournal(draft: BrowserDraft): boolean {
  try {
    const storage = globalThis.localStorage;
    if (!storage) return false;
    storage.setItem(PROJECT_JOURNAL_KEY, JSON.stringify(draft));
    return true;
  } catch {
    return false;
  }
}

function openDraftDatabase(): Promise<IDBDatabase> {
  if (!globalThis.indexedDB) return Promise.reject(new Error("IndexedDB is unavailable"));

  return new Promise((resolve, reject) => {
    const request = globalThis.indexedDB.open(DATABASE_NAME, DATABASE_VERSION);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(DRAFT_STORE)) {
        request.result.createObjectStore(DRAFT_STORE, { keyPath: "draftId" });
      }
    };
    request.onsuccess = () => {
      request.result.onversionchange = () => request.result.close();
      resolve(request.result);
    };
    request.onerror = () => reject(request.error ?? new Error("IndexedDB open failed"));
    request.onblocked = () => reject(new Error("IndexedDB upgrade is blocked"));
  });
}

function getDraft(database: IDBDatabase, draftId: string): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const request = database
      .transaction(DRAFT_STORE, "readonly")
      .objectStore(DRAFT_STORE)
      .get(draftId);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("IndexedDB read failed"));
  });
}

function putDraft(database: IDBDatabase, draft: BrowserDraft): Promise<void> {
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(DRAFT_STORE, "readwrite");
    transaction.oncomplete = () => resolve();
    transaction.onabort = () => reject(transaction.error ?? new Error("IndexedDB write aborted"));
    transaction.onerror = () => reject(transaction.error ?? new Error("IndexedDB write failed"));
    transaction.objectStore(DRAFT_STORE).put(draft);
  });
}

function validateProjectPath(path: string): string | null {
  if (byteLength(path) > PROJECT_LIMITS.pathBytes) {
    return `${path} exceeds ${PROJECT_LIMITS.pathBytes} path bytes`;
  }
  if (path.length === 0 || /[^A-Za-z0-9._/-]/.test(path)) {
    return `${path} has unsupported path characters`;
  }
  const segments = path.split("/");
  if (segments.some((segment) => segment === "" || segment === "." || segment === "..")) {
    return `${path} is not a normalized relative path`;
  }
  if (path.startsWith(".frothy/")) return `${path} uses the reserved .frothy directory`;
  return null;
}

function validateInstrument(
  value: unknown,
  index: number,
  ids: Set<string>,
  errors: string[],
): void {
  const at = `project.instruments[${index}]`;
  if (!isRecord(value)) {
    errors.push(`${at} must be an object`);
    return;
  }
  if (!boundedText(value.id)) {
    errors.push(`${at}.id must be bounded text`);
  } else if (ids.has(value.id)) {
    errors.push(`${at}.id repeats ${value.id}`);
  } else {
    ids.add(value.id);
  }

  if (value.kind === "control") {
    rejectExtraKeys(
      value,
      ["id", "kind", "binding", "value_type", "min", "max", "step"],
      at,
      errors,
    );
    if (!boundedText(value.binding)) errors.push(`${at}.binding must be bounded text`);
    if (value.value_type !== "int") errors.push(`${at}.value_type must be int`);
    if (![value.min, value.max, value.step].every(Number.isSafeInteger)) {
      errors.push(`${at} bounds must be integers`);
    } else if ((value.min as number) > (value.max as number) || (value.step as number) <= 0) {
      errors.push(`${at} needs min <= max and step > 0`);
    }
    return;
  }

  if (value.kind === "plot") {
    rejectExtraKeys(value, ["id", "kind", "signal", "unit", "scale"], at, errors);
    if (!boundedText(value.signal)) errors.push(`${at}.signal must be bounded text`);
    if (value.unit !== undefined && !boundedText(value.unit, true)) {
      errors.push(`${at}.unit must be bounded text`);
    }
    if (value.scale !== undefined && !Number.isFinite(value.scale)) {
      errors.push(`${at}.scale must be finite`);
    }
    return;
  }

  errors.push(`${at}.kind must be control or plot`);
}

function rejectExtraKeys(
  value: Record<string, unknown>,
  allowed: string[],
  at: string,
  errors: string[],
): void {
  const keys = new Set(allowed);
  for (const key of Object.keys(value).sort()) {
    if (!keys.has(key)) errors.push(`${at}.${key} is not supported`);
  }
}

function boundedText(value: unknown, allowEmpty = false): value is string {
  return typeof value === "string"
    && (allowEmpty || value.length > 0)
    && byteLength(value) <= PROJECT_LIMITS.metadataBytes;
}

function byteLength(value: string): number {
  return utf8.encode(value).byteLength;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
