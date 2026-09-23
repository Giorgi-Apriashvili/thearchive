<script lang="ts">
  // Choose among your own links: several for a chat message, one (with an optional note)
  // for sending to a person. Only links that still work are offered — the server refuses
  // the rest anyway, and offering them would only produce that refusal.
  import { fade, scale } from 'svelte/transition'
  import { api, type ShareSummary } from './api'
  import { bytes, until } from './format'
  import LockIcon from './LockIcon.svelte'

  let {
    title,
    confirmLabel,
    multiple = false,
    withNote = false,
    busy = false,
    error = '',
    onPick,
    onCancel,
  }: {
    title: string
    confirmLabel: string
    multiple?: boolean
    withNote?: boolean
    busy?: boolean
    error?: string
    onPick: (links: ShareSummary[], note: string) => void
    onCancel: () => void
  } = $props()

  const MAX = 10
  const MAX_NOTE = 300

  let links = $state<ShareSummary[] | null>(null)
  let loadError = $state('')
  let chosen = $state<string[]>([])
  let note = $state('')

  api
    .get<ShareSummary[]>('/api/shares')
    .then((all) => {
      const now = Date.now() / 1000
      links = all.filter(
        (s) =>
          s.expires_at > now &&
          s.file_count > 0 &&
          (s.max_downloads == null || s.download_count < s.max_downloads),
      )
    })
    .catch(() => (loadError = 'your links could not be loaded'))

  function toggle(token: string) {
    if (!multiple) {
      chosen = [token]
    } else if (chosen.includes(token)) {
      chosen = chosen.filter((t) => t !== token)
    } else if (chosen.length < MAX) {
      chosen = [...chosen, token]
    }
  }

  let panel = $state<HTMLElement | null>(null)
  $effect(() => panel?.focus())
  $effect(() => {
    const previous = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    return () => {
      document.body.style.overflow = previous
    }
  })
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && onCancel()} />

<div class="fixed inset-0 z-50 flex items-center justify-center p-4" transition:fade={{ duration: 120 }}>
  <button onclick={onCancel} aria-label="Cancel" tabindex="-1" class="absolute inset-0 cursor-default bg-ink-950/80 backdrop-blur-sm"
  ></button>

  <div
    bind:this={panel}
    role="dialog"
    aria-modal="true"
    aria-label={title}
    tabindex="-1"
    class="relative z-10 flex max-h-[85vh] w-full max-w-md flex-col rounded-xl border border-ink-700 bg-ink-900 p-5 outline-none"
    transition:scale={{ duration: 120, start: 0.97 }}
  >
    <h2 class="text-sm font-medium text-ink-100">{title}</h2>
    <p class="mt-1 text-xs text-ink-500">
      A link keeps its own rules: a members-only one opens for members, and one with a
      password still needs it.
    </p>

    <div class="mt-4 min-h-0 flex-1 overflow-y-auto">
      {#if loadError}
        <p class="text-sm text-red-300">{loadError}</p>
      {:else if !links}
        <p class="text-sm text-ink-500">Loading…</p>
      {:else if links.length === 0}
        <p class="text-sm text-ink-500">
          You have no working links. Upload something on the Uploads tab first.
        </p>
      {:else}
        <ul class="space-y-1.5">
          {#each links as link (link.token)}
            {@const picked = chosen.includes(link.token)}
            <li>
              <button
                type="button"
                onclick={() => toggle(link.token)}
                aria-pressed={picked}
                class="flex w-full items-start gap-3 rounded-lg border px-3 py-2 text-left transition {picked
                  ? 'border-accent bg-accent/5'
                  : 'border-ink-800 hover:border-ink-700'}"
              >
                <span
                  class="mt-0.5 flex h-4 w-4 shrink-0 items-center justify-center border text-[10px] {multiple
                    ? 'rounded'
                    : 'rounded-full'} {picked ? 'border-accent bg-accent text-ink-950' : 'border-ink-700'}"
                >{picked ? '✓' : ''}</span>
                <span class="min-w-0">
                  <span class="flex items-center gap-1.5 truncate text-sm text-ink-100">
                    {#if link.password_protected}<span class="text-ink-500" title="Password protected"><LockIcon /></span>{/if}
                    <span class="truncate">{link.title || 'Untitled'}</span>
                  </span>
                  <span class="tnum block text-xs text-ink-500">
                    {link.file_count} file{link.file_count === 1 ? '' : 's'} · {bytes(link.total_bytes)}
                    · expires in {until(link.expires_at)}
                    · {link.visibility === 'public' ? 'anyone with the link' : 'members only'}
                  </span>
                </span>
              </button>
            </li>
          {/each}
        </ul>
      {/if}
    </div>

    {#if withNote && links?.length}
      <label class="mt-4 block">
        <span class="flex justify-between text-xs text-ink-500">
          <span>Note (optional)</span>
          <span class="tnum">{[...note.trim()].length}/{MAX_NOTE}</span>
        </span>
        <textarea
          bind:value={note}
          rows="2"
          class="mt-1 w-full resize-none rounded-lg border border-ink-700 bg-ink-950 px-3 py-2 text-sm outline-none focus:border-accent"
        ></textarea>
      </label>
    {/if}

    {#if error}
      <p class="mt-3 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
    {/if}

    <div class="mt-4 flex items-center justify-between gap-3">
      <span class="text-xs text-ink-500">
        {#if multiple && chosen.length}{chosen.length} chosen{#if chosen.length === MAX} (the most a message carries){/if}{/if}
      </span>
      <div class="flex gap-2">
        <button onclick={onCancel} class="rounded-lg border border-ink-700 px-3 py-1.5 text-xs text-ink-300 hover:border-ink-500">
          Cancel
        </button>
        <button
          disabled={busy || chosen.length === 0 || [...note.trim()].length > MAX_NOTE}
          onclick={() => onPick((links ?? []).filter((l) => chosen.includes(l.token)), note)}
          class="rounded-lg bg-accent px-3 py-1.5 text-xs font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
        >{busy ? 'Sending…' : confirmLabel}</button>
      </div>
    </div>
  </div>
</div>
