import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// One self-contained script the greeter page loads; the page shows a plain message when it is missing.
export default defineConfig({
  plugins: [react()],
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  build: {
    outDir: "../dist/react",
    emptyOutDir: true,
    target: "es2022",
    minify: true,
    sourcemap: false,
    lib: { entry: "src/main.tsx", formats: ["iife"], name: "HelixGreeter", fileName: () => "helix-greeter.js" },
  },
});
