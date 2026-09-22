<script lang="ts">
  import * as tus from 'tus-js-client'
  import {
    api,
    ApiError,
    type CreatedShare,
    type Me,
    type ShareDetail,
    type ShareFile,
    type ShareSummary,
    type StorageInfo,
  } from '../lib/api'
  import { bytes, shortDate, until } from '../lib/format'
  import { link } from '../lib/router.svelte'
  import Confirm from '../lib/Confirm.svelte'

  let { me, onSignOut }: { me: Me; onSignOut: () => void } = $props()

  interface Item {
    file: File
    uploadId: string | null
    sent: number
    total: number
    status: 'uploading' | 'done' | 'error'
    error?: string
    handle?: tus.Upload
  }

  let items = $state<Item[]>([])
  let shares = $state<ShareSummary[]>([])
  let dragging = $state(false)
  let creating = $state(false)
  let created = $state<CreatedShare | null>(null)
  let copied = $state(false)
  let error = $state('')
  let inviteCode = $state('')

  let title = $state('')
  let expiresDays = $state(30)
  let password = $state('')
  let maxDownloads = $state('')

  const ready = $derived(items.filter((i) => i.status === 'done' && i.uploadId))
  const busy = $derived(items.some((i) => i.status === 'uploading'))
  const totalBytes = $derived(items.reduce((sum, i) => sum + i.total, 0))

  let storage = $state<StorageInfo | null>(null)

  async function loadShares() {
    try {
      shares = await api.get<ShareSummary[]>('/api/shares')
    } catch {
      shares = []
    }
    try {
      storage = await api.get<StorageInfo>('/api/storage')
    } catch {
      storage = null
    }
  }
  loadShares()

  // Space the archive occupies, versus everything else on the same partition. The disk
  // is shared, so "free" is not a quota — it is simply what is left on the volume.
  const disk = $derived.by(() => {
    if (!storage?.disk_total) return null
    const total = storage.disk_total
    const free = storage.disk_available ?? 0
    const mine = Math.min(storage.stored_bytes, total)
    const others = Math.max(0, total - free - mine)
    return {
      total,
      free,
      mine,
      others,
      minePct: (mine / total) * 100,
      othersPct: (others / total) * 100,
      saved: Math.max(0, storage.shared_logical_bytes - storage.shared_stored_bytes),
    }
  })

  function enqueue(files: FileList | File[]) {
    for (const file of Array.from(files)) {
      // Push first, then read the element back. $state deep-proxies array contents, and
      // the object literal above is the raw target — mutating it from the callbacks
      // below would update the data without notifying the proxy's signals, leaving
      // progress stuck at 0% and the file never appearing as finished.
      items.push({ file, uploadId: null, sent: 0, total: file.size, status: 'uploading' })
      const item = items[items.length - 1]

      const handle = new tus.Upload(file, {
        endpoint: '/files',
        // The server buffers each request body in memory and caps it at 64 MiB. Without
        // an explicit chunk size tus-js-client sends the whole file in one PATCH, which
        // fails the moment anyone uploads a video.
        chunkSize: 16 * 1024 * 1024,
        retryDelays: [0, 1000, 3000, 5000, 10000],
        metadata: {
          filename: file.name,
          filetype: file.type,
          lastModified: String(file.lastModified),
          relativePath: (file as any).webkitRelativePath || '',
        },
        // Fires as soon as the server has created the upload, long before it finishes.
        // Without this, removing an in-flight upload could not tell the server to
        // discard it and the partial file would linger until its TTL.
        onUploadUrlAvailable: () => {
          item.uploadId = handle.url?.split('/').pop() ?? null
        },
        onProgress: (sent, total) => {
          item.sent = sent
          item.total = total
        },
        onSuccess: () => {
          item.uploadId = handle.url?.split('/').pop() ?? null
          item.sent = item.total
          item.status = 'done'
        },
        onError: (err) => {
          item.status = 'error'
          item.error = err.message
        },
      })
      item.handle = handle
      handle.start()
    }
  }

  async function remove(item: Item) {
    item.handle?.abort()
    // Abort only stops this browser sending. Terminating tells the server to discard the
    // partial file now, rather than leaving it on disk until the upload expires.
    if (item.uploadId) {
      try {
        await api.del(`/files/${item.uploadId}`)
      } catch {
        // Already gone, or never created — nothing to reclaim either way.
      }
    }
    items = items.filter((i) => i !== item)
    await loadShares()
  }

  async function createShare() {
    error = ''
    creating = true
    try {
      const body: Record<string, unknown> = {
        uploads: ready.map((i) => i.uploadId),
        expires_days: expiresDays,
      }
      if (title.trim()) body.title = title.trim()
      if (password) body.password = password
      if (maxDownloads) body.max_downloads = Number(maxDownloads)

      created = await api.post<CreatedShare>('/api/shares', body)
      items = []
      title = ''
      password = ''
      maxDownloads = ''
      await loadShares()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not create the link'
    } finally {
      creating = false
    }
  }

  async function copy(text: string) {
    await navigator.clipboard.writeText(text)
    copied = true
    setTimeout(() => (copied = false), 1500)
  }

  async function revoke(token: string) {
    await api.del(`/api/shares/${token}`)
    if (expanded === token) expanded = null
    await loadShares()
  }

  let expanded = $state<string | null>(null)
  let expandedFiles = $state<ShareFile[]>([])
  let expanding = $state(false)

  async function toggle(token: string) {
    if (expanded === token) {
      expanded = null
      return
    }
    expanded = token
    expandedFiles = []
    expanding = true
    try {
      // The owner is exempt from the share password, so this works even on a protected
      // link without prompting for something they chose themselves.
      const detail = await api.get<ShareDetail>(`/api/shares/${token}`)
      expandedFiles = detail.files
    } catch {
      expanded = null
    } finally {
      expanding = false
    }
  }

  async function removeFile(token: string, fileId: number) {
    const result = await api.del<{ files_remaining: number; share_revoked: boolean }>(
      `/api/shares/${token}/files/${fileId}`,
    )
    expandedFiles = expandedFiles.filter((f) => f.id !== fileId)
    // Removing the last file revokes the share, so there is nothing left to show.
    if (result.share_revoked) expanded = null
    await loadShares()
  }

  let confirmingInvite = $state(false)

  async function makeInvite() {
    confirmingInvite = false
    const result = await api.post<{ code: string }>('/api/invites')
    inviteCode = result.code
  }

  async function signOut() {
    await api.post('/api/auth/logout')
    onSignOut()
  }
