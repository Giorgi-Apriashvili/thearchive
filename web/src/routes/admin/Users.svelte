<script lang="ts">
  import { api } from '../../lib/api'
  import { bytes, shortDate } from '../../lib/format'
  import { router, link } from '../../lib/router.svelte'

  interface AdminUser {
    id: number
    username: string
    role: 'user' | 'privileged' | 'admin'
    created_at: number
    disabled_at?: number
    share_count: number
    bytes: number
    invited_by?: string
  }

  let users = $state<AdminUser[]>([])
  let loading = $state(true)

  async function load() {
    users = await api.get<AdminUser[]>('/api/admin/users')
    loading = false
  }
  load()

  const roleStyle = (role: string) =>
    role === 'admin'
      ? 'border-accent/40 text-accent'
      : role === 'privileged'
        ? 'border-ink-500 text-ink-300'
        : 'border-ink-700 text-ink-500'
</script>

<h1 class="text-lg font-medium">Users</h1>

{#if loading}
  <p class="mt-4 text-sm text-ink-500">Loading…</p>
{:else}
  <ul class="mt-4 divide-y divide-ink-800 rounded-xl border border-ink-800">
    {#each users as user (user.id)}
      <li>
        <a
          href={`/admin/users/${user.id}`}
          onclick={(e) => link(e, `/admin/users/${user.id}`)}
          class="flex items-center justify-between gap-4 px-4 py-3 transition hover:bg-ink-900"
        >
          <div class="min-w-0">
            <p class="flex items-center gap-2 text-sm">
              <span class="truncate" class:text-ink-500={user.disabled_at}>{user.username}</span>
              <span class="rounded border px-1.5 py-0.5 text-[10px] uppercase {roleStyle(user.role)}">
                {user.role}
              </span>
              {#if user.disabled_at}
                <span class="rounded border border-red-900 px-1.5 py-0.5 text-[10px] uppercase text-red-400">
                  disabled
                </span>
              {/if}
            </p>
            <p class="tnum mt-0.5 text-xs text-ink-500">
              joined {shortDate(user.created_at)}
              {#if user.invited_by}· invited by {user.invited_by}{/if}
            </p>
          </div>
          <div class="tnum shrink-0 text-right text-xs text-ink-500">
            <p>{user.share_count} share{user.share_count === 1 ? '' : 's'}</p>
            <p class="mt-0.5">{bytes(user.bytes)}</p>
          </div>
        </a>
      </li>
    {/each}
  </ul>
{/if}
