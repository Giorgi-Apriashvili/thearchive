<script lang="ts">
  import { api, ApiError } from '../../lib/api'
  import { bytes, until } from '../../lib/format'

  interface AdminShare {
    token: string
    title: string
    owner: string
    created_at: number
    expires_at: number
    download_count: number
    password_protected: boolean
    file_count: number
    total_bytes: number
  }

  let shares = $state<AdminShare[]>([])
  let error = $state('')

  async function load() {
    shares = await api.get<AdminShare[]>('/api/admin/shares')
  }
  load()

  async function revoke(token: string) {
    error = ''
    try {
      await api.del(`/api/admin/shares/${token}`)
      await load()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not revoke that share'
    }
  }
</script>

<h1 class="text-lg font-medium">Shares</h1>
<p class="mt-1 text-xs text-ink-500">Every live link, whoever created it.</p>

{#if error}
  <p class="mt-4 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
{/if}

<ul class="mt-4 divide-y divide-ink-800 rounded-xl border border-ink-800">
  {#each shares as share (share.token)}
    <li class="flex items-center justify-between gap-4 px-4 py-3">
      <div class="min-w-0">
        <a href={`/d/${share.token}`} class="block truncate text-sm hover:text-accent">
          {share.title || 'Untitled'}
        </a>
        <p class="tnum mt-0.5 text-xs text-ink-500">
          {share.owner} · {share.file_count} file{share.file_count === 1 ? '' : 's'} ·
          {bytes(share.total_bytes)} · expires in {until(share.expires_at)} ·
          {share.download_count} download{share.download_count === 1 ? '' : 's'}
          {#if share.password_protected}· password{/if}
        </p>
      </div>
      <button
        onclick={() => revoke(share.token)}
        class="shrink-0 text-xs text-ink-500 hover:text-red-400">Revoke</button
      >
    </li>
  {:else}
    <li class="px-4 py-3 text-sm text-ink-500">No live shares.</li>
  {/each}
</ul>
