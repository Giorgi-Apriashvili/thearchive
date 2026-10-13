<script lang="ts">
  // The activity log: sign-ins, links opened and downloaded, uploads, and changes to
  // links, accounts and invitations — kept 90 days, with addresses shortened to their
  // network. The server's own request log lives on the server, not here.
  import { api, ApiError } from '../../lib/api'
  import { chatTime } from '../../lib/format'
  import MemberName from '../../lib/MemberName.svelte'

  interface ActivityItem {
    id: number
    at: number
    event: string
    detail?: string
    network?: string
    user_agent?: string
    actor?: { username: string; display_name?: string; role?: string; exists: boolean }
  }

  const labels: Record<string, string> = {
    'auth.bootstrap': 'Set up the site',
    'auth.register': 'Joined',
    'auth.login': 'Signed in',
    'auth.login_failed': 'Failed sign-in',
    'auth.login_refused': 'Sign-in refused: too many attempts',
    'auth.logout': 'Signed out',
    'auth.password_changed': 'Changed their password',
    'auth.password_change_failed': 'Failed password change',
    'invite.created': 'Made an invitation',
    'invite.revoked': 'Revoked an invitation',
    'upload.completed': 'Uploaded',
    'link.created': 'Made a link',
    'link.opened': 'Opened a link',
    'link.downloaded': 'Downloaded',
    'link.password_failed': 'Wrong link password',
    'link.changed': 'Changed a link',
    'link.files_added': 'Added files to a link',
    'link.file_removed': 'Removed a file from a link',
    'link.removed': 'Removed a link',
    'admin.role': 'Changed a role',
    'admin.renamed': 'Renamed an account',
    'admin.password_reset': 'Reset a password',
    'admin.disabled': 'Disabled an account',
    'admin.enabled': 'Enabled an account',
    'admin.user_deleted': 'Deleted an account',
    'admin.links_revoked': "Revoked someone's links",
    'admin.link_changed': "Changed someone's link",
    'admin.link_removed': "Removed someone's link",
    'admin.message_removed': 'Removed a chat message',
    'admin.marker_cleared': 'Cleared a removal marker',
    'admin.limit': 'Changed a storage limit',
    'chat.room_deleted': 'Deleted a chat room',
  }
  const failures = new Set([
    'auth.login_failed',
    'auth.login_refused',
    'auth.password_change_failed',
    'link.password_failed',
  ])
  const kinds = [
    { value: '', label: 'Everything' },
    { value: 'auth', label: 'Sign-ins and accounts' },
    { value: 'link', label: 'Links' },
    { value: 'upload', label: 'Uploads' },
    { value: 'invite', label: 'Invitations' },
    { value: 'chat', label: 'Chat rooms' },
    { value: 'admin', label: "Administrators' actions" },
  ]

  let items = $state<ActivityItem[]>([])
  let nextBefore = $state<number | null>(null)
  let retention = $state(90)
  let loading = $state(false)
  let error = $state('')

  let person = $state('')
  let kind = $state('')
  let network = $state('')

  function query(before?: number): string {
    const params = new URLSearchParams()
    if (before) params.set('before', String(before))
    if (person.trim()) params.set('user', person.trim())
    if (kind) params.set('event', kind)
    if (network.trim()) params.set('network', network.trim())
    const q = params.toString()
    return q ? `?${q}` : ''
  }

  async function load(more = false) {
    loading = true
    error = ''
    try {
      const data = await api.get<{ items: ActivityItem[]; next_before?: number; retention_days: number }>(
        `/api/admin/activity${query(more && nextBefore ? nextBefore : undefined)}`,
      )
      items = more ? [...items, ...data.items] : data.items
      nextBefore = data.next_before ?? null
      retention = data.retention_days
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not load the activity log'
    } finally {
      loading = false
    }
  }
  load()

  // A short name for a browser string — the whole of it is in the tooltip.
  function browser(ua?: string): string {
    if (!ua) return ''
    const name = /Edg\//.test(ua)
      ? 'Edge'
      : /Firefox\//.test(ua)
        ? 'Firefox'
        : /Chrome\//.test(ua)
          ? 'Chrome'
          : /Safari\//.test(ua)
            ? 'Safari'
            : /curl\//i.test(ua)
              ? 'curl'
              : 'Other'
    const os = /iPhone|iPad/.test(ua)
      ? 'iOS'
      : /Android/.test(ua)
        ? 'Android'
        : /Windows/.test(ua)
          ? 'Windows'
          : /Mac OS X/.test(ua)
            ? 'macOS'
            : /Linux/.test(ua)
              ? 'Linux'
              : ''
    return os ? `${name} on ${os}` : name
  }
