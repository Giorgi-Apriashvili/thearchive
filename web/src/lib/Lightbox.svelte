<script lang="ts">
  import { fade, scale } from 'svelte/transition'
  import { bytes, shortDate } from './format'
  import type { ShareFile } from './api'

  let {
    files,
    index = $bindable(0),
    thumbUrl,
    inlineUrl,
    downloadUrl,
    onClose,
  }: {
    files: ShareFile[]
    index: number
    thumbUrl: (id: number, size: 'sm' | 'lg') => string
    inlineUrl: (id: number) => string
    downloadUrl: (id: number) => string
    onClose: () => void
  } = $props()

  const current = $derived(files[index])
  let loaded = $state(false)

  function step(delta: number) {
    const next = index + delta
    if (next < 0 || next >= files.length) return
    index = next
    loaded = false
  }

  // Fetching the neighbours means arrowing through a set feels instant rather than
  // showing a blank frame on every press.
  $effect(() => {
    for (const offset of [1, -1]) {
      const neighbour = files[index + offset]
      if (neighbour?.preview === 'image') {
        new Image().src = thumbUrl(neighbour.id, 'lg')
      }
    }
  })

  // The page behind the overlay must not scroll while it is open, and the original
  // scroll position has to survive closing it.
  $effect(() => {
    const previous = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    return () => {
      document.body.style.overflow = previous
    }
  })

  function onKey(event: KeyboardEvent) {
    if (event.key === 'Escape') onClose()
    else if (event.key === 'ArrowRight') step(1)
    else if (event.key === 'ArrowLeft') step(-1)
  }

  // Touch handling is horizontal-only, and ignores mostly-vertical drags so a swipe
  // meant for the page does not flip the image.
  let touchX = 0
  let touchY = 0
  function onTouchStart(event: TouchEvent) {
    touchX = event.changedTouches[0].clientX
    touchY = event.changedTouches[0].clientY
  }
  function onTouchEnd(event: TouchEvent) {
    const dx = event.changedTouches[0].clientX - touchX
    const dy = event.changedTouches[0].clientY - touchY
    if (Math.abs(dx) > 60 && Math.abs(dx) > Math.abs(dy)) step(dx < 0 ? 1 : -1)
  }
</script>

<svelte:window onkeydown={onKey} />

<div
  role="dialog"
  aria-modal="true"
  tabindex="-1"
  aria-label={current?.filename}
  class="fixed inset-0 z-50 flex flex-col bg-ink-950/95 backdrop-blur-xl"
  transition:fade={{ duration: 150 }}
  ontouchstart={onTouchStart}
  ontouchend={onTouchEnd}
>
  <header class="flex items-start justify-between gap-4 px-4 py-3 sm:px-6">
    <div class="min-w-0">
      <p class="truncate text-sm text-ink-100">{current.filename}</p>
      <p class="tnum mt-0.5 text-xs text-ink-500">
        {index + 1} / {files.length} · {bytes(current.size)}
        {#if current.uploaded_by}· from {current.uploaded_by}{/if}
        {#if current.client_mtime}· {shortDate(current.client_mtime)}{/if}
      </p>
    </div>
    <div class="flex shrink-0 items-center gap-3">
      <a
        href={downloadUrl(current.id)}
        download={current.filename}
        class="rounded-lg border border-ink-700 px-3 py-1.5 text-xs hover:border-accent hover:text-accent"
      >
        Download
      </a>
      <button
        onclick={onClose}
        aria-label="Close"
        class="rounded-lg px-2 py-1 text-xl leading-none text-ink-500 hover:text-ink-100"
      >
        ×
      </button>
    </div>
  </header>

  <div class="relative flex min-h-0 flex-1 items-center justify-center p-4">
    <!-- The backdrop is a real button sitting behind the media rather than a click
         handler on the container. It is reachable by keyboard, and because the media is
         a sibling rather than a child, clicking the image cannot close the viewer -
         no stopPropagation needed anywhere. -->
    <button
      onclick={onClose}
      aria-label="Close preview"
      class="absolute inset-0 z-0 cursor-default"
      tabindex="-1"
    ></button>
    {#if index > 0}
      <button
        onclick={() => step(-1)}
        aria-label="Previous"
        class="absolute left-2 z-10 rounded-full bg-ink-900/70 px-3 py-4 text-ink-300 hover:text-ink-100 sm:left-6"
      >‹</button>
    {/if}

    {#key current.id}
      {#if current.preview === 'video'}
        <!-- svelte-ignore a11y_media_has_caption -->
        <video
          src={inlineUrl(current.id)}
          controls
          preload="metadata"
          class="relative z-10 max-h-full max-w-full rounded-lg"
          transition:scale={{ duration: 150, start: 0.98 }}
        ></video>
      {:else}
        <img
          src={thumbUrl(current.id, 'lg')}
          alt={current.filename}
          onload={() => (loaded = true)}
          class="relative z-10 max-h-full max-w-full rounded-lg object-contain transition-opacity duration-200"
          class:opacity-0={!loaded}
          transition:scale={{ duration: 150, start: 0.98 }}
        />
      {/if}
    {/key}

    {#if index < files.length - 1}
      <button
        onclick={() => step(1)}
        aria-label="Next"
        class="absolute right-2 z-10 rounded-full bg-ink-900/70 px-3 py-4 text-ink-300 hover:text-ink-100 sm:right-6"
      >›</button>
    {/if}
  </div>
</div>
