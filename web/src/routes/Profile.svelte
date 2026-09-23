<script lang="ts">
  import { api, ApiError, type Profile } from '../lib/api'
  import { shortDate } from '../lib/format'
  import { link } from '../lib/router.svelte'
  import { roleColor } from '../lib/roles'
  import Avatar from '../lib/Avatar.svelte'
  import MemberName from '../lib/MemberName.svelte'

  let { username }: { username: string } = $props()

  let profile = $state<Profile | null>(null)
  let error = $state('')

  // Keyed on the name, so following a link from one profile to another (an inviter, say)
  // loads the new one rather than showing the old.
  $effect(() => {
    const name = username
    profile = null
    error = ''
    api
      .get<Profile>(`/api/users/${encodeURIComponent(name)}`)
      .then((loaded) => {
        if (name === username) profile = loaded
      })
      .catch((e) => {
        error =
          e instanceof ApiError && e.status === 404
            ? 'No member by that name. They may have been renamed, or their account deleted.'
            : 'This profile could not be loaded.'
      })
  })
</script>

<div class="mx-auto max-w-2xl px-4 py-10">
  <a href="/" onclick={(e) => link(e, '/')} class="text-xs text-ink-500 hover:text-ink-300">← Back</a>

  {#if error}
    <p class="mt-8 text-sm text-ink-500">{error}</p>
  {:else if !profile}
    <p class="mt-8 text-sm text-ink-500">Loading…</p>
  {:else}
    <header class="mt-6 flex items-center gap-5">
      <Avatar src={profile.avatar} name={profile.username} size="lg" />
      <div class="min-w-0">
        {#if profile.display_name}
          <!-- <bdi>: a right-to-left name must not reorder the username beneath it. -->
          <h1 class="truncate text-xl font-semibold tracking-tight">
            <bdi class={roleColor(profile.role)}>{profile.display_name}</bdi>
          </h1>
          <p class="text-sm text-ink-500">@{profile.username}</p>
        {:else}
          <h1 class="truncate text-xl font-semibold tracking-tight {roleColor(profile.role)}">
            {profile.username}
          </h1>
        {/if}
        <p class="mt-1 text-xs text-ink-500">
          Joined {shortDate(profile.joined)}
          {#if profile.invited_by}
            · invited by <MemberName username={profile.invited_by} />
          {/if}
        </p>
      </div>
    </header>

    {#if profile.is_me}
      <a
        href="/account"
        onclick={(e) => link(e, '/account')}
        class="mt-5 inline-block rounded-lg border border-ink-700 px-3 py-1.5 text-xs text-ink-300 transition hover:border-ink-500"
      >Edit your profile</a>
    {/if}

    {#if profile.bio}
      <!-- Plain text, never {@html}: another member wrote this. Line breaks they typed
           are kept; nothing else is interpreted. -->
      <p class="mt-6 whitespace-pre-line break-words text-sm leading-relaxed text-ink-300">
        {profile.bio}
      </p>
    {:else if profile.is_me}
      <p class="mt-6 text-sm text-ink-500">You have not written anything about yourself yet.</p>
    {/if}

    <section class="mt-8">
      <h2 class="text-sm font-medium text-ink-300">
        {profile.is_me ? 'Your rooms' : 'Rooms you share'}
      </h2>
      {#if profile.shared_rooms.length === 0}
        <p class="mt-2 text-sm text-ink-500">
          {profile.is_me ? 'You are not in any rooms yet.' : 'None yet.'}
        </p>
      {:else}
        <ul class="mt-2 flex flex-wrap gap-2">
          {#each profile.shared_rooms as room (room.id)}
            <li>
              <a
                href="/chat/{room.id}"
                onclick={(e) => link(e, `/chat/${room.id}`)}
                class="inline-block rounded-lg border border-ink-800 px-3 py-1.5 text-sm text-ink-100 transition hover:border-ink-600"
              >{room.name}</a>
            </li>
          {/each}
        </ul>
      {/if}
    </section>
  {/if}
</div>
