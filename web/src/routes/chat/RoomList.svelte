<script lang="ts">
  import { api, ApiError, type ChatRoom } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'
  import DeclineDialog from './DeclineDialog.svelte'

  let { onOpen }: { onOpen: (id: number) => void } = $props()

  let creating = $state(false)
  let newName = $state('')
  let error = $state('')
  // Declining a topic and avoiding a person are different intentions, so the choice is
  // made here rather than assumed.
  let declining = $state<ChatRoom | null>(null)

  async function create(event: SubmitEvent) {
    event.preventDefault()
    const name = newName.trim()
    if (!name) return
    error = ''
    creating = true
    try {
      const room = await api.post<{ id: number }>('/api/chat/rooms', { name })
      newName = ''
      await chat.loadRooms()
      onOpen(room.id)
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not create the room'
    } finally {
      creating = false
    }
  }

  async function respond(room: ChatRoom, action: string) {
    declining = null
    error = ''
    try {
      await api.post(`/api/chat/rooms/${room.id}/respond`, { action })
      await chat.loadRooms()
      if (action === 'accept') onOpen(room.id)
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'that did not work'
    }
  }

  // Invitations first: they are the only entries that need an answer.
  const ordered = $derived(
    [...chat.rooms].sort((a, b) => {
      const rank = (r: ChatRoom) => (r.state === 'invited' ? 0 : r.state === 'member' ? 1 : 2)
      return rank(a) - rank(b) || b.created_at - a.created_at
    }),
  )
</script>

<form onsubmit={create} class="flex gap-2">
  <input
    bind:value={newName}
    maxlength="60"
    placeholder="New room"
    class="min-w-0 flex-1 rounded-lg border border-ink-700 bg-ink-900 px-3 py-1.5 text-sm outline-none focus:border-accent"
  />
  <button
    disabled={creating || !newName.trim()}
    class="shrink-0 rounded-lg border border-ink-700 px-3 py-1.5 text-xs text-ink-300 transition hover:border-ink-500 disabled:opacity-40"
  >Create</button>
</form>

{#if error}
  <p class="mt-2 text-xs text-red-400">{error}</p>
{/if}
{#if chat.roomsError}
  <p class="mt-2 text-xs text-red-400">{chat.roomsError}</p>
{/if}

{#if chat.loadingRooms}
  <p class="mt-4 text-xs text-ink-500">Loading…</p>
{:else if ordered.length === 0}
  <p class="mt-4 text-xs text-ink-500">No rooms yet. Make the first one.</p>
{:else}
  <ul class="mt-3 max-h-[28rem] space-y-1 overflow-y-auto pr-1">
    {#each ordered as room (room.id)}
      {@const mine = room.state === 'member'}
      <li>
        <button
          onclick={() => mine && onOpen(room.id)}
          disabled={!mine}
          class="w-full rounded-lg border px-3 py-2 text-left transition {chat.openId === room.id
            ? 'border-accent bg-accent/5'
            : 'border-ink-800 hover:border-ink-700'} {mine ? '' : 'cursor-default'}"
        >
          <span class="flex items-center justify-between gap-2">
            <span class="truncate text-sm {mine ? 'text-ink-100' : 'text-ink-500'}">
              {room.name}
            </span>
            {#if room.unread}
              <span class="tnum shrink-0 rounded-full bg-accent px-1.5 text-[10px] font-medium text-ink-950">
                {room.unread}
              </span>
            {:else if room.state === 'invited'}
              <span class="shrink-0 rounded border border-accent/50 px-1 text-[10px] uppercase text-accent">
                invited
              </span>
            {:else if !mine}
              <!-- The name is visible to everyone on purpose: a room nobody can see is
                   a room nobody can ask to join. -->
              <span class="shrink-0 text-[10px] uppercase text-ink-700">locked</span>
            {/if}
          </span>
          <span class="mt-0.5 block truncate text-xs text-ink-500">
            {room.member_count} member{room.member_count === 1 ? '' : 's'} · by {room.created_by}
          </span>
        </button>

        {#if room.state === 'invited'}
          <div class="mt-1 flex gap-2 pl-3 text-xs">
            <span class="text-ink-500">{room.invited_by} invited you</span>
            <button class="text-accent hover:text-accent-dim" onclick={() => respond(room, 'accept')}>
              Accept
            </button>
            <button class="text-ink-500 hover:text-ink-300" onclick={() => (declining = room)}>
              Decline
            </button>
          </div>
        {/if}
      </li>
    {/each}
  </ul>
{/if}

{#if declining}
  {@const room = declining}
  <DeclineDialog {room} onChoose={(action) => respond(room, action)} onCancel={() => (declining = null)} />
{/if}
