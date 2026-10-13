<script lang="ts">
  import { fade } from 'svelte/transition'
  import type { RoomMembers } from '../../lib/api'
  import MemberList from './MemberList.svelte'

  let {
    members,
    roomName,
    onClose,
  }: {
    members: RoomMembers | null
    roomName: string
    onClose: () => void
  } = $props()

  let panel = $state<HTMLElement | null>(null)
  $effect(() => panel?.focus())
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && onClose()} />

<!-- A popover rather than a dialog: seeing who is in a room is a glance, not a decision,
     and a modal would make it feel like one. -->
<button onclick={onClose} aria-label="Close" tabindex="-1" class="fixed inset-0 z-30 cursor-default"
></button>

<div
  bind:this={panel}
  role="group"
  aria-label="Members of {roomName}"
  tabindex="-1"
  class="absolute right-0 top-full z-40 mt-1 max-h-80 w-72 overflow-y-auto rounded-xl border border-ink-700 bg-ink-900 p-3 shadow-xl outline-none"
  transition:fade={{ duration: 100 }}
>
  <MemberList {members} />
</div>
