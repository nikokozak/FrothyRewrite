// <frothy-editor source="..." storage-key="..."></frothy-editor>
//
// A thin web-component wrapper around mountEditor for sites that want
// to drop the editor into a page without wiring DOM themselves.

import { mountEditor, DEFAULT_INITIAL_SOURCE } from "./editor.js";
import type { EditorHandle } from "./editor.js";

export class FrothyEditorElement extends HTMLElement {
  private handle: EditorHandle | null = null;

  connectedCallback(): void {
    if (this.handle) return;
    const source = this.getAttribute("source") ?? DEFAULT_INITIAL_SOURCE;
    const storageKey = this.getAttribute("storage-key") ?? undefined;
    const host = this.ownerDocument.createElement("div");
    this.appendChild(host);
    this.handle = mountEditor({
      mount: host,
      initialSource: source,
      ...(storageKey ? { storageKey } : {}),
    });
  }

  disconnectedCallback(): void {
    if (this.handle) {
      this.handle.destroy();
      this.handle = null;
    }
  }
}

export function register(name: string = "frothy-editor"): void {
  if (!globalThis.customElements) return;
  if (!globalThis.customElements.get(name)) {
    globalThis.customElements.define(name, FrothyEditorElement);
  }
}
