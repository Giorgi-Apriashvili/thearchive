<script lang="ts">
  // A link as it appears in a chat message or the inbox. The server resolved `card` when
  // it was fetched, so a link that has since expired says so here instead of being a click
  // that fails.
  import type { ShareCard } from './api'
  import { bytes, until } from './format'
  import LockIcon from './LockIcon.svelte'

  let { card }: { card: ShareCard } = $props()

  const why: Record<string, string> = {
    expired: 'This link has expired',
    revoked: 'This link was revoked',
    used_up: 'This link has reached its download limit',
    gone: 'This link no longer exists',
  }
</script>

{#if card.state === 'live' && card.token}
  <a
    href="/d/{card.token}"
    class="block max-w-sm rounded-xl border border-ink-700 bg-ink-900 p-3 transition hover:border-accent/60"
  >
    {#if card.previews?.length}
      <div class="mb-2.5 flex gap-1.5">
        {#each card.previews as preview (preview)}
          <img src={preview} alt="" loading="lazy" class="h-16 w-16 rounded-md bg-ink-800 object-cover" />
        {/each}
      </div>
    {/if}
    <p class="flex items-center gap-1.5 truncate text-sm text-ink-100">
      {#if card.password_protected}
        <!-- Sending a link grants nothing: its password still stands, so say so up front. -->
        <span title="Needs its password to open" class="text-ink-500">
          <LockIcon /><span class="sr-only">Password protected:</span>
        </span>
      {/if}
      <span class="truncate">{card.title || 'Untitled'}</span>
    </p>
    <p class="tnum mt-0.5 text-xs text-ink-500">
      {card.file_count} file{card.file_count === 1 ? '' : 's'} · {bytes(card.total_bytes ?? 0)}
      {#if card.expires_at}· expires in {until(card.expires_at)}{/if}
    </p>
  </a>
{:else}
  <div class="max-w-sm rounded-xl border border-dashed border-ink-800 px-3 py-2.5">
    <p class="truncate text-sm text-ink-500">{card.title || 'Untitled'}</p>
    <p class="mt-0.5 text-xs text-ink-700">{why[card.state] ?? 'This link is not available'}</p>
  </div>
{/if}
