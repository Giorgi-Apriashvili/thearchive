<script lang="ts">
  import { api } from '../../lib/api'
  import { bytes } from '../../lib/format'

  interface Overview {
    users: number
    blobs: number
    stored_bytes: number
    live_shares: number
    open_invites: number
    uploads_in_flight: number
    orphan_bytes: number
    by_uploader: { username: string; blobs: number; bytes: number }[]
    disk_total?: number
    disk_available?: number
  }

  let data = $state<Overview | null>(null)
  async function load() {
    data = await api.get<Overview>('/api/admin/overview')
  }
  load()

  const stats = $derived(
    data
      ? [
          { label: 'Users', value: String(data.users) },
          { label: 'Live shares', value: String(data.live_shares) },
          { label: 'Open invites', value: String(data.open_invites) },
          { label: 'Stored', value: bytes(data.stored_bytes) },
          { label: 'Files', value: String(data.blobs) },
          { label: 'Uploading', value: String(data.uploads_in_flight) },
        ]
      : [],
  )
</script>

<h1 class="text-lg font-medium">Overview</h1>

{#if data}
  <dl class="mt-4 grid grid-cols-2 gap-3 sm:grid-cols-3">
    {#each stats as stat (stat.label)}
      <div class="rounded-xl border border-ink-800 px-4 py-3">
        <dt class="text-xs text-ink-500">{stat.label}</dt>
        <dd class="tnum mt-1 text-xl">{stat.value}</dd>
      </div>
    {/each}
  </dl>

  {#if data.disk_total}
    <p class="tnum mt-4 text-xs text-ink-500">
      {bytes(data.disk_available ?? 0)} free of {bytes(data.disk_total)} on the volume
      {#if data.orphan_bytes > 0}
        · {bytes(data.orphan_bytes)} unreferenced, awaiting the next sweep
      {/if}
    </p>
  {/if}

  <h2 class="mt-8 text-sm font-medium text-ink-300">Stored by uploader</h2>
  <p class="mt-1 text-xs text-ink-500">
    Attributed to whoever uploaded each file first — deduplication means a second
    uploader of the same bytes adds nothing.
  </p>
  <ul class="mt-3 divide-y divide-ink-800 rounded-xl border border-ink-800">
    {#each data.by_uploader as row (row.username)}
      <li class="flex items-center justify-between px-4 py-2.5 text-sm">
        <span>{row.username}</span>
        <span class="tnum text-xs text-ink-500">{row.blobs} file{row.blobs === 1 ? '' : 's'} · {bytes(row.bytes)}</span>
      </li>
    {/each}
  </ul>
{:else}
  <p class="mt-4 text-sm text-ink-500">Loading…</p>
{/if}
