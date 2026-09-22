<script lang="ts">
  import { api, ApiError, type ChatMessage, type ChatRoom, type Me } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'
  import { chatTime } from '../../lib/format'
  import { linkify } from '../../lib/linkify'
  import Confirm from '../../lib/Confirm.svelte'

  let { room, me }: { room: ChatRoom; me: Me } = $props()

  let draft = $state('')
  let sending = $state(false)
  let error = $state('')

  let inviting = $state(false)
  let inviteName = $state('')
  let inviteNote = $state('')

  let removing = $state<ChatMessage | null>(null)

  let scroller = $state<HTMLElement | null>(null)
  // Only follow new messages when the reader is already at the bottom. Yanking someone
  // out of the history they are reading because a message arrived is the single most
  // annoying thing a chat window can do.
  let pinned = true

  function onScroll() {
    if (!scroller) return
    pinned = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 60
  }

  $effect(() => {
    // Touch the length so this re-runs when a message arrives.
    chat.messages.length
    if (pinned && scroller) scroller.scrollTop = scroller.scrollHeight
  })

  // A fresh room starts at the bottom regardless of where the last one was left.
  $effect(() => {
    room.id
    pinned = true
  })

  async function send(event: SubmitEvent) {
    event.preventDefault()
    const body = draft.trim()
    if (!body || sending) return
    error = ''
    sending = true
    try {
      await api.post(`/api/chat/rooms/${room.id}/messages`, { body })
      draft = ''
      pinned = true
      await chat.refresh()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not send that'
    } finally {
      sending = false
    }
  }

  // Enter sends, Shift+Enter breaks the line — the convention everywhere else, and the
  // textarea exists only so the second one is possible.
  function onKeydown(event: KeyboardEvent) {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault()
      ;(event.currentTarget as HTMLTextAreaElement).form?.requestSubmit()
    }
  }

  async function invite(event: SubmitEvent) {
    event.preventDefault()
    const username = inviteName.trim()
    if (!username) return
    error = ''
    try {
      await api.post(`/api/chat/rooms/${room.id}/invite`, { username })
      inviteName = ''
      inviting = false
      // Deliberately not "they have been invited": the server reports success even when
      // the invitee has blocked the inviter, so that a block is not disclosed. Saying
      // more than this would be saying something we do not know.
      inviteNote = `Invitation sent to ${username}.`
      setTimeout(() => (inviteNote = ''), 4000)
      await chat.loadRooms()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not invite them'
    }
  }

  async function remove(message: ChatMessage) {
    removing = null
    try {
      await api.del(`/api/chat/messages/${message.id}`)
      // The tombstone replaces an id the poll cursor has already passed, so this client
      // has to re-read the room to stop showing what it just took down.
      await chat.reload()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not remove it'
    }
  }

  // The invite box is for whoever can actually use it; everyone else gets no dead
  // control. The server enforces the same rule.
  const canInvite = $derived(room.is_creator || me.role === 'admin')
</script>

