import {
  DEFAULT_INITIAL_SOURCE,
  mountEditor,
} from "./editor.js";
import type {
  EditorHandle,
  EditorOptions,
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
  document: ProjectDocumentV1;
  pendingCloudSave: boolean;
}

export interface ProjectEditorOptions {
  mount: HTMLElement;
  onConnect?: EditorOptions["onConnect"];
  onError?: EditorOptions["onError"];
}

export interface ProjectEditorHandle extends EditorHandle {
  getDraft(): BrowserDraft;
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

  try {
    let draft = await loadOrCreateDraft(database, legacyStorage);
    const storage: SketchStorage = {
      load: () => draft.document.files["main.fr"] ?? null,
      save: async (source) => {
        // ponytail: main.fr mirrors synchronously while W2 is single-file;
        // replace with a whole-document journal when W3 adds file-tree edits.
        void legacyStorage.save(source);
        const next = withMainSource(draft, source);
        if (validateBrowserDraft(next).length > 0) return false;
        try {
          await putDraft(database, next);
          draft = next;
          return true;
        } catch {
          return false;
        }
      },
      download: (source, filename) => legacyStorage.download(source, filename),
    };

    const editorOptions: EditorOptions = { mount: opts.mount, storage };
    if (opts.onConnect) editorOptions.onConnect = opts.onConnect;
    if (opts.onError) editorOptions.onError = opts.onError;
    const editor = mountEditor(editorOptions);

    async function installDraft(next: BrowserDraft): Promise<void> {
      await putDraft(database, next);
      const source = next.document.files["main.fr"] ?? "";
      void legacyStorage.save(source);
      draft = next;
      if (editor.getSource() !== source) editor.setSource(source);
    }

    return {
      ...editor,
      getDraft() {
        return copyDraft(withMainSource(draft, editor.getSource()));
      },
      hasSameProjectDocument(document) {
        const current = withMainSource(draft, editor.getSource());
        return sameProjectDocument(current.document, document);
      },
      async openCloudProject(projectId, projectTitle, lockVersion, document) {
        const current = withMainSource(draft, editor.getSource());
        await installDraft(
          openCloudProjectDraft(current, projectId, projectTitle, lockVersion, document),
        );
      },
      async startNewProject() {
        const current = withMainSource(draft, editor.getSource());
        await installDraft(startNewProjectDraft(current));
      },
      async acknowledgeCloudSave(projectId, projectTitle, lockVersion, savedDocument) {
        const current = withMainSource(draft, editor.getSource());
        const next = acknowledgeBrowserDraft(
          current,
          projectId,
          projectTitle,
          lockVersion,
          savedDocument,
        );
        await putDraft(database, next);
        draft = next;
      },
      destroy() {
        editor.destroy();
        database.close();
      },
    };
  } catch (error) {
    database.close();
    throw error;
  }
}

function withMainSource(draft: BrowserDraft, source: string): BrowserDraft {
  const changed = source !== draft.document.files["main.fr"];

  return {
    ...draft,
    pendingCloudSave:
      draft.pendingCloudSave || (changed && draft.cloudProjectId !== null),
    document: {
      ...draft.document,
      files: { ...draft.document.files, "main.fr": source },
    },
  };
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
    document: copyProjectDocument(draft.document),
    pendingCloudSave: false,
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

  if (stored !== undefined) {
    let draft = parseBrowserDraft(migrateBrowserDraft(stored));
    if (legacySource !== null && legacySource !== draft.document.files["main.fr"]) {
      draft = withMainSource(draft, legacySource);
      const errors = validateBrowserDraft(draft);
      if (errors.length > 0) throw new Error(errors[0]);
      await putDraft(database, draft);
    }
    return draft;
  }

  const draft = createBrowserDraft(legacySource ?? DEFAULT_INITIAL_SOURCE);
  await putDraft(database, draft);
  return draft;
}

export function migrateBrowserDraft(value: unknown): unknown {
  if (isRecord(value) && !Object.hasOwn(value, "cloudProjectTitle")) {
    return { ...value, cloudProjectTitle: null };
  }
  return value;
}

function parseBrowserDraft(value: unknown): BrowserDraft {
  const errors = validateBrowserDraft(value);
  if (errors.length > 0) throw new Error(errors[0]);
  return value as BrowserDraft;
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
