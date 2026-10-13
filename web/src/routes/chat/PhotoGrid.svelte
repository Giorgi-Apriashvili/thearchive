<script lang="ts">
  // Photos posted straight into chat, shown as photos: one large, or a grid of up to nine
  // with "+N" on the last. Clicking one opens the viewer at it (onOpen).
  import type { ShareCard } from '../../lib/api'

  let { card, onOpen }: { card: ShareCard; onOpen: (photoId: number) => void } = $props()

  const MAX_SHOWN = 9
  const photos = $derived(card.photos ?? [])
  const shown = $derived(photos.slice(0, MAX_SHOWN))
  const hidden = $derived(photos.length - shown.length)
</script>

{#if card.state !== 'live' || !photos.length}
  <!-- Their link was removed — by its owner, or with the message — so there is nothing to show. -->
  <p class="mt-1 text-xs italic text-ink-700">These photos have been removed.</p>
{:else if photos.length === 1}
  {@const photo = photos[0]}
  <button
    onclick={() => onOpen(photo.id)}
    class="mt-1.5 block w-fit max-w-full overflow-hidden rounded-xl border border-ink-800 bg-ink-900 transition hover:border-accent/60"
    aria-label="Open photo"
  >
    {#if photo.thumb}
      <img src={photo.thumb} alt="" loading="lazy" class="block max-h-80 max-w-full object-contain sm:max-w-md" />
    {:else}
      <span class="flex h-40 w-64 items-center justify-center text-2xl text-ink-300">▶</span>
    {/if}
  </button>
{:else}
  <div class="mt-1.5 grid max-w-md gap-1 {photos.length === 2 || photos.length === 4 ? 'grid-cols-2' : 'grid-cols-3'}">
    {#each shown as photo, i (photo.id)}
      <button
        onclick={() => onOpen(photo.id)}
        class="relative aspect-square overflow-hidden rounded-lg border border-ink-800 bg-ink-900 transition hover:border-accent/60"
        aria-label="Open photo {i + 1} of {photos.length}"
      >
        {#if photo.thumb}
          <img src="{photo.thumb}?s=sm" alt="" loading="lazy" class="size-full object-cover" />
        {:else}
          <span class="flex size-full items-center justify-center text-xl text-ink-300">▶</span>
        {/if}
        {#if hidden > 0 && i === shown.length - 1}
          <span class="absolute inset-0 flex items-center justify-center bg-ink-950/70 text-lg font-medium text-ink-100">
            +{hidden}
          </span>
        {/if}
      </button>
    {/each}
  </div>
{/if}
