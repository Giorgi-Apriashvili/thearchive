<script lang="ts">
  import { api, ApiError } from '../../lib/api'
  import { bytes, shortDate, until } from '../../lib/format'
  import { router, link } from '../../lib/router.svelte'
  import VisibilityToggle from '../../lib/VisibilityToggle.svelte'
  import Confirm from '../../lib/Confirm.svelte'

  let { id }: { id: string } = $props()

  interface UserShare {
    token: string
    title: string
    created_at: number
    expires_at: number
    download_count: number
    revoked: boolean
    password_protected: boolean
    visibility: 'private' | 'public'
    url: string
    file_count: number
    total_bytes: number
  }

  interface Detail {
    id: number
    username: string
    role: 'user' | 'privileged' | 'admin'
    created_at: number
    disabled_at?: number
    share_count: number
    bytes: number
    invited_by?: string
    shares: UserShare[]
    invited: string[]
  }

  let user = $state<Detail | null>(null)
  let error = $state('')
  let busy = $state('')
  // Deletion cascades and cannot be undone, so it takes a typed confirmation rather
  // than a button that is one mis-click away from removing someone's account.
  let confirmName = $state('')

  async function load() {
    try {
      user = await api.get<Detail>(`/api/admin/users/${id}`)
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not load this user'
    }
  }
  load()

  async function act(action: string, body?: unknown) {
    error = ''
    busy = action
    try {
      await api.post(`/api/admin/users/${id}/${action}`, body)
      await load()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'that did not work'
    } finally {
      busy = ''
    }
  }

  async function remove() {
    error = ''
    busy = 'delete'
    try {
      await api.del(`/api/admin/users/${id}`)
      router.go('/admin/users')
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not delete the account'
      busy = ''
    }
  }

  const roles = ['user', 'privileged', 'admin'] as const

  let renaming = $state('')
  let confirmingRename = $state(false)

  async function rename() {
    confirmingRename = false
    error = ''
    busy = 'username'
    try {
      await api.post(`/api/admin/users/${id}/username`, { username: renaming.trim() })
      renaming = ''
      await load()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not change the username'
    } finally {
      busy = ''
    }
  }

  // The only recovery path there is — nothing here sends mail. The generated password is
  // returned once and never stored, so it is held in memory until this page is left.
  let confirmingReset = $state(false)
  let issued = $state('')

  async function resetPassword() {
    confirmingReset = false
    error = ''
    busy = 'password'
    try {
      const result = await api.post<{ password: string }>(`/api/admin/users/${id}/password`)
      issued = result.password
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not reset the password'
    } finally {
      busy = ''
    }
  }

  async function revokeShare(token: string) {
    await api.del(`/api/admin/shares/${token}`)
    await load()
  }
</script>

<a
  href="/admin/users"
  onclick={(e) => link(e, '/admin/users')}
  class="text-xs text-ink-500 hover:text-ink-300">← Users</a
>

