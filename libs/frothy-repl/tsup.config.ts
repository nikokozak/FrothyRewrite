import { defineConfig } from "tsup";

export default defineConfig([
  {
    entry: ["src/index.ts"],
    format: ["esm"],
    dts: true,
    clean: true,
    outDir: "dist",
  },
  {
    entry: { index: "src/browser.ts" },
    format: ["esm"],
    clean: false,
    outDir: "dist/browser",
    minify: true,
  },
]);
