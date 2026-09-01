// Test-only config, deliberately separate from vite.config.ts: this harness
// exists for useTurn.test.tsx alone (see that file's header comment), and
// keeping it in its own file means the dev server and the production build
// config in vite.config.ts are untouched by adding it.
import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.tsx'],
  },
})
