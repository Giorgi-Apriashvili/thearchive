<script lang="ts">
  // Changing when one of your links expires. The new time is counted from now, as it is
  // when a link is created, so "7 days" can shorten a link as well as extend it.
  import { api, ApiError } from './api'
  import { expiryChoices, until } from './format'

  let {
    token,
    expiresAt,
    onChanged,
  }: { token: string; expiresAt: number; onChanged: (expiresAt: number) => void } = $props()

  let days = $state(30)
  let busy = $state(false)
  let error = $state('')
  let saved = $state(false)

  // The server refuses an expired link — its files are released within minutes — so the
  // control is not offered for one.
  const expired = $derived(expiresAt <= Date.now() / 1000)
  const when = $derived(
    new Date(expiresAt * 1000).toLocaleString(undefined, {
      day: 'numeric',
      month: 'short',
      year: 'numeric',
      hour: '2-digit',
      minute: '2-digit',
    }),
  )

  async function save() {
    busy = true
    error = ''
    saved = false
    try {
      const out = await api.patch<{ expires_at: number }>(`/api/shares/${token}`, {
        expires_days: days,
      })
      onChanged(out.expires_at)
      saved = true
      setTimeout(() => (saved = false), 2500)
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not change the expiry'
    } finally {
      busy = false
    }
  }
</script>

<div class="mt-3 border-t border-ink-800 pt-3 text-xs">
  {#if expired}
    <p class="text-ink-500">
      This link has expired. To share these files again, upload them into a new link.
    </p>
  {:else}
    <p class="text-ink-500">
      Expires <span class="tnum text-ink-300">{when}</span>, in {until(expiresAt)}.
    </p>
    <div class="mt-2 flex flex-wrap items-center gap-2">
      <label class="flex items-center gap-2">
        <span class="text-ink-500">Change to</span>
        <select
          bind:value={days}
          disabled={busy}
          class="rounded-lg border border-ink-700 bg-ink-900 px-2 py-1 text-xs outline-none focus:border-accent"
        >
          {#each expiryChoices as choice (choice.days)}
            <option value={choice.days}>{choice.label} from now</option>
          {/each}
        </select>
      </label>
      <button
        onclick={save}
        disabled={busy}
        class="rounded-lg border border-ink-700 px-2.5 py-1 hover:border-ink-500 disabled:opacity-40"
      >
        {busy ? 'Saving…' : 'Save'}
      </button>
      {#if saved}<span class="text-accent">Saved</span>{/if}
    </div>
    {#if error}<p class="mt-2 text-red-400">{error}</p>{/if}
  {/if}
</div>
