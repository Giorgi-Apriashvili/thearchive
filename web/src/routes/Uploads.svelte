<script lang="ts">
  import { untrack } from 'svelte'
  import * as tus from 'tus-js-client'
  import {
    api,
    ApiError,
    type CreatedShare,
    type ShareDetail,
    type ShareDownloads,
    type ShareFile,
    type ShareSummary,
    type StorageInfo,
  } from '../lib/api'
  import { bytes, chatTime, expiry, expiryChoices, until } from '../lib/format'
  import { inbox } from '../lib/inbox.svelte'
  import { uploading } from '../lib/uploading.svelte'
  import VisibilityToggle from '../lib/VisibilityToggle.svelte'
  import ExpiryEditor from '../lib/ExpiryEditor.svelte'
  import Confirm from '../lib/Confirm.svelte'
  import Avatar from '../lib/Avatar.svelte'
  import MemberName from '../lib/MemberName.svelte'
  import ShareCard from '../lib/ShareCard.svelte'

  // This pane stays mounted while Chat is showing, so that uploads in flight survive a
  // tab switch. `visible` is what stops links sent to you being marked seen while you are
  // looking at a chat room instead.
  // `canNeverExpire`: privileged members and admins may make links that never expire. The
  // server enforces it; this only decides whether to offer the choice.
  let { visible = true, canNeverExpire = false }: { visible?: boolean; canNeverExpire?: boolean } =
    $props()

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
    key: string
    name: string
    uploadId: string | null
    sent: number
    total: number
    status: 'uploading' | 'done' | 'error'
    error?: string
    handle?: tus.Upload
    /** Refused for want of space: retrying would only be refused again. */
    overLimit?: boolean
    /** Counted in `uploading` until it finishes, fails or is removed — exactly once. */
    inFlight?: boolean
    /** Set on a file uploaded earlier — in a tab since closed, or on another device — and
     *  offered again from the server. It is deleted at this time unless made into a link. */
    expiresAt?: number
  }

  interface PendingUpload {
    id: string
    filename: string
    size: number
    expires_at: number
  }

  /** An upload that stopped part-way — a closed tab, a crash, a dropped connection. The
   *  server keeps what arrived until `expires_at`; choosing the same file again continues
   *  from `offset`. `modified` is the file's last-modified time, in seconds. */
  interface UnfinishedUpload {
    id: string
    filename: string
    size: number
    offset: number
    modified?: number
    expires_at: number
  }

  let unfinished = $state<UnfinishedUpload[]>([])
  let discarding = $state<UnfinishedUpload | null>(null)

  // The same file, as far as can be told without reading it: name, size and modification
  // time all equal. A different file matching all three would be continued as if it were
  // this one, so the time is required whenever the server has one.
  function previousAttempt(file: File): UnfinishedUpload | undefined {
    return unfinished.find(
      (u) =>
        u.filename === file.name &&
        u.size === file.size &&
        (u.modified === undefined || u.modified === Math.floor(file.lastModified / 1000)),
    )
  }

  async function discard(upload: UnfinishedUpload) {
    discarding = null
    try {
      await api.del(`/files/${upload.id}`)
    } catch {
      // Already gone; nothing left to discard.
    }
    unfinished = unfinished.filter((u) => u.id !== upload.id)
    await loadShares()
  }

  let items = $state<Item[]>([])
  let shares = $state<ShareSummary[]>([])
  let dragging = $state(false)
  let creating = $state(false)
  let created = $state<CreatedShare | null>(null)
  // Where the finished uploads go: a new link (null), or one of yours that still works.
  let target = $state<string | null>(null)
  let added = $state<{ title: string; count: number } | null>(null)
  let fileInput = $state<HTMLInputElement | null>(null)
  let pickedForLink = false
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
  const earliestExpiry = $derived(
    Math.min(...items.flatMap((i) => (i.expiresAt ? [i.expiresAt] : []))),
  )

  // Finished uploads that never became a link live only on the server once their tab is
  // closed. Offer them again whenever this pane comes into view, so closing the tab before
  // pressing "Create link" costs nothing — for as long as they have not expired.
  async function loadPending() {
    try {
      const data = await api.get<{ uploads: PendingUpload[]; unfinished: UnfinishedUpload[] }>(
        '/api/uploads',
      )
      const { uploads } = data
      const known = new Set(items.map((i) => i.uploadId))
      // Ones already continuing in this tab are not "unfinished" from here.
      unfinished = data.unfinished.filter((u) => !known.has(u.id))
      for (const upload of uploads) {
        if (known.has(upload.id)) continue
        items.push({
          key: `upload:${upload.id}`,
          name: upload.filename,
          uploadId: upload.id,
          sent: upload.size,
          total: upload.size,
          status: 'done',
          expiresAt: upload.expires_at,
        })
      }
    } catch {
      // Nothing to offer, or offline; the next time the pane is shown tries again.
    }
  }
  $effect(() => {
    if (visible) void loadPending()
  })

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

  let nextKey = 0

  function enqueue(files: FileList | File[]) {
    for (const file of Array.from(files)) {
      // Push first, then read the element back. $state deep-proxies array contents, and
      // the object literal above is the raw target — mutating it from the callbacks
      // below would update the data without notifying the proxy's signals, leaving
      // progress stuck at 0% and the file never appearing as finished.
      const previous = previousAttempt(file)
      if (previous) unfinished = unfinished.filter((u) => u !== previous)
      items.push({
        key: `file:${nextKey++}`,
        name: file.name,
        uploadId: previous?.id ?? null,
        sent: previous?.offset ?? 0,
        total: file.size,
        status: 'uploading',
        inFlight: true,
      })
      const item = items[items.length - 1]
      uploading.start()

      const handle = new tus.Upload(file, {
        endpoint: '/files',
        // Continue an earlier attempt at this file from where the server got to. Should
        // that upload have expired meanwhile, the client starts a fresh one on its own.
        uploadUrl: previous ? `/files/${previous.id}` : null,
        // Resuming is matched against the server's list instead, which works from any
        // browser; tus's own localStorage record would only pile up.
        storeFingerprintForResuming: false,
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
          settle(item)
          void loadShares() // what you are using has changed
        },
        onError: (err) => {
          item.status = 'error'
          item.error = err.message
          // Over the storage limit: show the server's own sentence, which says how much
          // is in use, rather than tus's account of the HTTP exchange.
          const response = (err as tus.DetailedError).originalResponse
          if (response?.getStatus() === 413) {
            item.overLimit = true
            try {
              item.error = JSON.parse(response.getBody()).error ?? item.error
            } catch {
              item.error = 'not enough space left under your storage limit'
            }
          }
          settle(item)
        },
      })
      item.handle = handle
      handle.start()
    }
  }

  // The upload whose removal is awaiting confirmation. A failed one goes without asking:
  // nothing of it is stored, so there is nothing to lose.
  let removingUpload = $state<Item | null>(null)

  /** Tries a failed upload again. tus asks the server how much arrived and continues
   *  from there, so nothing already sent is sent twice. */
  function retry(item: Item) {
    if (item.status !== 'error' || !item.handle) return
    item.status = 'uploading'
    item.error = undefined
    item.inFlight = true
    uploading.start()
    item.handle.start()
  }

  // A dropped connection fails uploads once tus's own retries run out; when the browser
  // says it is back online, carry on without waiting for a click.
  $effect(() => {
    const onOnline = () => items.filter((i) => i.status === 'error').forEach(retry)
    window.addEventListener('online', onOnline)
    return () => window.removeEventListener('online', onOnline)
  })

  function askRemove(item: Item) {
    if (item.status === 'error') void remove(item)
    else removingUpload = item
  }

  function settle(item: Item) {
    if (!item.inFlight) return
    item.inFlight = false
    uploading.end()
  }

  async function remove(item: Item) {
    removingUpload = null
    item.handle?.abort()
    settle(item)
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

  // Links that can still take files: not expired, not used up. The server checks again.
  const liveShares = $derived(
    shares.filter(
      (s) =>
        s.expires_at > Date.now() / 1000 &&
        !(s.max_downloads !== undefined && s.download_count >= s.max_downloads),
    ),
  )
  const targetShare = $derived(target ? (liveShares.find((s) => s.token === target) ?? null) : null)

  /** "Add files" on one of your links: that link becomes the destination, then the usual
   *  file chooser — so the upload itself is the ordinary one, progress and all. */
  function addFilesTo(token: string) {
    target = token
    pickedForLink = true
    fileInput?.click()
  }

  async function addToShare(share: ShareSummary) {
    error = ''
    creating = true
    try {
      await api.post(`/api/shares/${share.token}/files`, { uploads: ready.map((i) => i.uploadId) })
      added = { title: share.title || 'Untitled', count: ready.length }
      created = null
      items = []
      target = null
      await loadShares()
      if (expanded === share.token) await loadExpanded(share.token)
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not add the files'
    } finally {
      creating = false
    }
  }

  function submit() {
    if (targetShare) void addToShare(targetShare)
    else void createShare()
  }

  async function createShare() {
    error = ''
    creating = true
    try {
      // 0 in the menu is "Never".
      const body: Record<string, unknown> = {
        uploads: ready.map((i) => i.uploadId),
        ...(expiresDays === 0 ? { never_expires: true } : { expires_days: expiresDays }),
      }
      if (title.trim()) body.title = title.trim()
      if (password) body.password = password
      if (maxDownloads) body.max_downloads = Number(maxDownloads)
      if (!isPrivate) body.public = true

      created = await api.post<CreatedShare>('/api/shares', body)
      added = null
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

  // The link whose removal is awaiting confirmation. One click would otherwise end a link
  // that might hold a gigabyte of someone's weekend, with no way back.
  let removing = $state<ShareSummary | null>(null)

  async function revoke(token: string) {
    removing = null
    try {
      await api.del(`/api/shares/${token}`)
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not remove that link'
    }
    if (expanded === token) expanded = null
    await loadShares()
  }

  let expanded = $state<string | null>(null)
  let expandedFiles = $state<ShareFile[]>([])
  let expandedDownloads = $state<ShareDownloads | null>(null)
  let expanding = $state(false)

  async function toggle(token: string) {
    if (expanded === token) {
      expanded = null
      return
    }
    expanded = token
    expandedFiles = []
    expandedDownloads = null
    expanding = true
    try {
      await loadExpanded(token)
    } catch {
      expanded = null
    } finally {
      expanding = false
    }
  }

  async function loadExpanded(token: string) {
    // The owner is exempt from the share password, so this works even on a protected
    // link without prompting for something they chose themselves.
    const [detail, downloads] = await Promise.all([
      api.get<ShareDetail>(`/api/shares/${token}`),
      api.get<ShareDownloads>(`/api/shares/${token}/downloads`),
    ])
    expandedFiles = detail.files
    expandedDownloads = downloads
  }

  function took(files: (string | null)[]): string {
    const names = files.map((f) => f ?? 'everything, as a zip')
    return names.length > 3 ? `${names.slice(0, 3).join(', ')} and ${names.length - 3} more` : names.join(', ')
  }

  // The file whose removal is awaiting confirmation, and the link it is in.
  let removingFile = $state<{ share: ShareSummary; file: ShareFile; last: boolean } | null>(null)

  async function removeFile(token: string, fileId: number) {
    removingFile = null
    try {
      const result = await api.del<{ files_remaining: number; share_revoked: boolean }>(
        `/api/shares/${token}/files/${fileId}`,
      )
      expandedFiles = expandedFiles.filter((f) => f.id !== fileId)
      // Removing the last file revokes the share, so there is nothing left to show.
      if (result.share_revoked) expanded = null
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not remove that file'
    }
    await loadShares()
  }
</script>

<!-- One column on phones and small windows; from lg, what you are sending on the left and
     what you have sent and been sent on the right, so neither scrolls the other away. -->
<div class="lg:grid lg:grid-cols-2 lg:items-start lg:gap-10">
  <div class="min-w-0">
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

        {#if storage?.my_usage !== undefined}
          <div class="mt-3 border-t border-ink-800 pt-3">
            <div class="flex items-baseline justify-between text-xs">
              <span class="text-ink-300">Your space</span>
              <span class="tnum text-ink-500">
                {#if storage.my_limit}
                  {bytes(storage.my_usage)} of {bytes(storage.my_limit)}
                {:else}
                  {bytes(storage.my_usage)} · no limit
                {/if}
              </span>
            </div>
            {#if storage.my_limit}
              <div class="mt-2 h-1.5 overflow-hidden rounded bg-ink-800">
                <div
                  class="h-full {storage.my_usage / storage.my_limit > 0.9 ? 'bg-red-400' : 'bg-accent'}"
                  style="width: {Math.min(100, (storage.my_usage / storage.my_limit) * 100)}%"
                ></div>
              </div>
              <p class="mt-2 text-xs text-ink-500">
                Each file counts once, however many of your links hold it. Links that expire or
                are removed give their space back.
              </p>
            {/if}
          </div>
        {/if}
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
          bind:this={fileInput}
          type="file"
          multiple
          class="hidden"
          onchange={(e) => {
            const input = e.currentTarget as HTMLInputElement
            if (input.files) enqueue(input.files)
            input.value = ''
            // Chosen from a link further down: bring the upload list into view.
            if (pickedForLink) document.getElementById('upload-list')?.scrollIntoView({ behavior: 'smooth' })
            pickedForLink = false
          }}
        />
      </label>
    </div>

    {#if unfinished.length}
      <section class="mt-6 rounded-lg border border-ink-700 bg-ink-900 px-3 py-2.5 text-sm">
        <p class="text-ink-300">
          {unfinished.length === 1 ? 'An upload' : 'Some uploads'} stopped part-way. Choose the same
          file again to continue from where it stopped.
        </p>
        <ul class="mt-2 space-y-1.5">
          {#each unfinished as upload (upload.id)}
            <li class="flex items-center justify-between gap-3 text-xs">
              <span class="tnum min-w-0 truncate text-ink-300">
                {upload.filename}
                <span class="text-ink-500">
                  · {Math.floor((upload.offset / Math.max(1, upload.size)) * 100)}% of {bytes(upload.size)} ·
                  kept for {until(upload.expires_at)}
                </span>
              </span>
              <button class="shrink-0 text-ink-500 hover:text-red-400" onclick={() => (discarding = upload)}>
                Discard
              </button>
            </li>
          {/each}
        </ul>
      </section>
    {/if}

    {#if items.length}
      {#if Number.isFinite(earliestExpiry)}
        <p class="mt-6 rounded-lg border border-accent/40 bg-accent/5 px-3 py-2 text-sm text-ink-300">
          You uploaded these earlier but never made a link. Create one within
          {until(earliestExpiry)}, or they are deleted.
        </p>
      {/if}
      <ul id="upload-list" class="mt-6 space-y-2">
        {#each items as item (item.key)}
          <li class="rounded-lg border border-ink-700 bg-ink-900 px-3 py-2">
            <div class="flex items-center justify-between gap-3 text-sm">
              <span class="truncate">{item.name}</span>
              <span class="tnum shrink-0 text-xs text-ink-500">
                {#if item.status === 'error'}
                  <span class="text-red-400">{item.error}</span>
                {:else if item.status === 'done'}
                  {bytes(item.total)}
                {:else}
                  {Math.floor((item.sent / Math.max(1, item.total)) * 100)}%
                {/if}
              </span>
              {#if item.status === 'error' && item.handle && !item.overLimit}
                <button class="shrink-0 text-xs text-accent hover:text-accent-dim" onclick={() => retry(item)}>
                  Retry
                </button>
              {/if}
              <button class="shrink-0 text-xs text-ink-500 hover:text-ink-300" onclick={() => askRemove(item)}>
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

      {#if liveShares.length}
        <label class="mt-6 block">
          <span class="text-xs text-ink-500">Put these files in</span>
          <select
            bind:value={target}
            class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
          >
            <option value={null}>A new link</option>
            {#each liveShares as share (share.token)}
              <option value={share.token}>
                “{share.title || 'Untitled'}” · {share.file_count} file{share.file_count === 1 ? '' : 's'}
              </option>
            {/each}
          </select>
        </label>
      {/if}

      {#if targetShare}
        <p class="mt-3 text-xs text-ink-500">
          They join the link as it is: its address, expiry, visibility and password stay the same,
          and everyone who has the link sees the new files.
        </p>
      {:else}
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
              {#each expiryChoices as choice (choice.days)}
                <option value={choice.days}>{choice.label}</option>
              {/each}
              {#if canNeverExpire}
                <option value={0}>Never</option>
              {/if}
            </select>
            {#if expiresDays === 0}
              <span class="mt-1 block text-xs text-ink-500">
                Its files stay until you remove the link, and count against your space until then.
              </span>
            {/if}
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
      {/if}

      {#if error}
        <p class="mt-4 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
      {/if}

      <button
        disabled={busy || creating || ready.length === 0}
        onclick={submit}
        class="mt-4 w-full rounded-lg bg-accent px-3 py-2 text-sm font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
      >
        {#if busy}
          Uploading…
        {:else if creating}
          {targetShare ? 'Adding…' : 'Creating link…'}
        {:else if targetShare}
          Add {ready.length} file{ready.length === 1 ? '' : 's'} to “{targetShare.title || 'Untitled'}” ({bytes(totalBytes)})
        {:else}
          Create link for {ready.length} file{ready.length === 1 ? '' : 's'} ({bytes(totalBytes)})
        {/if}
      </button>
    {/if}

    {#if added}
      <p class="mt-6 rounded-xl border border-accent/40 bg-accent/5 px-4 py-3 text-sm text-ink-300">
        Added {added.count} file{added.count === 1 ? '' : 's'} to “{added.title}”. Everyone who has
        the link sees {added.count === 1 ? 'it' : 'them'} now.
      </p>
    {/if}

    {#if created}
      <div class="mt-6 rounded-xl border border-accent/40 bg-accent/5 p-4">
        <p class="text-xs text-ink-500">
          Link created — {expiry(created.expires_at)} ·
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
  </div>
  <div class="min-w-0 lg:[&>section:first-child]:mt-6">
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
              <div class="mt-2.5 flex flex-col gap-2">
                {#each item.cards as card, i (i)}
                  <ShareCard {card} />
                {/each}
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
              <!-- Actions under the details on a phone, beside them from sm. -->
              <div class="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between sm:gap-3">
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
                    {expiry(share.expires_at)} · {share.download_count} download{share.download_count ===
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
                  <button class="text-ink-500 hover:text-red-400" onclick={() => (removing = share)}>
                    Remove
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
                          onclick={() =>
                            (removingFile = { share, file, last: expandedFiles.length === 1 })}
                        >
                          Remove
                        </button>
                      </li>
                    {/each}
                  </ul>
                  {#if expandedFiles.length === 1}
                    <p class="mt-2 text-xs text-ink-500">
                      Removing the last file removes the link.
                    </p>
                  {/if}
                  {#if expandedDownloads}
                    <div class="mt-4 border-t border-ink-800 pt-3 text-xs">
                      <p class="text-ink-500">Downloaded by</p>
                      {#if expandedDownloads.people.length === 0 && expandedDownloads.anonymous.downloads === 0}
                        <p class="mt-1.5 text-ink-500">No one but you yet.</p>
                      {/if}
                      <ul class="mt-1.5 space-y-2">
                        {#each expandedDownloads.people as person (person.username)}
                          <li class="flex items-start gap-2">
                            <Avatar src={person.avatar} name={person.username} size="xs" />
                            <div class="min-w-0">
                              <p class="flex flex-wrap items-baseline gap-x-1.5">
                                <MemberName
                                  username={person.username}
                                  displayName={person.display_name}
                                  role={person.role}
                                />
                                <span class="tnum text-ink-500">
                                  · {person.downloads} download{person.downloads === 1 ? '' : 's'} · {chatTime(person.last_at)}
                                </span>
                              </p>
                              <p class="truncate text-ink-500">{took(person.files)}</p>
                            </div>
                          </li>
                        {/each}
                        {#if expandedDownloads.anonymous.downloads > 0}
                          <li class="tnum text-ink-500">
                            {expandedDownloads.anonymous.downloads} download{expandedDownloads.anonymous.downloads === 1
                              ? ''
                              : 's'} by people who were not signed in{#if expandedDownloads.anonymous.last_at}{' '}·
                              last {chatTime(expandedDownloads.anonymous.last_at)}{/if}
                          </li>
                        {/if}
                      </ul>
                    </div>
                  {/if}
                  {#if liveShares.some((s) => s.token === share.token)}
                    <button
                      class="mt-3 rounded-lg border border-ink-700 px-2.5 py-1 text-xs hover:border-ink-500"
                      onclick={() => addFilesTo(share.token)}
                    >Add files</button>
                  {/if}
                  <ExpiryEditor
                    token={share.token}
                    expiresAt={share.expires_at}
                    canNever={canNeverExpire}
                    onChanged={(next) => (share.expires_at = next)}
                  />
                {/if}
              {/if}
            </li>
          {/each}
        </ul>
      </section>
    {/if}
  </div>
</div>

{#if removing}
  <Confirm
    title="Remove “{removing.title || 'Untitled'}”?"
    body={`The link stops working at once, for everyone you sent it to, and its ${removing.file_count} file${removing.file_count === 1 ? '' : 's'} (${bytes(removing.total_bytes)}) are deleted within about 15 minutes — unless one of your other links still holds them. This cannot be undone.`}
    confirmLabel="Remove link"
    danger
    onConfirm={() => revoke(removing!.token)}
    onCancel={() => (removing = null)}
  />
{/if}

{#if removingFile}
  {@const { share, file, last } = removingFile}
  <Confirm
    title={last ? `Remove the last file, and the link with it?` : `Remove “${file.filename}”?`}
    body={last
      ? `“${file.filename}” is the only file in “${share.title || 'Untitled'}”, so removing it removes the link too: it stops working for everyone you sent it to. The file (${bytes(file.size)}) is deleted within about 15 minutes, unless one of your other links still holds it. This cannot be undone.`
      : `It disappears from “${share.title || 'Untitled'}” for everyone with the link; the rest of the link keeps working. The file (${bytes(file.size)}) is deleted within about 15 minutes, unless one of your other links still holds it. This cannot be undone.`}
    confirmLabel={last ? 'Remove file and link' : 'Remove file'}
    danger
    onConfirm={() => removeFile(share.token, file.id)}
    onCancel={() => (removingFile = null)}
  />
{/if}

{#if removingUpload}
  {@const item = removingUpload}
  <Confirm
    title={item.status === 'uploading' ? `Stop uploading “${item.name}”?` : `Remove “${item.name}”?`}
    body={item.status === 'uploading'
      ? 'The upload stops and what has arrived so far is discarded. To include this file later you would have to upload it again from the start.'
      : `It is deleted from the server now (${bytes(item.total)}) and will not be in the link you create. To include it later you would have to upload it again.`}
    confirmLabel={item.status === 'uploading' ? 'Stop and remove' : 'Remove file'}
    danger
    onConfirm={() => remove(item)}
    onCancel={() => (removingUpload = null)}
  />
{/if}

{#if discarding}
  {@const upload = discarding}
  <Confirm
    title={`Discard “${upload.filename}”?`}
    body={`What has arrived so far (${bytes(upload.offset)} of ${bytes(upload.size)}) is deleted. To send this file you would start again from the beginning.`}
    confirmLabel="Discard"
    danger
    onConfirm={() => discard(upload)}
    onCancel={() => (discarding = null)}
  />
{/if}
