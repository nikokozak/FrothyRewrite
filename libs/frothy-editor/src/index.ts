// Public exports.

export { mountEditor, DEFAULT_INITIAL_SOURCE } from "./editor.js";
export type { EditorHandle, EditorOptions } from "./editor.js";
export { frothyLanguage, frothyHighlight } from "./highlight.js";
export { FrothyEditorElement, register } from "./web-component.js";
export { makeStorage } from "./storage.js";
export type { SketchStorage } from "./storage.js";
export {
  acknowledgeBrowserDraft,
  createBrowserDraft,
  createProjectDocument,
  DEFAULT_PROJECT_MANIFEST,
  mountProjectEditor,
  PROJECT_LIMITS,
  validateBrowserDraft,
  validateProjectDocument,
} from "./project.js";
export type {
  BrowserDraft,
  ControlInstrument,
  Instrument,
  PlotInstrument,
  ProjectDocumentV1,
  ProjectEditorHandle,
  ProjectEditorOptions,
} from "./project.js";