<div class="flex h-[34rem] flex-col rounded-xl border border-ink-800">
  <header class="flex items-center justify-between gap-3 border-b border-ink-800 px-4 py-2.5">
    <div class="min-w-0">
      <h2 class="truncate text-sm text-ink-100">{room.name}</h2>
      <p class="truncate text-xs text-ink-500">
        {room.member_count} member{room.member_count === 1 ? '' : 's'} · started by {room.created_by}
      </p>
    </div>
    {#if canInvite}
      <button
        class="shrink-0 text-xs text-ink-500 hover:text-ink-300"
        onclick={() => (inviting = !inviting)}
      >{inviting ? 'Close' : 'Invite'}</button>
    {/if}
  </header>

  {#if inviting}
    <form onsubmit={invite} class="flex gap-2 border-b border-ink-800 px-4 py-2">
      <input
        bind:value={inviteName}
        placeholder="username"
        autocapitalize="none"
        autocorrect="off"
        spellcheck="false"
        class="min-w-0 flex-1 rounded-lg border border-ink-700 bg-ink-950 px-3 py-1.5 text-xs outline-none focus:border-accent"
      />
      <button class="shrink-0 rounded-lg border border-ink-700 px-3 py-1.5 text-xs hover:border-ink-500">
        Invite
      </button>
    </form>
  {/if}

  {#if inviteNote}
    <p class="border-b border-ink-800 px-4 py-2 text-xs text-ink-500">{inviteNote}</p>
  {/if}

  <div bind:this={scroller} onscroll={onScroll} class="flex-1 space-y-3 overflow-y-auto px-4 py-3">
    {#if chat.loadingMessages}
      <p class="text-xs text-ink-500">Loading…</p>
    {:else if chat.messages.length === 0}
      <p class="text-xs text-ink-500">Nothing here yet.</p>
    {/if}

    {#each chat.messages as message (message.id)}
      <div class="group">
        <p class="flex items-baseline gap-2">
          <span class="text-xs font-medium {message.author === me.username ? 'text-accent' : 'text-ink-300'}">
            {message.author}
          </span>
          {#if message.author_departed}
            <span class="text-[10px] uppercase text-ink-700" title="This account has been deleted">
              former member
            </span>
          {/if}
          <span class="tnum text-[10px] text-ink-700">{chatTime(message.created_at)}</span>
          {#if me.role === 'admin' && !message.deleted}
            <button
              class="ml-auto text-[10px] text-ink-700 opacity-0 transition group-hover:opacity-100 hover:text-red-400"
              onclick={() => (removing = message)}
            >Remove</button>
          {/if}
        </p>
        {#if message.deleted}
          <!-- A tombstone, not a gap: quietly reflowing a conversation around what was
               taken out is its own kind of dishonesty. -->
          <p class="mt-0.5 text-sm italic text-ink-700">Removed by {message.deleted_by}</p>
        {:else}
          <!-- Rendered as segments, never {@html}. linkify() returns data precisely so
               that Svelte keeps escaping both the text and the href — this is the one
               place in the app where another person's arbitrary text reaches the DOM. -->
          <p class="mt-0.5 whitespace-pre-wrap break-words text-sm text-ink-100">
            {#each linkify(message.body ?? '') as segment}{#if segment.href}<a
                  href={segment.href}
                  target="_blank"
                  rel="noopener noreferrer"
                  class="text-accent underline decoration-accent/40 underline-offset-2 hover:decoration-accent"
                  >{segment.text}</a
                >{:else}{segment.text}{/if}{/each}
          </p>
        {/if}
      </div>
    {/each}
  </div>

  {#if chat.messagesError}
    <p class="border-t border-ink-800 px-4 py-2 text-xs text-red-400">{chat.messagesError}</p>
  {/if}
  {#if error}
    <p class="border-t border-ink-800 px-4 py-2 text-xs text-red-400">{error}</p>
  {/if}

  <form onsubmit={send} class="flex items-end gap-2 border-t border-ink-800 px-4 py-3">
    <textarea
      bind:value={draft}
      onkeydown={onKeydown}
      rows="1"
      maxlength="4000"
      placeholder="Message {room.name}"
      class="max-h-32 min-h-[2.25rem] flex-1 resize-none rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
    ></textarea>
    <button
      disabled={sending || !draft.trim()}
      class="shrink-0 rounded-lg bg-accent px-3 py-2 text-xs font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
    >Send</button>
  </form>
</div>

{#if removing}
  {@const message = removing}
  <Confirm
    title="Remove this message?"
    body="It stays in the room as “Removed by {me.username}”, and its text is gone for good. History is permanent here, so nothing is deleted quietly."
    confirmLabel="Remove"
    danger={true}
    onConfirm={() => remove(message)}
    onCancel={() => (removing = null)}
  />
{/if}
