import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// One self-contained script the shell page loads after shell.js; it adds `window.HelixShellReact`. The page keeps its hand-written
// drawing as the fallback for a build without node.
export default defineConfig({
  plugins: [react()],
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  build: {
    outDir: "../dist/react",
    emptyOutDir: true,
    target: "es2022",
    minify: true,
    sourcemap: false,
    lib: { entry: "src/main.tsx", formats: ["iife"], name: "HelixShellReact", fileName: () => "helix-shell-react.js" },
  },
});
