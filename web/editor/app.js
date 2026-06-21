import { mountEditor } from "./vendor/frothy-editor/0.0.0/index.js";

const mount = document.getElementById("editor");
if (!mount) {
  throw new Error("#editor element missing");
}

mountEditor({
  mount,
  storageKey: "frothy-editor:sketch",
});
