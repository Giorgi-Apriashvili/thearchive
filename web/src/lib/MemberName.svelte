<script lang="ts">
  // How a member is named anywhere something is attributed to them: their display name
  // if they chose one, and always their @username beside it.
  //
  // The username is the part that cannot be faked. Display names are whatever their
  // owner typed, so two people can pick the same one, or one can pick another's; showing
  // the display name alone would make a message from "Giorgi" mean nothing. Keeping the
  // rule in this one component means it cannot be forgotten at a single call site.
  import { link } from './router.svelte'
  import { roleColor } from './roles'

  let {
    username,
    displayName,
    role,
    linked = true,
    departed = false,
  }: {
    username: string
    displayName?: string
    role?: string
    /** False where the reader cannot open profiles — someone holding only a link. */
    linked?: boolean
    /** The account is gone: nothing to link to, and no display name survives it. */
    departed?: boolean
  } = $props()

  const href = $derived(`/u/${encodeURIComponent(username)}`)
</script>

{#snippet label()}
  {#if displayName && !departed}
    <!-- <bdi> keeps a right-to-left name from reordering the @username after it. The
         server already refuses the explicit direction-override characters; this covers
         names that are simply written in a right-to-left script. -->
    <bdi class="font-medium {roleColor(role)}">{displayName}</bdi>
    <span class="text-ink-500">@{username}</span>
  {:else}
    <span class="font-medium {roleColor(role)}">{username}</span>
  {/if}
{/snippet}

{#if linked && !departed}
  <a {href} onclick={(e) => link(e, href)} class="inline-flex items-baseline gap-1 hover:underline">
    {@render label()}
  </a>
{:else}
  <span class="inline-flex items-baseline gap-1">{@render label()}</span>
{/if}
