import { defineConfig } from "tsup";

// Two bundles:
// - "esm": the library shape (externalizes peer deps; consumers bring CodeMirror).
// - "browser": a single self-contained file for static demo pages
//   (web/editor/) — inlines everything.

export default defineConfig([
  {
    entry: ["src/index.ts"],
    format: ["esm"],
    dts: true,
    clean: true,
    outDir: "dist",
  },
  {
    entry: { editor: "src/index.ts" },
    format: ["esm"],
    noExternal: [/.*/],
    clean: false,
    outDir: "dist/browser",
    minify: true,
  },
]);
