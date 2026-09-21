<script lang="ts">
  import { api, ApiError } from '../lib/api'

  let { onSignedIn }: { onSignedIn: () => void } = $props()

  type Mode = 'login' | 'redeem' | 'bootstrap'

  let mode = $state<Mode>('login')
  let username = $state('')
  let password = $state('')
  let invite = $state('')
  let error = $state('')
  let busy = $state(false)

  // An instance with no users at all needs its first account created without an invite,
  // since there is nobody who could have issued one.
  api
    .get<{ initialised: boolean }>('/api/status')
    .then((status) => {
      if (!status.initialised) mode = 'bootstrap'
    })
    .catch(() => {})

  const heading = $derived(
    mode === 'bootstrap' ? 'Set up TheArchive' : mode === 'redeem' ? 'Redeem an invite' : 'Sign in',
  )

  // Distinct from the heading: a button repeating the title above it says nothing about
  // what pressing it does.
  const action = $derived(
    mode === 'bootstrap' ? 'Create admin account' : mode === 'redeem' ? 'Create account' : 'Sign in',
  )

  async function submit(event: SubmitEvent) {
    event.preventDefault()
    error = ''
    busy = true
    try {
      if (mode === 'login') {
        await api.post('/api/auth/login', { username, password })
      } else if (mode === 'redeem') {
        await api.post('/api/auth/register', { invite, username, password })
      } else {
        await api.post('/api/auth/bootstrap', { username, password })
      }
      onSignedIn()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'something went wrong'
    } finally {
      busy = false
    }
  }
</script>

<div class="flex min-h-screen items-center justify-center px-4">
  <div class="w-full max-w-sm">
    <h1 class="text-2xl font-semibold tracking-tight">TheArchive</h1>
    <p class="mt-1 text-sm text-ink-500">Drop files, share a link, forget about it.</p>

    <form class="mt-8 space-y-4" onsubmit={submit}>
      <h2 class="text-sm font-medium text-ink-300">{heading}</h2>

      {#if mode === 'bootstrap'}
        <p class="text-xs text-ink-500">
          No accounts exist yet. This first one becomes the administrator, and everyone
          after it joins by invite.
        </p>
      {/if}

      {#if mode === 'redeem'}
        <label class="block">
          <span class="text-xs text-ink-500">Invite code</span>
          <input
            bind:value={invite}
            required
            autocomplete="off"
            class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
          />
        </label>
      {/if}

      <label class="block">
        <span class="text-xs text-ink-500">Username</span>
        <input
          bind:value={username}
          required
          autocomplete="username"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>

      <label class="block">
        <span class="text-xs text-ink-500">Password</span>
        <input
          type="password"
          bind:value={password}
          required
          autocomplete={mode === 'login' ? 'current-password' : 'new-password'}
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
        {#if mode !== 'login'}
          <span class="mt-1 block text-xs text-ink-500">At least 6 characters.</span>
        {/if}
      </label>

      {#if error}
        <p class="rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
      {/if}

      <button
        type="submit"
        disabled={busy}
        class="w-full rounded-lg bg-accent px-3 py-2 text-sm font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-50"
      >
        {busy ? 'Working…' : action}
      </button>
    </form>

    {#if mode !== 'bootstrap'}
      <button
        class="mt-4 text-xs text-ink-500 underline-offset-2 hover:text-ink-300 hover:underline"
        onclick={() => {
          mode = mode === 'login' ? 'redeem' : 'login'
          error = ''
        }}
      >
        {mode === 'login' ? 'Have an invite code?' : 'Already have an account?'}
      </button>
    {/if}
  </div>
</div>
