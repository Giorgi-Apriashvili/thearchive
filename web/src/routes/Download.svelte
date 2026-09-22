<script lang="ts">
  import { api, ApiError, type ShareDetail } from '../lib/api'
  import { bytes, shortDate, until } from '../lib/format'

  let { token }: { token: string } = $props()

  let share = $state<ShareDetail | null>(null)
  let needsPassword = $state(false)
  let password = $state('')
  let error = $state('')
  let loading = $state(true)

  // Held in memory only, and appended to download URLs. Putting it in the address bar
  // would leak it into history and into any Referer the browser sends.
  let unlocked = $state('')

  async function load(candidate = '') {
    loading = true
    error = ''
    try {
      const headers = candidate ? { 'X-Share-Password': candidate } : undefined
      share = await api.get<ShareDetail>(`/api/shares/${token}`, headers)
      unlocked = candidate
      needsPassword = false
    } catch (e) {
      share = null
      if (e instanceof ApiError && e.status === 401) {
        needsPassword = true
        if (candidate) error = 'That password did not work.'
      } else {
        error = e instanceof ApiError ? e.message : 'could not load this link'
      }
    } finally {
      loading = false
    }
  }
  load()

  function downloadUrl(fileId: number): string {
    const base = `/d/${token}/${fileId}`
    return unlocked ? `${base}?p=${encodeURIComponent(unlocked)}` : base
  }

  const archiveUrl = $derived(
    unlocked ? `/d/${token}/all.zip?p=${encodeURIComponent(unlocked)}` : `/d/${token}/all.zip`,
  )

  const totalBytes = $derived(share?.files.reduce((sum, f) => sum + f.size, 0) ?? 0)

  const remaining = $derived(
    share?.max_downloads ? share.max_downloads - share.download_count : null,
  )
</script>

<div class="mx-auto max-w-2xl px-4 py-16">
  <a href="/" class="wordmark text-xl text-ink-300 transition hover:text-ink-100">Weekend<em>Archive</em></a>

  {#if loading}
    <p class="mt-10 text-sm text-ink-500">Loading…</p>
  {:else if needsPassword}
    <h1 class="mt-8 text-xl font-semibold tracking-tight">This link is password protected</h1>
    <form
      class="mt-6 flex gap-3"
      onsubmit={(e) => {
        e.preventDefault()
        load(password)
      }}
    >
      <input
        type="password"
        bind:value={password}
        required
        autocomplete="off"
        class="flex-1 rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
      />
      <button class="rounded-lg bg-accent px-4 py-2 text-sm font-medium text-ink-950 hover:bg-accent-dim">
        Unlock
      </button>
    </form>
    {#if error}
      <p class="mt-3 text-sm text-red-300">{error}</p>
    {/if}
  {:else if share}
    <h1 class="mt-8 text-xl font-semibold tracking-tight">{share.title || 'Shared files'}</h1>
    <p class="tnum mt-1 text-sm text-ink-500">
      Shared {shortDate(share.created_at)} · expires in {until(share.expires_at)}
      {#if remaining !== null}
        · {remaining} download{remaining === 1 ? '' : 's'} left
      {/if}
    </p>

    {#if share.files.length > 1}
      <!-- A plain link, not a fetch: the archive is generated as it is sent and carries
           a real Content-Length, so the browser's own download UI shows accurate
           progress and an ETA. Intercepting it in JavaScript would replace that with
           something worse. -->
      <a
        href={archiveUrl}
        class="mt-8 flex items-center justify-between gap-4 rounded-xl border border-accent/40 bg-accent/5 px-4 py-3 transition hover:bg-accent/10"
      >
        <span class="text-sm font-medium text-accent">
          Download all {share.files.length} files
        </span>
        <span class="tnum shrink-0 text-xs text-ink-500">{bytes(totalBytes)} · .zip</span>
      </a>
    {/if}

    <ul class="mt-4 divide-y divide-ink-800 rounded-xl border border-ink-800">
      {#each share.files as file (file.id)}
        <li class="flex items-center justify-between gap-4 px-4 py-3">
          <div class="min-w-0">
            <p class="truncate text-sm">{file.filename}</p>
            <p class="tnum mt-0.5 text-xs text-ink-500">
              {bytes(file.size)}
              {#if file.uploaded_by}· from {file.uploaded_by}{/if}
              {#if file.client_mtime}· {shortDate(file.client_mtime)}{/if}
            </p>
          </div>
          <a
            href={downloadUrl(file.id)}
            download={file.filename}
            class="shrink-0 rounded-lg border border-ink-700 px-3 py-1.5 text-sm hover:border-accent hover:text-accent"
          >
            Download
          </a>
        </li>
      {/each}
    </ul>
  {:else}
    <h1 class="mt-8 text-xl font-semibold tracking-tight">Nothing here</h1>
    <p class="mt-2 text-sm text-ink-500">
      {error || 'This link has expired or never existed.'}
    </p>
  {/if}
</div>
