<script lang="ts">
  import { api, ApiError, type ChatMessage, type ChatRoom, type Me, type ShareSummary } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'
  import { chatTime } from '../../lib/format'
  import { parseMessage } from '../../lib/message'
  import { link } from '../../lib/router.svelte'
  import Avatar from '../../lib/Avatar.svelte'
  import MemberName from '../../lib/MemberName.svelte'
  import ShareCard from '../../lib/ShareCard.svelte'
  import SharePicker from '../../lib/SharePicker.svelte'
  import LockIcon from '../../lib/LockIcon.svelte'
  import Confirm from '../../lib/Confirm.svelte'
  import Members from './Members.svelte'

  let { room, me }: { room: ChatRoom; me: Me } = $props()

  let draft = $state('')
  // Links to go with the next message, chosen through the picker.
  let attachments = $state<ShareSummary[]>([])
  let picking = $state(false)
  let sending = $state(false)
  let error = $state('')

  let showMembers = $state(false)
  let composer = $state<HTMLTextAreaElement | null>(null)

  // @-autocomplete. The candidate list is the room's membership, because that is what
  // the server will resolve a mention against — offering a completion that would not
  // resolve is a promise the send cannot keep.
  let mentionQuery = $state<string | null>(null)
  let mentionAt = $state(0)
  let highlighted = $state(0)

  const candidates = $derived.by(() => {
    if (mentionQuery === null) return []
    const names = (chat.members?.members ?? []).map((m) => m.username)
    return names
      .filter((n) => n !== me.username && n.startsWith(mentionQuery!))
      .slice(0, 6)
  })

  // The @token the caret currently sits in, or null. Only a token that starts a word
  // counts, matching how the server decides what is a mention.
  function syncMentionQuery() {
    const field = composer
    if (!field) return
    const upto = draft.slice(0, field.selectionStart ?? 0)
    const match = upto.match(/(?:^|[^\w.-])@([\w.-]*)$/)
    if (!match) {
      mentionQuery = null
      return
    }
    mentionQuery = match[1].toLowerCase()
    mentionAt = upto.length - match[1].length - 1
    highlighted = 0
  }

  function complete(name: string) {
    const field = composer
    if (!field) return
    const end = (field.selectionStart ?? 0)
    draft = `${draft.slice(0, mentionAt)}@${name} ${draft.slice(end)}`
    mentionQuery = null
    const caret = mentionAt + name.length + 2
    // The caret has to move after Svelte writes the new value back into the field.
    queueMicrotask(() => {
      field.focus()
      field.setSelectionRange(caret, caret)
    })
  }

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
    if ((!body && attachments.length === 0) || sending) return
    error = ''
    sending = true
    try {
      await api.post(`/api/chat/rooms/${room.id}/messages`, {
        body,
        shares: attachments.map((a) => a.token),
      })
      draft = ''
      attachments = []
      mentionQuery = null
      pinned = true
      await chat.refresh()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not send that'
    } finally {
      sending = false
    }
  }

  // Enter sends, Shift+Enter breaks the line — the convention everywhere else, and the
  // textarea exists only so the second one is possible. While the completion list is
  // open, Enter and Tab take the highlighted name instead: a list you can see but
  // cannot accept with the keyboard is a list you end up reaching for the mouse past.
  function onKeydown(event: KeyboardEvent) {
    if (candidates.length > 0) {
      if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
        event.preventDefault()
        const step = event.key === 'ArrowDown' ? 1 : candidates.length - 1
        highlighted = (highlighted + step) % candidates.length
        return
      }
      if (event.key === 'Enter' || event.key === 'Tab') {
        event.preventDefault()
        complete(candidates[highlighted])
        return
      }
      if (event.key === 'Escape') {
        event.preventDefault()
        mentionQuery = null
        return
      }
    }
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
      await chat.loadMembers()
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
  <header class="relative flex items-center justify-between gap-3 border-b border-ink-800 px-4 py-2.5">
    <div class="min-w-0">
      <h2 class="truncate text-sm text-ink-100">{room.name}</h2>
      <p class="truncate text-xs text-ink-500">
        <button
          onclick={() => (showMembers = !showMembers)}
          class="underline decoration-dotted underline-offset-2 transition hover:text-ink-300"
          aria-expanded={showMembers}
        >{room.member_count} member{room.member_count === 1 ? '' : 's'}</button>
        · started by {room.created_by}
      </p>
    </div>
    {#if showMembers}
      <Members members={chat.members} roomName={room.name} onClose={() => (showMembers = false)} />
    {/if}
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
      <div class="group flex gap-2.5">
        <Avatar
          src={message.author_departed ? undefined : message.author_avatar}
          name={message.author}
        />
        <div class="min-w-0 flex-1">
        <p class="flex items-baseline gap-2 text-xs">
          <!-- Role is carried by colour alone. Spelling the tier out beside every name
               turns a conversation into a staff list. -->
          <MemberName
            username={message.author}
            displayName={message.author_display_name}
            role={message.author_role}
            departed={message.author_departed}
          />
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
          {#if message.body}
          <p class="mt-0.5 whitespace-pre-wrap break-words text-sm text-ink-100">
            {#each parseMessage(message.body ?? '', message.mentions ?? []) as segment}{#if segment.href}<a
                  href={segment.href}
                  target="_blank"
                  rel="noopener noreferrer"
                  class="text-accent underline decoration-accent/40 underline-offset-2 hover:decoration-accent"
                  >{segment.text}</a
                >{:else if segment.mention}<!--
                  Both halves are needed. `mentions_me` says the message named my
                  account, which a name comparison cannot know once names can be changed
                  and reused; the name comparison says *this* @ is the one that meant me,
                  which a per-message flag cannot say when a message names two people.
                --><a
                  href="/u/{encodeURIComponent(segment.mention)}"
                  onclick={(e) => link(e, `/u/${encodeURIComponent(segment.mention!)}`)}
                  class="rounded px-0.5 font-medium hover:underline {message.mentions_me &&
                  segment.mention === me.username
                    ? 'bg-accent/20 text-accent'
                    : 'text-ink-300'}">{segment.text}</a
                >{:else}{segment.text}{/if}{/each}
          </p>
          {/if}
          {#if message.shares?.length}
            <div class="mt-1.5 flex flex-col gap-1.5">
              {#each message.shares as card, i (i)}
                <ShareCard {card} />
              {/each}
            </div>
          {/if}
        {/if}
        </div>
      </div>
    {/each}
  </div>

  {#if chat.messagesError}
    <p class="border-t border-ink-800 px-4 py-2 text-xs text-red-400">{chat.messagesError}</p>
  {/if}
  {#if error}
    <p class="border-t border-ink-800 px-4 py-2 text-xs text-red-400">{error}</p>
  {/if}

  <div class="border-t border-ink-800 px-4 py-3">
  {#if attachments.length}
    <ul class="mb-2 flex flex-wrap gap-1.5">
      {#each attachments as link (link.token)}
        <li class="flex items-center gap-1.5 rounded-lg border border-ink-700 bg-ink-900 py-1 pl-2.5 pr-1.5 text-xs text-ink-300">
          {#if link.password_protected}<span class="text-ink-500" title="Password protected"><LockIcon size={11} /></span>{/if}
          <span class="max-w-48 truncate">{link.title || 'Untitled'}</span>
          <button
            type="button"
            aria-label="Remove {link.title || 'this link'}"
            onclick={() => (attachments = attachments.filter((a) => a.token !== link.token))}
            class="px-1 text-ink-500 hover:text-ink-100"
          >×</button>
        </li>
      {/each}
    </ul>
  {/if}
  <form onsubmit={send} class="relative flex items-end gap-2">
    {#if candidates.length}
      <ul
        class="absolute bottom-full left-4 z-20 mb-1 w-48 overflow-hidden rounded-lg border border-ink-700 bg-ink-900 shadow-xl"
      >
        {#each candidates as name, i (name)}
          <li>
            <button
              type="button"
              onmousedown={(e) => {
                // mousedown, not click: click fires after the textarea has already lost
                // focus, and the caret position it needs is gone by then.
                e.preventDefault()
                complete(name)
              }}
              class="block w-full px-3 py-1.5 text-left text-xs transition {i === highlighted
                ? 'bg-accent/15 text-accent'
                : 'text-ink-300 hover:bg-ink-800'}"
            >@{name}</button>
          </li>
        {/each}
      </ul>
    {/if}
    <button
      type="button"
      onclick={() => (picking = true)}
      title="Attach your links"
      aria-label="Attach your links"
      class="shrink-0 rounded-lg border border-ink-700 px-2.5 py-2 text-xs text-ink-300 transition hover:border-ink-500"
    >+ Link</button>
    <textarea
      bind:this={composer}
      bind:value={draft}
      onkeydown={onKeydown}
      oninput={syncMentionQuery}
      onclick={syncMentionQuery}
      onblur={() => (mentionQuery = null)}
      rows="1"
      maxlength="4000"
      placeholder="Message {room.name}"
      class="max-h-32 min-h-[2.25rem] flex-1 resize-none rounded-lg border border-ink-700 bg-ink-900 px-3 py-2 text-sm outline-none focus:border-accent"
    ></textarea>
    <button
      disabled={sending || (!draft.trim() && attachments.length === 0)}
      class="shrink-0 rounded-lg bg-accent px-3 py-2 text-xs font-medium text-ink-950 transition hover:bg-accent-dim disabled:opacity-40"
    >Send</button>
  </form>
  </div>
</div>

{#if picking}
  <SharePicker
    title="Attach your links"
    confirmLabel="Attach"
    multiple
    onPick={(links) => {
      // Merged with anything already attached, once each, up to the ten a message holds.
      const known = new Set(attachments.map((a) => a.token))
      attachments = [...attachments, ...links.filter((l) => !known.has(l.token))].slice(0, 10)
      picking = false
    }}
    onCancel={() => (picking = false)}
  />
{/if}

{#if removing}
  {@const message = removing}
  <Confirm
    title="Remove this message?"
    body="It stays in the room as “Removed by {me.username}”, and its text is erased from the database. Backups taken before now still hold it until they expire. History is permanent here, so nothing is deleted quietly."
    confirmLabel="Remove"
    danger={true}
    onConfirm={() => remove(message)}
    onCancel={() => (removing = null)}
  />
{/if}
