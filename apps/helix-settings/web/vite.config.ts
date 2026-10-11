import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// One self-contained script the Tauri page loads next to the hand-written ones; it adds `window.HelixReact`.
export default defineConfig({
  plugins: [react()],
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  build: {
    outDir: "../dist/react",
    emptyOutDir: true,
    target: "es2022",
    minify: true,
    sourcemap: false,
    lib: { entry: "src/main.tsx", formats: ["iife"], name: "HelixReact", fileName: () => "helix-react.js" },
  },
});
