<script lang="ts">
  import { untrack } from 'svelte'
  import { api, ApiError, type Me, type Profile } from '../lib/api'
  import { link } from '../lib/router.svelte'
  import Avatar from '../lib/Avatar.svelte'

  let { me, onChanged }: { me: Me; onChanged: () => void } = $props()

  // ---- profile ---------------------------------------------------------------------
  // Mirrors the server's limits so the counters are honest. Characters, not bytes, as
  // the server counts them: [...text] splits by code point, which .length does not.
  const MAX_NAME = 40
  const MAX_BIO = 500
  const MAX_PICTURE_BYTES = 10 * 1024 * 1024
  const chars = (text: string) => [...text.trim()].length

  let displayName = $state('')
  let bio = $state('')
  let profileBusy = $state(false)
  let profileError = $state('')
  let profileSaved = $state(false)
  let pictureBusy = $state(false)
  let pictureError = $state('')

  // Read through the same endpoint other members use, so what is edited here is exactly
  // what they see. Once, on arrival: `me` is refreshed after every save and every new
  // picture, and reloading then would wipe whatever is half-typed in the other field.
  api
    .get<Profile>(`/api/users/${encodeURIComponent(untrack(() => me.username))}`)
    .then((profile) => {
      displayName = profile.display_name ?? ''
      bio = profile.bio ?? ''
    })
    .catch(() => (profileError = 'your profile could not be loaded'))

  async function saveProfile(event: SubmitEvent) {
    event.preventDefault()
    profileError = ''
    profileSaved = false
    profileBusy = true
    try {
      await api.patch('/api/me/profile', { display_name: displayName, bio })
      profileSaved = true
      onChanged()
    } catch (e) {
      profileError = e instanceof ApiError ? e.message : 'could not save your profile'
    } finally {
      profileBusy = false
    }
  }

  async function choosePicture(event: Event) {
    const input = event.currentTarget as HTMLInputElement
    const file = input.files?.[0]
    input.value = ''
    if (!file) return
    pictureError = ''
    // Checked here only to save uploading 40 MB to be told no; the server decides.
    if (file.size > MAX_PICTURE_BYTES) {
      pictureError = 'a picture is at most 10 MB'
      return
    }
    pictureBusy = true
    try {
      await api.put('/api/me/avatar', file)
      onChanged()
    } catch (e) {
      pictureError = e instanceof ApiError ? e.message : 'could not use that picture'
    } finally {
      pictureBusy = false
    }
  }

  async function removePicture() {
    pictureError = ''
    pictureBusy = true
    try {
      await api.del('/api/me/avatar')
      onChanged()
    } catch (e) {
      pictureError = e instanceof ApiError ? e.message : 'could not remove your picture'
    } finally {
      pictureBusy = false
    }
  }

  // ---- password ----------------------------------------------------------------------

  const MIN_LENGTH = 6

  let current = $state('')
  let next = $state('')
  let confirm = $state('')
  let busy = $state(false)
  let error = $state('')
  let done = $state(false)

  // Checked here as well as on the server so the two obvious mistakes cost no round
  // trip. The server remains the authority — these only decide whether to bother it.
  const problem = $derived.by(() => {
    if (!current || !next || !confirm) return 'fill in all three'
    if (next.length < MIN_LENGTH) return `the new password needs at least ${MIN_LENGTH} characters`
    if (next !== confirm) return 'the two new passwords do not match'
    if (next === current) return 'the new password must be different'
    return null
  })

  async function submit(event: SubmitEvent) {
    event.preventDefault()
    if (problem || busy) return
    error = ''
    busy = true
    try {
      await api.post('/api/auth/password', { current_password: current, new_password: next })
      current = ''
      next = ''
      confirm = ''
      done = true
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not change your password'
    } finally {
      busy = false
    }
  }
</script>

