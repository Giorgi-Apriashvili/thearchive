<script lang="ts">
  import { api, type Me } from './lib/api'
  import Auth from './routes/Auth.svelte'
  import Workspace from './routes/Workspace.svelte'
  import Download from './routes/Download.svelte'
  import Account from './routes/Account.svelte'
  import Privacy from './routes/Privacy.svelte'
  import Profile from './routes/Profile.svelte'
  import Admin from './routes/admin/Admin.svelte'
  import { router } from './lib/router.svelte'

  // The download page is matched against the live path so the control panel's
  // client-side navigation is reflected here too.
  const downloadToken = $derived(
    router.path.match(/^\/d\/([A-Za-z0-9_-]+)\/?$/)?.[1] ?? null,
  )
  const isAdminRoute = $derived(router.segments[0] === 'admin')
  const isAccountRoute = $derived(router.segments[0] === 'account')
  const isPrivacyRoute = $derived(router.path === '/privacy')
  // /u/<username>. Decoded here once, so the page and the API see the same name.
  const profileName = $derived.by(() => {
    const raw = router.path.match(/^\/u\/([^/]+)\/?$/)?.[1]
    if (!raw) return null
    try {
      return decodeURIComponent(raw)
    } catch {
      return null
    }
  })

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

  // A one-time decision at startup, so it reads the location directly rather than the
  // reactive path: the public download page and the privacy notice need no session, and
  // asking for one would just produce a 401 on every visit from someone with no account —
  // who, for both pages, is much of the audience.
  if (/^\/d\/[A-Za-z0-9_-]+\/?$/.test(location.pathname) || location.pathname === '/privacy') {
    ready = true
  } else {
    loadSession()
  }
</script>

<main class="min-h-screen">
  {#if downloadToken}
    <Download token={downloadToken} />
  {:else if isPrivacyRoute}
    <!-- Before the session gate: readable by anyone, signed in or not. -->
    <Privacy />
  {:else if !ready}
    <div class="flex min-h-screen items-center justify-center text-ink-500">Loading…</div>
  {:else if me && isAdminRoute}
    <!-- The server rejects non-admins on every /api/admin route; this only avoids
         rendering a panel that would answer 403 to everything. -->
    {#if me.role === 'admin'}
      <Admin {me} />
    {:else}
      <div class="flex min-h-screen items-center justify-center text-sm text-ink-500">
        Not available for this account.
      </div>
    {/if}
  {:else if me && isAccountRoute}
    <!-- Reloading the session after an edit is what puts a new picture or name into
         the header straight away. -->
    <Account {me} onChanged={loadSession} />
  {:else if me && profileName}
    <!-- Members only, like the API behind it: signed out, this falls through to the
         sign-in page rather than showing an empty profile. -->
    {#key profileName}
      <Profile username={profileName} />
    {/key}
  {:else if me}
    <Workspace {me} onSignOut={() => (me = null)} />
  {:else}
    <Auth onSignedIn={loadSession} />
  {/if}
</main>
