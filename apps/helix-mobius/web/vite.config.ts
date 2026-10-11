import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// One self-contained script the page loads before software.js; it adds `window.HelixMobiusReact`. The hand-written page stays the
// fallback for a build without node.
export default defineConfig({
  plugins: [react()],
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  build: {
    outDir: "../dist/react",
    emptyOutDir: true,
    target: "es2022",
    minify: true,
    sourcemap: false,
    lib: { entry: "src/main.tsx", formats: ["iife"], name: "HelixMobiusReact", fileName: () => "helix-mobius.js" },
  },
});
