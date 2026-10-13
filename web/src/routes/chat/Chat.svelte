<script lang="ts">
  import type { Me } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'
  import { router } from '../../lib/router.svelte'
  import RoomList from './RoomList.svelte'
  import Room from './Room.svelte'
  import Blocks from './Blocks.svelte'
  import MemberList from './MemberList.svelte'

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

<!-- minmax(0, 1fr), not 1fr — grid-cols-1 is the same on phones: a plain 1fr, or the
     implicit column, will not shrink below its widest content, so one long link in a
     message would push the conversation past the page.
     From 2xl a third column keeps the open room's members in view. -->
<div class="mt-6 grid grid-cols-1 gap-4 md:grid-cols-[16rem_minmax(0,1fr)] 2xl:grid-cols-[16rem_minmax(0,1fr)_16rem]">
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
      <div class="flex h-[34rem] items-center justify-center rounded-xl border border-dashed border-ink-800 px-6 text-center md:h-[calc(100dvh-15rem)] md:min-h-[28rem]">
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

  <aside class="hidden 2xl:block">
    {#if chat.open}
      <div class="max-h-[calc(100dvh-15rem)] overflow-y-auto rounded-xl border border-ink-800 p-3">
        <MemberList members={chat.members} />
      </div>
    {/if}
  </aside>
</div>
