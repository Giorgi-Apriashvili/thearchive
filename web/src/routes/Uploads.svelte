<script lang="ts">
  import { untrack } from 'svelte'
  import * as tus from 'tus-js-client'
  import {
    api,
    ApiError,
    type CreatedShare,
    type ShareDetail,
    type ShareFile,
    type ShareSummary,
    type StorageInfo,
  } from '../lib/api'
  import { bytes, chatTime, until } from '../lib/format'
  import { inbox } from '../lib/inbox.svelte'
  import VisibilityToggle from '../lib/VisibilityToggle.svelte'
  import Avatar from '../lib/Avatar.svelte'
  import MemberName from '../lib/MemberName.svelte'
  import ShareCard from '../lib/ShareCard.svelte'

  // This pane stays mounted while Chat is showing, so that uploads in flight survive a
  // tab switch. `visible` is what stops links sent to you being marked seen while you are
  // looking at a chat room instead.
  let { visible = true }: { visible?: boolean } = $props()

  // Load the inbox when this pane is on screen, and again whenever something new arrives
  // while it is — then mark it seen, which is what clears the badge.
  // Reacts to visibility and the unread count only: reading `items` as tracked state would
  // re-run this when its own load lands, loading everything twice.
  $effect(() => {
    if (!visible) return
    const unread = inbox.unread
    if (untrack(() => inbox.items === null) || unread > 0) {
      void inbox.load().then(() => inbox.markSeen())
    }
  })

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

  let title = $state('')
  let expiresDays = $state(30)
  let password = $state('')
  let maxDownloads = $state('')
  // Private by default: the safe option should be the one you get by not thinking
  // about it.
  let isPrivate = $state(true)

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
      if (!isPrivate) body.public = true

      created = await api.post<CreatedShare>('/api/shares', body)
      items = []
      title = ''
      password = ''
      maxDownloads = ''
      isPrivate = true
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
</script>

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
    <label class="flex cursor-pointer items-start gap-3 rounded-lg border border-ink-700 bg-ink-900 px-3 py-2.5 sm:col-span-2">
      <input type="checkbox" bind:checked={isPrivate} class="mt-0.5 accent-[#e0a458]" />
      <span class="text-xs">
        <span class="block text-ink-100">Private — members only</span>
        <span class="mt-0.5 block text-ink-500">
          {isPrivate
            ? 'Only people with an account here can open the link.'
            : 'Anyone with the link can open it, account or not.'}
        </span>
      </span>
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
    <p class="text-xs text-ink-500">
      Link created — expires in {until(created.expires_at)} ·
      {created.visibility === 'public' ? 'anyone with the link' : 'members only'}
    </p>
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

{#if inbox.items?.length}
  <section class="mt-12">
    <h2 class="text-sm font-medium text-ink-300">Shared with you</h2>
    {#if inbox.error}
      <p class="mt-2 text-xs text-red-400">{inbox.error}</p>
    {/if}
    <ul class="mt-3 space-y-3">
      {#each inbox.items as item (item.id)}
        <li
          class="rounded-xl border px-4 py-3 {item.seen ? 'border-ink-800' : 'border-accent/40 bg-accent/5'}"
        >
          <div class="flex items-center justify-between gap-3">
            <p class="flex min-w-0 items-center gap-2 text-xs text-ink-500">
              <Avatar src={item.sender.avatar} name={item.sender.username} size="xs" />
              <MemberName
                username={item.sender.username}
                displayName={item.sender.display_name}
                role={item.sender.role}
              />
              <span class="tnum shrink-0">· {chatTime(item.sent_at)}</span>
              {#if !item.seen}<span class="shrink-0 text-[10px] uppercase text-accent">new</span>{/if}
            </p>
            <button
              onclick={() => inbox.dismiss(item.id)}
              title="Remove from this list"
              aria-label="Dismiss"
              class="shrink-0 text-xs text-ink-500 hover:text-ink-300"
            >Dismiss</button>
          </div>
          {#if item.note}
            <!-- Plain text: another member wrote it. -->
            <p class="mt-2 whitespace-pre-line break-words text-sm text-ink-300">{item.note}</p>
          {/if}
          <div class="mt-2.5">
            <ShareCard card={item.card} />
          </div>
        </li>
      {/each}
    </ul>
  </section>
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
              <VisibilityToggle
                visibility={share.visibility}
                endpoint={`/api/shares/${share.token}`}
                onChanged={(next) => (share.visibility = next)}
              />
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
