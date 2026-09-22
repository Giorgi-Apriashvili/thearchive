<script lang="ts">
  import { api, ApiError, type Me } from '../lib/api'
  import { link } from '../lib/router.svelte'

  let { me }: { me: Me } = $props()

  const MIN_LENGTH = 6

  let current = $state('')
  let next = $state('')
  let confirm = $state('')
  let busy = $state(false)
  let error = $state('')
  let done = $state(false)

  // Checked here as well as on the server so the two obvious mistakes cost no round
  // trip. The server remains the authority — these only decide whether to bother it.
  const problem = $derived.by(() => {
    if (!current || !next || !confirm) return 'fill in all three'
    if (next.length < MIN_LENGTH) return `the new password needs at least ${MIN_LENGTH} characters`
    if (next !== confirm) return 'the two new passwords do not match'
    if (next === current) return 'the new password must be different'
    return null
  })

  async function submit(event: SubmitEvent) {
    event.preventDefault()
    if (problem || busy) return
    error = ''
    busy = true
    try {
      await api.post('/api/auth/password', { current_password: current, new_password: next })
      current = ''
      next = ''
      confirm = ''
      done = true
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not change your password'
    } finally {
      busy = false
    }
  }
</script>

<div class="mx-auto max-w-lg px-4 py-10">
  <a href="/" onclick={(e) => link(e, '/')} class="text-xs text-ink-500 hover:text-ink-300">
    ← Back
  </a>

  <h1 class="mt-3 text-lg font-medium">Account</h1>
  <p class="mt-1 text-sm text-ink-500">
    {me.username}
    {#if me.role !== 'user'}· {me.role}{/if}
  </p>

  <section class="mt-6 rounded-xl border border-ink-800 p-4">
    <h2 class="text-sm font-medium text-ink-300">Change your password</h2>
    <p class="mt-1 text-xs text-ink-500">
      You will stay signed in here. Everywhere else you are signed in will be signed out —
      which is the point, if you are changing it because someone else might know it.
    </p>

    <form onsubmit={submit} class="mt-4 space-y-3">
      <label class="block">
        <span class="text-xs text-ink-500">Current password</span>
        <input
          type="password"
          bind:value={current}
          autocomplete="current-password"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block">
        <span class="text-xs text-ink-500">New password</span>
        <input
          type="password"
          bind:value={next}
          autocomplete="new-password"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block">
        <span class="text-xs text-ink-500">Confirm new password</span>
        <input
          type="password"
          bind:value={confirm}
          autocomplete="new-password"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>

      {#if error}
        <p class="rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
      {/if}
      {#if done}
        <p class="rounded-lg border border-accent/40 bg-accent/5 px-3 py-2 text-sm text-ink-300">
          Password changed. Any other sessions have been signed out.
        </p>
      {/if}

      <div class="flex items-center justify-between gap-3">
        <!-- The reason the button is disabled, rather than a button that looks broken.
             Only once something has been typed: telling someone to fill in a form they
             have not started is noise. -->
        <span class="text-xs text-ink-500">
          {#if problem && (current || next || confirm)}{problem}{/if}
        </span>
        <button
          disabled={busy || problem !== null}
          class="shrink-0 rounded-lg bg-accent px-3 py-2 text-sm font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
        >
          {busy ? 'Changing…' : 'Change password'}
        </button>
      </div>
    </form>
  </section>

  <p class="mt-4 text-xs text-ink-500">
    Forgotten it instead? There is no email in this system, so nothing can be sent to you —
    ask an administrator to reset it.
  </p>
</div>
