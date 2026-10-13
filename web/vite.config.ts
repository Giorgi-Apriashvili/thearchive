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
    // Vite inlines small assets as data: URLs, but the Content-Security-Policy allows
    // fonts only from this origin (font-src 'self'), so an inlined font is refused. Fonts
    // always go out as files; everything else keeps the default.
    assetsInlineLimit: (file) => (/\.(woff2?|ttf|otf)$/.test(file) ? false : undefined),
  },
})