</script>

<div class="mx-auto max-w-3xl px-4 py-10">
  <header class="flex items-baseline justify-between">
    <div>
      <h1 class="wordmark text-3xl">Weekend<em>Archive</em></h1>
      <p class="mt-1 text-sm text-ink-500">Signed in as {me.username}</p>
    </div>
    <div class="flex items-center gap-4 text-xs">
      <!-- Minting is admin-only server-side; showing the button to everyone would just
           offer a 403. -->
      {#if me.role !== 'user'}
        <button class="text-ink-500 hover:text-ink-300" onclick={() => (confirmingInvite = true)}>Invite</button>
      {/if}
      {#if me.role === 'admin'}
        <a href="/admin" onclick={(e) => link(e, '/admin')} class="text-ink-500 hover:text-ink-300">
          Control panel
        </a>
      {/if}
      <button class="text-ink-500 hover:text-ink-300" onclick={signOut}>Sign out</button>
    </div>
  </header>

  {#if inviteCode}
    <div class="mt-4 flex items-center justify-between rounded-lg border border-ink-700 bg-ink-900 px-3 py-2">
      <code class="text-sm text-accent">{inviteCode}</code>
      <button class="text-xs text-ink-500 hover:text-ink-300" onclick={() => copy(inviteCode)}>Copy</button>
    </div>
  {/if}

  {#if disk}
    <section class="mt-6 rounded-xl border border-ink-800 px-4 py-3">
      <div class="flex items-baseline justify-between text-xs">
        <span class="text-ink-300">Storage</span>
        <span class="tnum text-ink-500">{bytes(disk.free)} free of {bytes(disk.total)}</span>
      </div>

      <!-- Two segments: what WeekendArchive holds, and what else is on the same volume.
           Separating them makes it obvious whether a full disk is our doing. -->
      <div class="mt-2 flex h-1.5 overflow-hidden rounded bg-ink-800" title="{bytes(disk.mine)} archive, {bytes(disk.others)} other">
        <div class="h-full bg-accent" style="width: {disk.minePct}%"></div>
        <div class="h-full bg-ink-700" style="width: {disk.othersPct}%"></div>
      </div>

      <p class="tnum mt-2 text-xs text-ink-500">
        <span class="text-accent">■</span>
        WeekendArchive {bytes(disk.mine)}
        {#if storage}({storage.blob_count} file{storage.blob_count === 1 ? '' : 's'}){/if}
        · <span class="text-ink-700">■</span> other {bytes(disk.others)}
        {#if disk.saved > 0}
          · deduplication saved {bytes(disk.saved)}
        {/if}
        {#if storage?.incoming_bytes}
          · {bytes(storage.incoming_bytes)} uploading
        {/if}
      </p>
    </section>
  {/if}

  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div
    role="region"
    aria-label="Drop files to upload"
    class="mt-8 rounded-xl border-2 border-dashed p-10 text-center transition {dragging
      ? 'border-accent bg-ink-900'
      : 'border-ink-700'}"
    ondragover={(e) => {
      e.preventDefault()
      dragging = true
    }}
    ondragleave={() => (dragging = false)}
    ondrop={(e) => {
      e.preventDefault()
      dragging = false
      if (e.dataTransfer?.files) enqueue(e.dataTransfer.files)
    }}
  >
    <p class="text-sm text-ink-300">Drop files here</p>
    <p class="mt-1 text-xs text-ink-500">or</p>
    <label class="mt-3 inline-block cursor-pointer rounded-lg border border-ink-700 px-3 py-1.5 text-sm hover:border-ink-500">
      Choose files
      <input
        type="file"
        multiple
        class="hidden"
        onchange={(e) => {
          const input = e.currentTarget as HTMLInputElement
          if (input.files) enqueue(input.files)
          input.value = ''
        }}
      />
    </label>
  </div>

  {#if items.length}
    <ul class="mt-6 space-y-2">
      {#each items as item (item.file.name + item.file.lastModified + item.total)}
        <li class="rounded-lg border border-ink-700 bg-ink-900 px-3 py-2">
          <div class="flex items-center justify-between gap-3 text-sm">
            <span class="truncate">{item.file.name}</span>
            <span class="tnum shrink-0 text-xs text-ink-500">
              {#if item.status === 'error'}
                <span class="text-red-400">{item.error}</span>
              {:else if item.status === 'done'}
                {bytes(item.total)}
              {:else}
                {Math.floor((item.sent / Math.max(1, item.total)) * 100)}%
              {/if}
            </span>
            <button class="shrink-0 text-xs text-ink-500 hover:text-ink-300" onclick={() => remove(item)}>
              Remove
            </button>
          </div>
          {#if item.status === 'uploading'}
            <div class="mt-2 h-1 overflow-hidden rounded bg-ink-800">
              <div
                class="h-full bg-accent transition-[width]"
                style="width: {(item.sent / Math.max(1, item.total)) * 100}%"
              ></div>
            </div>
          {/if}
        </li>
      {/each}
    </ul>

    <div class="mt-6 grid gap-4 sm:grid-cols-2">
      <label class="block sm:col-span-2">
        <span class="text-xs text-ink-500">Title (optional)</span>
        <input
          bind:value={title}
          placeholder="Saturday night"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block">
        <span class="text-xs text-ink-500">Expires after</span>
        <select
          bind:value={expiresDays}
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        >
          <option value={1}>1 day</option>
          <option value={7}>7 days</option>
          <option value={30}>30 days</option>
          <option value={90}>90 days</option>
          <option value={365}>1 year</option>
        </select>
      </label>
      <label class="block">
        <span class="text-xs text-ink-500">Max downloads (optional)</span>
        <input
          bind:value={maxDownloads}
          inputmode="numeric"
          placeholder="unlimited"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block sm:col-span-2">
        <span class="text-xs text-ink-500">Password (optional)</span>
        <input
          type="password"
          bind:value={password}
          autocomplete="new-password"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
    </div>

    {#if error}
      <p class="mt-4 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
    {/if}

    <button
      disabled={busy || creating || ready.length === 0}
      onclick={createShare}
      class="mt-4 w-full rounded-lg bg-accent px-3 py-2 text-sm font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
    >
      {#if busy}
        Uploading…
      {:else if creating}
        Creating link…
      {:else}
        Create link for {ready.length} file{ready.length === 1 ? '' : 's'} ({bytes(totalBytes)})
      {/if}
    </button>
  {/if}

  {#if created}
    <div class="mt-6 rounded-xl border border-accent/40 bg-accent/5 p-4">
      <p class="text-xs text-ink-500">Link created — expires in {until(created.expires_at)}</p>
      <div class="mt-2 flex items-center gap-3">
        <input
          readonly
          value={created.url}
          class="flex-1 truncate rounded-lg border border-ink-700 bg-ink-950 px-3 py-2 text-sm text-accent"
        />
        <button
          class="shrink-0 rounded-lg border border-ink-700 px-3 py-2 text-sm hover:border-ink-500"
          onclick={() => copy(created!.url)}
        >
          {copied ? 'Copied' : 'Copy'}
        </button>
      </div>
    </div>
  {/if}

  {#if shares.length}
    <section class="mt-12">
      <h2 class="text-sm font-medium text-ink-300">Your links</h2>
      <ul class="mt-3 divide-y divide-ink-800 rounded-xl border border-ink-800">
        {#each shares as share (share.token)}
          <li class="px-4 py-3">
            <div class="flex items-center justify-between gap-3">
              <div class="min-w-0">
                <button
                  class="flex items-center gap-1.5 truncate text-sm hover:text-accent"
                  onclick={() => toggle(share.token)}
                >
                  <span class="text-ink-500">{expanded === share.token ? '▾' : '▸'}</span>
                  {share.title || 'Untitled'}
                </button>
                <p class="tnum mt-0.5 text-xs text-ink-500">
                  {share.file_count} file{share.file_count === 1 ? '' : 's'} · {bytes(share.total_bytes)} ·
                  expires in {until(share.expires_at)} · {share.download_count} download{share.download_count ===
                  1
                    ? ''
                    : 's'}
                  {#if share.password_protected}· password{/if}
                </p>
              </div>
              <div class="flex shrink-0 items-center gap-3 text-xs">
                <a href={`/d/${share.token}`} class="text-ink-500 hover:text-ink-300">Open</a>
                <button class="text-ink-500 hover:text-ink-300" onclick={() => copy(share.url)}>Copy</button>
                <button class="text-ink-500 hover:text-red-400" onclick={() => revoke(share.token)}>
                  Revoke
                </button>
              </div>
            </div>

            {#if expanded === share.token}
              {#if expanding}
                <p class="mt-3 text-xs text-ink-500">Loading…</p>
              {:else}
                <ul class="mt-3 space-y-1 border-l border-ink-800 pl-3">
                  {#each expandedFiles as file (file.id)}
                    <li class="flex items-center justify-between gap-3">
                      <span class="tnum min-w-0 truncate text-xs text-ink-300">
                        {file.filename}
                        <span class="text-ink-500">· {bytes(file.size)}</span>
                      </span>
                      <button
                        class="shrink-0 text-xs text-ink-500 hover:text-red-400"
                        title="Remove this file from the share"
                        onclick={() => removeFile(share.token, file.id)}
                      >
                        Remove
                      </button>
                    </li>
                  {/each}
                </ul>
                {#if expandedFiles.length === 1}
                  <p class="mt-2 text-xs text-ink-500">
                    Removing the last file revokes the link.
                  </p>
                {/if}
              {/if}
            {/if}
          </li>
        {/each}
      </ul>
    </section>
  {/if}
</div>

{#if confirmingInvite}
  <Confirm
    title="Create an invite code?"
    body="Creates a code that lets one person register an account. It is valid for 14 days and works for whoever holds it, so treat it like a password — anyone it reaches can join."
    confirmLabel="Create invite"
    onConfirm={makeInvite}
    onCancel={() => (confirmingInvite = false)}
  />
{/if}
