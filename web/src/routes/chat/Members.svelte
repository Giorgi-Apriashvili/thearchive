<script lang="ts">
  import { fade } from 'svelte/transition'
  import type { RoomMembers } from '../../lib/api'
  import { shortDate } from '../../lib/format'

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
  class="absolute right-0 top-full z-40 mt-1 max-h-80 w-56 overflow-y-auto rounded-xl border border-ink-700 bg-ink-900 p-3 shadow-xl outline-none"
  transition:fade={{ duration: 100 }}
>
  {#if !members}
    <p class="text-xs text-ink-500">Loading…</p>
  {:else}
    <p class="text-[10px] uppercase tracking-wide text-ink-700">
      {members.members.length} member{members.members.length === 1 ? '' : 's'}
    </p>
    <ul class="mt-1.5 space-y-1">
      {#each members.members as person (person.username)}
        <li class="flex items-baseline justify-between gap-2">
          <span class="truncate text-sm text-ink-100">{person.username}</span>
          {#if person.is_creator}
            <span class="shrink-0 text-[10px] uppercase text-accent">creator</span>
          {:else}
            <span class="tnum shrink-0 text-[10px] text-ink-700">{shortDate(person.since)}</span>
          {/if}
        </li>
      {/each}
    </ul>

    {#if members.invited.length}
      <p class="mt-3 text-[10px] uppercase tracking-wide text-ink-700">
        Invited, not yet answered
      </p>
      <ul class="mt-1.5 space-y-1">
        {#each members.invited as person (person.username)}
          <li class="flex items-baseline justify-between gap-2">
            <span class="truncate text-sm text-ink-500">{person.username}</span>
            <span class="tnum shrink-0 text-[10px] text-ink-700">{shortDate(person.since)}</span>
          </li>
        {/each}
      </ul>
    {/if}
  {/if}
</div>