{#if error}
  <p class="mt-4 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
{/if}

{#if user}
  <h1 class="mt-3 text-lg font-medium">
    {user.username}
    {#if user.disabled_at}
      <span class="ml-2 rounded border border-red-900 px-1.5 py-0.5 text-[10px] uppercase text-red-400">
        disabled
      </span>
    {/if}
  </h1>
  <p class="tnum mt-1 text-sm text-ink-500">
    joined {shortDate(user.created_at)}
    {#if user.invited_by}· invited by {user.invited_by}{/if}
    · {bytes(user.bytes)} across {user.share_count} live share{user.share_count === 1 ? '' : 's'}
  </p>

  <section class="mt-6 rounded-xl border border-ink-800 p-4">
    <h2 class="text-sm font-medium text-ink-300">Username</h2>
    <p class="mt-1 text-xs text-ink-500">
      This is what they sign in with, so tell them — nothing here can. Their uploads and
      their chat history follow the new name; their session and their password do not
      change.
    </p>
    <form
      onsubmit={(e) => {
        e.preventDefault()
        if (renaming.trim()) confirmingRename = true
      }}
      class="mt-3 flex gap-2"
    >
      <input
        bind:value={renaming}
        placeholder={user.username}
        autocapitalize="none"
        autocorrect="off"
        spellcheck="false"
        class="flex-1 rounded-lg border border-ink-700 bg-ink-950 px-3 py-1.5 text-xs outline-none focus:border-accent"
      />
      <button
        disabled={busy !== '' || !renaming.trim() || renaming.trim() === user.username}
        class="shrink-0 rounded-lg border border-ink-700 px-3 py-1.5 text-xs text-ink-300 transition hover:border-ink-500 disabled:opacity-40"
      >Rename</button>
    </form>
  </section>

  <section class="mt-4 rounded-xl border border-ink-800 p-4">
    <h2 class="text-sm font-medium text-ink-300">Role</h2>
    <p class="mt-1 text-xs text-ink-500">
      <code>privileged</code> grants nothing beyond <code>user</code> today — it exists so
      the tier can be assigned before deciding what it should mean.
    </p>
    <div class="mt-3 flex gap-2">
      {#each roles as role (role)}
        <button
          disabled={busy !== '' || user.role === role}
          onclick={() => act('role', { role })}
          class="rounded-lg border px-3 py-1.5 text-xs transition disabled:opacity-40 {user.role === role
            ? 'border-accent text-accent'
            : 'border-ink-700 text-ink-300 hover:border-ink-500'}"
        >{role}</button>
      {/each}
    </div>
  </section>

  <section class="mt-4 grid gap-3 sm:grid-cols-2">
    <button
      disabled={busy !== ''}
      onclick={() => act('revoke')}
      class="rounded-lg border border-ink-700 px-3 py-2 text-left text-xs hover:border-ink-500 disabled:opacity-40"
    >
      <span class="block text-ink-100">Revoke all their links</span>
      <span class="mt-0.5 block text-ink-500">Files and attribution stay; the links die.</span>
    </button>

    <button
      disabled={busy !== ''}
      onclick={() => act(user!.disabled_at ? 'enable' : 'disable')}
      class="rounded-lg border border-ink-700 px-3 py-2 text-left text-xs hover:border-ink-500 disabled:opacity-40"
    >
      <span class="block text-ink-100">
        {user.disabled_at ? 'Re-enable the account' : 'Disable the account'}
      </span>
      <span class="mt-0.5 block text-ink-500">
        {user.disabled_at
          ? 'Restores sign-in.'
          : 'Blocks sign-in and ends their sessions now. Reversible.'}
      </span>
    </button>
  </section>

  <section class="mt-4 rounded-xl border border-ink-800 p-4">
    <h2 class="text-sm font-medium text-ink-300">Reset password</h2>
    <p class="mt-1 text-xs text-ink-500">
      Sets a new password and ends every session they have. There is no email in this
      system, so this is the only way back in for someone who has forgotten theirs.
    </p>

    {#if issued}
      <div class="mt-3 rounded-lg border border-accent/40 bg-accent/5 px-3 py-2">
        <div class="flex items-center justify-between gap-3">
          <code class="truncate text-sm text-accent">{issued}</code>
          <button
            class="shrink-0 text-xs text-ink-500 hover:text-ink-300"
            onclick={() => navigator.clipboard.writeText(issued)}>Copy</button
          >
        </div>
        <p class="mt-1.5 text-xs text-ink-500">
          Only stored as a hash, so this cannot be shown again. Give it to
          {user.username} and have them change it on their account page.
        </p>
      </div>
    {:else}
      <button
        disabled={busy !== ''}
        onclick={() => (confirmingReset = true)}
        class="mt-3 rounded-lg border border-ink-700 px-3 py-1.5 text-xs text-ink-300 transition hover:border-ink-500 disabled:opacity-40"
      >Reset password</button>
    {/if}
  </section>

  <section class="mt-4 rounded-xl border border-red-950 bg-red-950/20 p-4">
    <h2 class="text-sm font-medium text-red-300">Delete account</h2>
    <p class="mt-1 text-xs text-ink-500">
      Removes the account, its sessions and all of its shares. Uploaded files lose their
      last reference and leave on the next sweep. This cannot be undone — type
      <code class="text-ink-300">{user.username}</code> to confirm.
    </p>
    <div class="mt-3 flex gap-2">
      <input
        bind:value={confirmName}
        placeholder={user.username}
        autocapitalize="none"
        autocorrect="off"
        spellcheck="false"
        class="flex-1 rounded-lg border border-ink-700 bg-ink-950 px-3 py-1.5 text-xs outline-none focus:border-red-800"
      />
      <button
        disabled={busy !== '' || confirmName !== user.username}
        onclick={remove}
        class="shrink-0 rounded-lg border border-red-900 px-3 py-1.5 text-xs text-red-300 transition hover:bg-red-950 disabled:opacity-30"
      >Delete</button>
    </div>
  </section>

  {#if user.invited.length}
    <section class="mt-6">
      <h2 class="text-sm font-medium text-ink-300">Brought in</h2>
      <p class="mt-2 text-sm text-ink-500">{user.invited.join(', ')}</p>
    </section>
  {/if}

  <section class="mt-6">
    <h2 class="text-sm font-medium text-ink-300">Shares</h2>
    {#if user.shares.length === 0}
      <p class="mt-2 text-sm text-ink-500">None.</p>
    {:else}
      <ul class="mt-3 divide-y divide-ink-800 rounded-xl border border-ink-800">
        {#each user.shares as share (share.token)}
          <li class="flex items-center justify-between gap-4 px-4 py-3">
            <div class="min-w-0">
              <p class="truncate text-sm" class:text-ink-500={share.revoked}>
                {share.title || 'Untitled'}
                {#if share.revoked}<span class="text-xs">· revoked</span>{/if}
              </p>
              <p class="tnum mt-0.5 text-xs text-ink-500">
                {share.file_count} file{share.file_count === 1 ? '' : 's'} ·
                {bytes(share.total_bytes)} · {share.download_count} download{share.download_count === 1 ? '' : 's'}
                {#if !share.revoked}· expires in {until(share.expires_at)}{/if}
                {#if share.password_protected}· password{/if}
              </p>
            </div>
            {#if !share.revoked}
              <div class="flex shrink-0 items-center gap-3">
                <a href={`/d/${share.token}`} class="text-xs text-ink-500 hover:text-ink-300">Open</a>
                <button
                  class="text-xs text-ink-500 hover:text-ink-300"
                  onclick={() => navigator.clipboard.writeText(share.url)}>Copy</button
                >
                <VisibilityToggle
                  visibility={share.visibility}
                  endpoint={`/api/admin/shares/${share.token}`}
                  onChanged={(next) => (share.visibility = next)}
                />
                <button
                  onclick={() => revokeShare(share.token)}
                  class="text-xs text-ink-500 hover:text-red-400">Revoke</button
                >
              </div>
            {/if}
          </li>
        {/each}
      </ul>
    {/if}
  </section>

  {#if confirmingRename}
    <Confirm
      title="Rename {user.username} to {renaming.trim().toLowerCase()}?"
      body="They sign in with this name, and nothing here will tell them it changed — until you do, they cannot get back in. Their uploads and chat history will show the new name."
      confirmLabel="Rename"
      onConfirm={rename}
      onCancel={() => (confirmingRename = false)}
    />
  {/if}

  {#if confirmingReset}
    <Confirm
      title="Reset {user.username}'s password?"
      body="They will be signed out everywhere and will not be able to sign in again until you give them the new password. It is shown once and cannot be recovered afterwards."
      confirmLabel="Reset password"
      onConfirm={resetPassword}
      onCancel={() => (confirmingReset = false)}
    />
  {/if}
{/if}
