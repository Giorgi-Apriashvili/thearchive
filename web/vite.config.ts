import { defineConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'
import tailwindcss from '@tailwindcss/vite'

// The backend owns /api, /files, /d and /healthz. Proxying them in dev means the app
// runs against the real C++ server on the same origin, so session cookies behave
// exactly as they will in production — no CORS, no SameSite surprises.
const backend = process.env.ARCHIVE_BACKEND ?? 'http://127.0.0.1:8080'

export default defineConfig({
  plugins: [svelte(), tailwindcss()],
  server: {
    proxy: {
      '/api': backend,
      '/files': backend,
      '/d': backend,
      '/healthz': backend,
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
})