<div class="mx-auto max-w-lg px-4 py-10">
  <a href="/" onclick={(e) => link(e, '/')} class="text-xs text-ink-500 hover:text-ink-300">
    ← Back
  </a>

  <h1 class="mt-3 text-lg font-medium">Account</h1>
  <p class="mt-1 text-sm text-ink-500">
    {me.username}
    {#if me.role !== 'user'}· {me.role}{/if}
  </p>

  <section class="mt-6 rounded-xl border border-ink-800 p-4">
    <div class="flex items-baseline justify-between gap-3">
      <h2 class="text-sm font-medium text-ink-300">Profile</h2>
      <a
        href="/u/{encodeURIComponent(me.username)}"
        onclick={(e) => link(e, `/u/${encodeURIComponent(me.username)}`)}
        class="text-xs text-ink-500 hover:text-ink-300"
      >View your profile</a>
    </div>
    <p class="mt-1 text-xs text-ink-500">
      Seen by every member here, never by someone who only has a link. Your @username is
      always shown beside your display name, so nobody can pass as you by choosing yours.
    </p>

    <div class="mt-4 flex items-center gap-4">
      <Avatar src={me.avatar} name={me.username} size="md" />
      <div class="flex flex-wrap items-center gap-3 text-xs">
        <label
          class="cursor-pointer rounded-lg border border-ink-700 px-3 py-1.5 text-ink-300 transition hover:border-ink-500 {pictureBusy
            ? 'pointer-events-none opacity-40'
            : ''}"
        >
          {pictureBusy ? 'Working…' : me.avatar ? 'Change picture' : 'Add a picture'}
          <input type="file" accept="image/*" class="hidden" onchange={choosePicture} />
        </label>
        {#if me.avatar}
          <button disabled={pictureBusy} onclick={removePicture} class="text-ink-500 hover:text-red-400 disabled:opacity-40">
            Remove
          </button>
        {/if}
      </div>
    </div>
    <p class="mt-2 text-xs text-ink-500">
      Cropped square from the centre. Only a small copy is kept, and everything else in the
      file — including where a photo was taken — is thrown away.
    </p>
    {#if pictureError}
      <p class="mt-2 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{pictureError}</p>
    {/if}

    <form onsubmit={saveProfile} class="mt-5 space-y-3">
      <label class="block">
        <span class="flex justify-between text-xs text-ink-500">
          <span>Display name</span>
          <span class="tnum {chars(displayName) > MAX_NAME ? 'text-red-400' : ''}">{chars(displayName)}/{MAX_NAME}</span>
        </span>
        <input
          bind:value={displayName}
          placeholder={me.username}
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block">
        <span class="flex justify-between text-xs text-ink-500">
          <span>About you</span>
          <span class="tnum {chars(bio) > MAX_BIO ? 'text-red-400' : ''}">{chars(bio)}/{MAX_BIO}</span>
        </span>
        <textarea
          bind:value={bio}
          rows="4"
          class="mt-1 w-full resize-y rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        ></textarea>
      </label>

      {#if profileError}
        <p class="rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{profileError}</p>
      {/if}
      {#if profileSaved}
        <p class="text-xs text-ink-500">Saved.</p>
      {/if}
      <button
        disabled={profileBusy || chars(displayName) > MAX_NAME || chars(bio) > MAX_BIO}
        class="rounded-lg bg-accent px-3 py-2 text-sm font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
      >{profileBusy ? 'Saving…' : 'Save profile'}</button>
    </form>
  </section>

  <section class="mt-4 rounded-xl border border-ink-800 p-4">
    <h2 class="text-sm font-medium text-ink-300">Change your password</h2>
    <p class="mt-1 text-xs text-ink-500">
      You will stay signed in here. Everywhere else you are signed in will be signed out —
      which is the point, if you are changing it because someone else might know it.
    </p>

    <form onsubmit={submit} class="mt-4 space-y-3">
      <label class="block">
        <span class="text-xs text-ink-500">Current password</span>
        <input
          type="password"
          bind:value={current}
          autocomplete="current-password"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block">
        <span class="text-xs text-ink-500">New password</span>
        <input
          type="password"
          bind:value={next}
          autocomplete="new-password"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>
      <label class="block">
        <span class="text-xs text-ink-500">Confirm new password</span>
        <input
          type="password"
          bind:value={confirm}
          autocomplete="new-password"
          class="mt-1 w-full rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
        />
      </label>

      {#if error}
        <p class="rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">{error}</p>
      {/if}
      {#if done}
        <p class="rounded-lg border border-accent/40 bg-accent/5 px-3 py-2 text-sm text-ink-300">
          Password changed. Any other sessions have been signed out.
        </p>
      {/if}

      <div class="flex items-center justify-between gap-3">
        <!-- The reason the button is disabled, rather than a button that looks broken.
             Only once something has been typed: telling someone to fill in a form they
             have not started is noise. -->
        <span class="text-xs text-ink-500">
          {#if problem && (current || next || confirm)}{problem}{/if}
        </span>
        <button
          disabled={busy || problem !== null}
          class="shrink-0 rounded-lg bg-accent px-3 py-2 text-sm font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
        >
          {busy ? 'Changing…' : 'Change password'}
        </button>
      </div>
    </form>
  </section>

  <p class="mt-4 text-xs text-ink-500">
    Forgotten it instead? There is no email in this system, so nothing can be sent to you —
    ask an administrator to reset it.
  </p>
</div>
