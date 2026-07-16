import { test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";

import { mountEditor } from "../src/editor.js";

test("editor keeps one CodeMirror state per project file", () => {
  const dom = new JSDOM("<!doctype html><div id=\"editor\"></div>", {
    pretendToBeVisual: true,
    url: "http://localhost/",
  });
  const window = dom.window;
  Object.assign(globalThis, {
    window,
    document: window.document,
    MutationObserver: window.MutationObserver,
    HTMLElement: window.HTMLElement,
    Node: window.Node,
    getComputedStyle: window.getComputedStyle,
    requestAnimationFrame: window.requestAnimationFrame.bind(window),
    cancelAnimationFrame: window.cancelAnimationFrame.bind(window),
  });

  const handle = mountEditor({
    mount: window.document.querySelector("#editor") as HTMLElement,
    documentId: "main.fr",
    storage: {
      load: () => "main\n",
      save: () => true,
      download: () => undefined,
    },
  });

  handle.setSource("main changed\n");
  handle.openDocument("src/blink.fr", "blink\n");
  handle.setSource("blink changed\n");
  handle.openDocument("main.fr", "ignored\n");
  assert.equal(handle.getSource(), "main changed\n");

  handle.renameDocument("src/blink.fr", "src/led.fr");
  handle.openDocument("src/led.fr", "ignored\n");
  assert.equal(handle.getSource(), "blink changed\n");

  handle.openDocument("main.fr", "ignored\n");
  handle.forgetDocument("src/led.fr");
  handle.openDocument("src/led.fr", "fresh\n");
  assert.equal(handle.getSource(), "fresh\n");
  handle.destroy();
});
