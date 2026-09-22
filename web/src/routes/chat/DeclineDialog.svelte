<script lang="ts">
  import { fade, scale } from 'svelte/transition'
  import type { ChatRoom } from '../../lib/api'

  // Not Confirm.svelte: that dialog asks one yes/no question, and this is a choice
  // between three different outcomes. Declining a topic, hiding a room and avoiding a
  // person are separate intentions, and flattening them into "are you sure" would make
  // the mildest of the three the only one on offer.
  let {
    room,
    onChoose,
    onCancel,
  }: {
    room: ChatRoom
    onChoose: (action: 'decline' | 'block_room' | 'block_user') => void
    onCancel: () => void
  } = $props()

  // The panel takes focus rather than any one choice. Confirm.svelte focuses its action
  // so Enter confirms, but there is no default answer here — pre-selecting one of three
  // outcomes, one of which blocks a person, would be picking for the user.
  let panel = $state<HTMLElement | null>(null)
  $effect(() => panel?.focus())

  $effect(() => {
    const previous = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    return () => {
      document.body.style.overflow = previous
    }
  })

  const choices = $derived([
    {
      action: 'decline' as const,
      label: 'Just decline',
      note: `The room stays visible and ${room.invited_by} can ask you again.`,
      danger: false,
    },
    {
      action: 'block_room' as const,
      label: 'Hide this room',
      note: 'It disappears from your list and no invitation to it reaches you again.',
      danger: false,
    },
    {
      action: 'block_user' as const,
      label: `Block ${room.invited_by}`,
      note: 'They can no longer invite you to any room. They are not told.',
      danger: true,
    },
  ])
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && onCancel()} />

<div class="fixed inset-0 z-50 flex items-center justify-center p-4" transition:fade={{ duration: 120 }}>
  <button
    onclick={onCancel}
    aria-label="Cancel"
    tabindex="-1"
    class="absolute inset-0 cursor-default bg-ink-950/80 backdrop-blur-sm"
  ></button>

  <div
    bind:this={panel}
    role="dialog"
    aria-modal="true"
    aria-label="Decline {room.name}"
    tabindex="-1"
    class="relative z-10 w-full max-w-sm rounded-xl border border-ink-700 bg-ink-900 p-5 outline-none"
    transition:scale={{ duration: 120, start: 0.97 }}
  >
    <h2 class="text-sm font-medium text-ink-100">Decline {room.name}?</h2>

    <div class="mt-4 space-y-2">
      {#each choices as choice (choice.action)}
        <button
          onclick={() => onChoose(choice.action)}
          class="block w-full rounded-lg border px-3 py-2 text-left transition {choice.danger
            ? 'border-red-950 hover:border-red-900 hover:bg-red-950/30'
            : 'border-ink-700 hover:border-ink-500'}"
        >
          <span class="block text-xs {choice.danger ? 'text-red-300' : 'text-ink-100'}">
            {choice.label}
          </span>
          <span class="mt-0.5 block text-xs text-ink-500">{choice.note}</span>
        </button>
      {/each}
    </div>

    <div class="mt-4 flex justify-end">
      <button onclick={onCancel} class="text-xs text-ink-500 hover:text-ink-300">Cancel</button>
    </div>
  </div>
</div>
