<script lang="ts">
  import { api, ApiError, type ChatBlocks } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'

  // A block you cannot find is a block you cannot undo, which turns a moment's
  // irritation into a permanent one.
  let blocks = $state<ChatBlocks | null>(null)
  let error = $state('')

  async function load() {
    try {
      blocks = await api.get<ChatBlocks>('/api/chat/blocks')
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not load your blocks'
    }
  }
  load()

  async function unblock(kind: 'room' | 'user', id: number) {
    error = ''
    try {
      await api.del(`/api/chat/blocks/${kind}/${id}`)
      await load()
      // An unblocked room reappears in the list, so it has to be re-read.
      await chat.loadRooms()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not undo that'
    }
  }

  const empty = $derived(blocks !== null && !blocks.rooms.length && !blocks.users.length)
</script>

<div class="rounded-xl border border-ink-800 p-4">
  <h2 class="text-sm text-ink-300">Blocked</h2>

  {#if error}
    <p class="mt-2 text-xs text-red-400">{error}</p>
  {/if}

  {#if empty}
    <p class="mt-2 text-xs text-ink-500">Nothing blocked.</p>
  {:else if blocks}
    {#if blocks.users.length}
      <p class="mt-3 text-xs text-ink-500">People who cannot invite you</p>
      <ul class="mt-1.5 space-y-1">
        {#each blocks.users as person (person.id)}
          <li class="flex items-center justify-between gap-3 text-sm">
            <span class="truncate text-ink-100">{person.username}</span>
            <button
              class="shrink-0 text-xs text-ink-500 hover:text-accent"
              onclick={() => unblock('user', person.id)}
            >Unblock</button>
          </li>
        {/each}
      </ul>
    {/if}

    {#if blocks.rooms.length}
      <p class="mt-4 text-xs text-ink-500">Rooms hidden from your list</p>
      <ul class="mt-1.5 space-y-1">
        {#each blocks.rooms as room (room.id)}
          <li class="flex items-center justify-between gap-3 text-sm">
            <span class="truncate text-ink-100">{room.name}</span>
            <button
              class="shrink-0 text-xs text-ink-500 hover:text-accent"
              onclick={() => unblock('room', room.id)}
            >Unhide</button>
          </li>
        {/each}
      </ul>
    {/if}
  {/if}
</div>
