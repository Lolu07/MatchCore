import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

// The dev server proxies /api to the C++ matchcore_server, so the browser only
// ever talks to one origin (no CORS handling needed in the C++ server).
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, ".", "");
  const target = env.MATCHCORE_API ?? "http://127.0.0.1:8080";
  return {
    plugins: [react()],
    server: { proxy: { "/api": target } },
    preview: { proxy: { "/api": target } },
  };
});
