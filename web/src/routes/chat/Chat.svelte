<script lang="ts">
  import type { Me } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'
  import { router } from '../../lib/router.svelte'
  import RoomList from './RoomList.svelte'
  import Room from './Room.svelte'
  import Blocks from './Blocks.svelte'

  let { me }: { me: Me } = $props()

  // The fast message poll lives and dies with this pane; the slow room poll that feeds
  // the unread badge is the shell's, and keeps running without it.
  $effect(() => chat.activate())

  // The open room comes from the URL, so a room is linkable and the back button works.
  const routeId = $derived.by(() => {
    const [section, id] = router.segments
    return section === 'chat' && id ? Number(id) : null
  })

  $effect(() => {
    void chat.select(Number.isFinite(routeId) ? routeId : null)
  })

  function open(id: number) {
    router.go(`/chat/${id}`)
  }

  let showBlocks = $state(false)
</script>

<div class="mt-6 grid gap-4 md:grid-cols-[16rem_1fr]">
  <aside>
    <RoomList onOpen={open} />
    <button
      class="mt-3 text-xs text-ink-500 hover:text-ink-300"
      onclick={() => (showBlocks = !showBlocks)}
    >{showBlocks ? 'Hide blocked' : 'Blocked'}</button>
    {#if showBlocks}
      <div class="mt-2">
        <Blocks />
      </div>
    {/if}
  </aside>

  <section>
    {#if chat.open}
      <!-- Keyed on the room so the composer, scroll position and draft do not follow
           you from one conversation into the next. -->
      {#key chat.open.id}
        <Room room={chat.open} {me} />
      {/key}
    {:else}
      <div class="flex h-[34rem] items-center justify-center rounded-xl border border-dashed border-ink-800 px-6 text-center">
        <p class="text-sm text-ink-500">
          {#if routeId !== null}
            That room is not open to you. Ask whoever started it for an invitation.
          {:else}
            Pick a room, or start one.
          {/if}
        </p>
      </div>
    {/if}
  </section>
</div>
