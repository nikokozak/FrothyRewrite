// Sketch persistence: localStorage round-trip + download as .fr.

const DEFAULT_KEY = "frothy-editor:sketch";

export interface SketchStorage {
  load(): string | null;
  save(source: string): boolean;
  download(source: string, filename?: string): void;
}

export function makeStorage(key: string = DEFAULT_KEY): SketchStorage {
  return {
    load() {
      try {
        return globalThis.localStorage?.getItem(key) ?? null;
      } catch {
        return null;
      }
    },
    save(source: string) {
      try {
        const storage = globalThis.localStorage;
        if (!storage) return false;
        storage.setItem(key, source);
        return true;
      } catch {
        return false;
      }
    },
    download(source: string, filename = "sketch.fr") {
      const doc = globalThis.document;
      if (!doc) return;
      const blob = new Blob([source], { type: "text/plain;charset=utf-8" });
      const url = URL.createObjectURL(blob);
      const a = doc.createElement("a");
      a.href = url;
      a.download = filename;
      doc.body.appendChild(a);
      a.click();
      doc.body.removeChild(a);
      URL.revokeObjectURL(url);
    },
  };
}
