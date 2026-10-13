<script lang="ts">
  // Running a room: its name and topic, who is in it, and deleting it. Offered to the
  // room's creator while they are in it, and to administrators; the server checks the
  // same rule on every request.
  import { untrack } from 'svelte'
  import { fade, scale } from 'svelte/transition'
  import { api, ApiError, type ChatRoom } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'
  import { router } from '../../lib/router.svelte'
  import Avatar from '../../lib/Avatar.svelte'
  import MemberName from '../../lib/MemberName.svelte'

  let { room, onClose }: { room: ChatRoom; onClose: () => void } = $props()

  // Starting values: the form is not meant to change under someone typing in it.
  let name = $state(untrack(() => room.name))
  let topic = $state(untrack(() => room.topic ?? ''))
  let busy = $state('')
  let error = $state('')
  let saved = $state('')
  // Deleting takes the room's name typed out: it removes every message in it, for everyone.
  let confirmName = $state('')

  $effect(() => {
    const previous = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    return () => {
      document.body.style.overflow = previous
    }
  })

  async function run(what: string, action: () => Promise<unknown>, done?: string) {
    busy = what
    error = ''
    saved = ''
    try {
      await action()
      await Promise.all([chat.loadRooms(), chat.loadMembers()])
      if (done) saved = done
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'that did not work'
    } finally {
      busy = ''
    }
  }

  const saveDetails = () =>
    run(
      'details',
      () => api.patch(`/api/chat/rooms/${room.id}`, { name: name.trim(), topic: topic.trim() }),
      'Saved.',
    )

  const removeMember = (username: string) =>
    run(username, () =>
      api.post(`/api/chat/rooms/${room.id}/members/${encodeURIComponent(username)}/remove`),
    )

  async function deleteRoom() {
    busy = 'delete'
    error = ''
    try {
      await api.del(`/api/chat/rooms/${room.id}`)
      onClose()
      router.go('/chat')
      await chat.loadRooms()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not delete the room'
      busy = ''
    }
  }
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && onClose()} />

<div class="fixed inset-0 z-50 flex items-center justify-center p-4" transition:fade={{ duration: 120 }}>
  <button
    onclick={onClose}
    aria-label="Close"
    tabindex="-1"
    class="absolute inset-0 cursor-default bg-ink-950/80 backdrop-blur-sm"
  ></button>

  <div
    role="dialog"
    aria-modal="true"
    aria-label="Room settings"
    class="relative z-10 max-h-[90dvh] w-full max-w-md overflow-y-auto rounded-xl border border-ink-700 bg-ink-900 p-5"
    transition:scale={{ duration: 120, start: 0.97 }}
  >
    <div class="flex items-baseline justify-between">
      <h2 class="text-sm font-medium text-ink-100">Room settings</h2>
      <button class="text-xs text-ink-500 hover:text-ink-300" onclick={onClose}>Close</button>
    </div>

    <form
      class="mt-4 space-y-3"
      onsubmit={(e) => {
        e.preventDefault()
        void saveDetails()
      }}
    >
      <label class="block">
        <span class="text-xs text-ink-500">Name</span>
        <input
          bind:value={name}
          maxlength="60"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-950 px-3 py-1.5 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block">
        <span class="text-xs text-ink-500">Topic — shown under the name, for everyone in the room</span>
        <input
          bind:value={topic}
          maxlength="200"
          placeholder="What this room is for"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-950 px-3 py-1.5 text-sm outline-none focus:border-accent"
        />
      </label>
      <div class="flex items-center gap-3">
        <button
          disabled={busy !== '' || !name.trim()}
          class="rounded-lg bg-accent px-3 py-1.5 text-xs font-medium text-ink-950 hover:bg-accent-dim disabled:opacity-40"
        >Save</button>
        {#if saved}<span class="text-xs text-accent">{saved}</span>{/if}
      </div>
    </form>

    <section class="mt-6">
      <h3 class="text-xs text-ink-500">Members</h3>
      {#if chat.members}
        <ul class="mt-2 space-y-1.5">
          {#each chat.members.members as person (person.username)}
            <li class="flex items-center justify-between gap-2 text-sm">
              <span class="flex min-w-0 items-center gap-2 truncate">
                <Avatar src={person.avatar} name={person.username} size="xs" />
                <MemberName username={person.username} displayName={person.display_name} role={person.role} />
              </span>
              {#if person.is_creator}
                <span class="shrink-0 text-[10px] uppercase text-ink-500">creator</span>
              {:else}
                <button
                  disabled={busy !== ''}
                  onclick={() => removeMember(person.username)}
                  class="shrink-0 text-xs text-ink-500 hover:text-red-400 disabled:opacity-40"
                >{busy === person.username ? 'Removing…' : 'Remove'}</button>
              {/if}
            </li>
          {/each}
          {#each chat.members.invited as person (person.username)}
            <li class="flex items-center justify-between gap-2 text-sm">
              <span class="truncate text-ink-500">{person.username} · invited</span>
              <button
                disabled={busy !== ''}
                onclick={() => removeMember(person.username)}
                class="shrink-0 text-xs text-ink-500 hover:text-red-400 disabled:opacity-40"
              >Withdraw</button>
            </li>
          {/each}
        </ul>
        <p class="mt-2 text-xs text-ink-500">
          Someone removed keeps nothing they had not already seen, and can come back only if
          invited again. What they said stays.
        </p>
      {:else}
        <p class="mt-2 text-xs text-ink-500">Loading…</p>
      {/if}
    </section>

    <section class="mt-6 rounded-lg border border-red-950 bg-red-950/20 p-3">
      <h3 class="text-xs font-medium text-red-300">Delete this room</h3>
      <p class="mt-1 text-xs text-ink-500">
        Removes the room and every message in it, for everyone. Links that were posted here
        are not affected. Type the room's name to confirm.
      </p>
      <div class="mt-2 flex gap-2">
        <input
          bind:value={confirmName}
          placeholder={room.name}
          class="min-w-0 flex-1 rounded-lg border border-ink-700 bg-ink-950 px-3 py-1.5 text-xs outline-none focus:border-red-400"
        />
        <button
          disabled={busy !== '' || confirmName.trim() !== room.name}
          onclick={deleteRoom}
          class="shrink-0 rounded-lg border border-red-900 px-3 py-1.5 text-xs text-red-300 hover:bg-red-950 disabled:opacity-40"
        >{busy === 'delete' ? 'Deleting…' : 'Delete room'}</button>
      </div>
    </section>

    {#if error}
      <p class="mt-4 rounded-lg bg-red-950/60 px-3 py-2 text-xs text-red-300">{error}</p>
    {/if}
  </div>
</div>
