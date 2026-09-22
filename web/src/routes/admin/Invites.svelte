<script lang="ts">
  import { api, ApiError } from '../../lib/api'
  import { shortDate, until } from '../../lib/format'

  interface Invite {
    code: string
    created_at: number
    expires_at?: number
    created_by: string
    used_by?: string
    state: 'open' | 'used' | 'expired'
  }

  let invites = $state<Invite[]>([])
  let error = $state('')
  let fresh = $state('')

  async function load() {
    invites = await api.get<Invite[]>('/api/admin/invites')
  }
  load()

  async function mint() {
    error = ''
    try {
      const result = await api.post<{ code: string }>('/api/invites')
      fresh = result.code
      await load()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not create an invite'
    }
  }

  async function revoke(code: string) {
    error = ''
    try {
      await api.del(`/api/admin/invites/${encodeURIComponent(code)}`)
      if (fresh === code) fresh = ''
      await load()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not revoke that code'
    }
  }

  const stateStyle = (s: string) =>
    s === 'open' ? 'text-accent' : s === 'used' ? 'text-ink-500' : 'text-ink-700'
</script>

<div class="flex items-center justify-between">
  <h1 class="text-lg font-medium">Invites</h1>
  <button
    onclick={mint}
    class="rounded-lg bg-accent px-3 py-1.5 text-xs font-medium text-ink-950 hover:bg-accent-dim"
  >New invite</button>
</div>

{#if error}
  <p class="mt-4 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
{/if}

{#if fresh}
  <div class="mt-4 flex items-center justify-between rounded-lg border border-accent/40 bg-accent/5 px-3 py-2">
    <code class="text-sm text-accent">{fresh}</code>
    <button
      class="text-xs text-ink-500 hover:text-ink-300"
      onclick={() => navigator.clipboard.writeText(fresh)}>Copy</button
    >
  </div>
{/if}

<ul class="mt-4 divide-y divide-ink-800 rounded-xl border border-ink-800">
  {#each invites as invite (invite.code)}
    <li class="flex items-center justify-between gap-4 px-4 py-3">
      <div class="min-w-0">
        <code class="text-sm {stateStyle(invite.state)}">{invite.code}</code>
        <p class="tnum mt-0.5 text-xs text-ink-500">
          by {invite.created_by} · {shortDate(invite.created_at)}
          {#if invite.state === 'used'}· redeemed by {invite.used_by}
          {:else if invite.state === 'open' && invite.expires_at}· expires in {until(invite.expires_at)}
          {:else if invite.state === 'expired'}· expired{/if}
        </p>
      </div>
      {#if invite.state === 'open'}
        <button
          onclick={() => revoke(invite.code)}
          class="shrink-0 text-xs text-ink-500 hover:text-red-400">Revoke</button
        >
      {/if}
    </li>
  {:else}
    <li class="px-4 py-3 text-sm text-ink-500">No invites yet.</li>
  {/each}
</ul>
