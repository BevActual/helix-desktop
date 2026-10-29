import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// One self-contained script the page loads; it adds `window.HelixModelsReact`.
export default defineConfig({
  plugins: [react()],
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  build: {
    outDir: "../dist/react",
    emptyOutDir: true,
    target: "es2022",
    minify: true,
    sourcemap: false,
    lib: { entry: "src/main.tsx", formats: ["iife"], name: "HelixModelsReact", fileName: () => "helix-models.js" },
  },
});
