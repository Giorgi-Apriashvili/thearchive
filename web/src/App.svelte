<script lang="ts">
  import { api, type Me } from './lib/api'
  import Auth from './routes/Auth.svelte'
  import Workspace from './routes/Workspace.svelte'
  import Download from './routes/Download.svelte'

  // Three screens and no client-side navigation between them — every transition is a
  // real page load — so a regex over the path is a complete router here. A routing
  // library would be more moving parts than the app has routes.
  const downloadToken = location.pathname.match(/^\/d\/([A-Za-z0-9_-]+)\/?$/)?.[1] ?? null

  let me = $state<Me | null>(null)
  let ready = $state(false)

  async function loadSession() {
    try {
      me = await api.get<Me>('/api/me')
    } catch {
      me = null
    }
    ready = true
  }

  if (downloadToken) {
    ready = true
  } else {
    loadSession()
  }
</script>

<main class="min-h-screen">
  {#if downloadToken}
    <Download token={downloadToken} />
  {:else if !ready}
    <div class="flex min-h-screen items-center justify-center text-ink-500">Loading…</div>
  {:else if me}
    <Workspace {me} onSignOut={() => (me = null)} />
  {:else}
    <Auth onSignedIn={loadSession} />
  {/if}
</main>
