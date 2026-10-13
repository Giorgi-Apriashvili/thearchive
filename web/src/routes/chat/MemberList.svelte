<script lang="ts">
  // Who is in a room, and who has been asked: the members popover on narrower screens and
  // the members panel beside the conversation on wide ones.
  import type { RoomMembers } from '../../lib/api'
  import { shortDate } from '../../lib/format'
  import Avatar from '../../lib/Avatar.svelte'
  import MemberName from '../../lib/MemberName.svelte'

  let { members }: { members: RoomMembers | null } = $props()
</script>

{#if !members}
  <p class="text-xs text-ink-500">Loading…</p>
{:else}
  <p class="text-[10px] uppercase tracking-wide text-ink-700">
    {members.members.length} member{members.members.length === 1 ? '' : 's'}
  </p>
  <ul class="mt-1.5 space-y-1">
    {#each members.members as person (person.username)}
      <li class="flex items-center justify-between gap-2">
        <span class="flex min-w-0 items-center gap-2 truncate text-sm">
          <Avatar src={person.avatar} name={person.username} size="xs" />
          <MemberName username={person.username} displayName={person.display_name} role={person.role} />
        </span>
        <!-- "creator" stays: it says something about this room, not about the
             account's tier, and it is the one thing the list cannot convey by colour. -->
        {#if person.is_creator}
          <span class="shrink-0 text-[10px] uppercase text-ink-500">creator</span>
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
          <!-- Dimmed rather than role-coloured: they are not in the room yet, and
               colouring them as though they were would misreport who is present. -->
          <span class="truncate text-sm text-ink-500">{person.username}</span>
          <span class="tnum shrink-0 text-[10px] text-ink-700">{shortDate(person.since)}</span>
        </li>
      {/each}
    </ul>
  {/if}
{/if}