</script>

<h1 class="text-lg font-medium">Activity</h1>
<p class="mt-1 text-xs text-ink-500">
  Sign-ins, links opened and downloaded, uploads, and changes to links, accounts and
  invitations. Kept for {retention} days; addresses are shortened to their network. Never
  message text, passwords or link addresses.
</p>

<form
  class="mt-4 grid gap-2 sm:grid-cols-[1fr_1fr_1fr_auto]"
  onsubmit={(e) => {
    e.preventDefault()
    void load()
  }}
>
  <input
    bind:value={person}
    placeholder="Username"
    class="rounded-lg border border-ink-700 bg-ink-900 px-3 py-1.5 text-sm outline-none focus:border-accent"
  />
  <select
    bind:value={kind}
    onchange={() => void load()}
    class="rounded-lg border border-ink-700 bg-ink-900 px-3 py-1.5 text-sm outline-none focus:border-accent"
  >
    {#each kinds as k (k.value)}
      <option value={k.value}>{k.label}</option>
    {/each}
  </select>
  <input
    bind:value={network}
    placeholder="Network, e.g. 203.0.113"
    class="rounded-lg border border-ink-700 bg-ink-900 px-3 py-1.5 text-sm outline-none focus:border-accent"
  />
  <button class="rounded-lg border border-ink-700 px-3 py-1.5 text-sm hover:border-ink-500">Filter</button>
</form>

{#if error}
  <p class="mt-4 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
{/if}

<ul class="mt-4 divide-y divide-ink-800 rounded-xl border border-ink-800">
  {#each items as item (item.id)}
    <li class="px-4 py-2.5 text-sm">
      <div class="flex flex-wrap items-baseline justify-between gap-x-3 gap-y-0.5">
        <p class="min-w-0">
          <span class={failures.has(item.event) ? 'text-red-300' : 'text-ink-100'}>
            {labels[item.event] ?? item.event}
          </span>
          {#if item.detail}<span class="text-ink-300"> · {item.detail}</span>{/if}
        </p>
        <span class="tnum shrink-0 text-xs text-ink-500" title={new Date(item.at * 1000).toLocaleString()}>
          {chatTime(item.at)}
        </span>
      </div>
      <p class="mt-0.5 flex flex-wrap items-baseline gap-x-2 text-xs text-ink-500">
        {#if item.actor}
          {#if item.actor.exists}
            <MemberName
              username={item.actor.username}
              displayName={item.actor.display_name}
              role={item.actor.role ?? 'user'}
            />
          {:else}
            <span>{item.actor.username} (account deleted)</span>
          {/if}
        {:else}
          <span>not signed in</span>
        {/if}
        {#if item.network}<span class="tnum">· {item.network}</span>{/if}
        {#if item.user_agent}<span title={item.user_agent}>· {browser(item.user_agent)}</span>{/if}
      </p>
    </li>
  {:else}
    <li class="px-4 py-6 text-center text-sm text-ink-500">
      {loading ? 'Loading…' : 'Nothing recorded that matches.'}
    </li>
  {/each}
</ul>

{#if nextBefore}
  <button
    class="mt-3 rounded-lg border border-ink-700 px-3 py-1.5 text-xs hover:border-ink-500 disabled:opacity-40"
    disabled={loading}
    onclick={() => void load(true)}
  >{loading ? 'Loading…' : 'Older'}</button>
{/if}
