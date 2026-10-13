<script lang="ts">
  import { api, ApiError, type ChatMessage, type ChatRoom, type Me, type ShareSummary } from '../../lib/api'
  import { chat } from '../../lib/chat.svelte'
  import { chatTime, clockTime, dayKey, dayLabel } from '../../lib/format'
  import { parseBlocks, type Segment } from '../../lib/message'
  import { link } from '../../lib/router.svelte'
  import Avatar from '../../lib/Avatar.svelte'
  import MemberName from '../../lib/MemberName.svelte'
  import ShareCard from '../../lib/ShareCard.svelte'
  import SharePicker from '../../lib/SharePicker.svelte'
  import LockIcon from '../../lib/LockIcon.svelte'
  import Confirm from '../../lib/Confirm.svelte'
  import Members from './Members.svelte'
  import RoomSettings from './RoomSettings.svelte'
  import EmojiPicker from './EmojiPicker.svelte'
  import { router } from '../../lib/router.svelte'
  import { tick } from 'svelte'

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
  let menuOpen = $state(false)
  let showSettings = $state(false)
  let confirmingLeave = $state(false)

  // Editing one of your own messages, in place.
  let editingId = $state<number | null>(null)
  let editText = $state('')
  let savingEdit = $state(false)

  function startEdit(message: ChatMessage) {
    editingId = message.id
    editText = message.body ?? ''
  }

  async function saveEdit() {
    if (editingId === null || savingEdit) return
    savingEdit = true
    error = ''
    try {
      await api.patch(`/api/chat/messages/${editingId}`, { body: editText.trim() })
      editingId = null
      await chat.reload()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not save the edit'
    } finally {
      savingEdit = false
    }
  }

  function onEditKeydown(event: KeyboardEvent) {
    if (event.key === 'Escape') {
      event.preventDefault()
      editingId = null
    } else if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault()
      void saveEdit()
    }
  }

  async function leave() {
    confirmingLeave = false
    try {
      await api.post(`/api/chat/rooms/${room.id}/leave`)
      router.go('/chat')
      await chat.loadRooms()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not leave the room'
    }
  }

  let scroller = $state<HTMLElement | null>(null)
  // Only follow new messages when the reader is already at the bottom. Yanking someone
  // out of the history they are reading because a message arrived is the single most
  // annoying thing a chat window can do.
  let pinned = $state(true)
  // The newest message seen while at the bottom: anything after it arrived while
  // scrolled up, and is what the jump button counts.
  let seenUpTo = $state(0)
  $effect(() => {
    const last = chat.messages[chat.messages.length - 1]
    if (pinned && last) seenUpTo = last.id
  })
  const newBelow = $derived(
    chat.messages.filter((m) => m.id > seenUpTo && m.author !== me.username).length,
  )

  function jumpToLatest() {
    if (!scroller) return
    scroller.scrollTo({ top: scroller.scrollHeight, behavior: 'smooth' })
    pinned = true
  }

  function styleOf(segment: Segment): string {
    return [
      segment.bold ? 'font-semibold' : '',
      segment.italic ? 'italic' : '',
      segment.strike ? 'line-through' : '',
    ].join(' ')
  }

  let showEmoji = $state(false)

  // Into the draft at the caret, keeping the composer focused for whatever comes next.
  function insertEmoji(emoji: string) {
    const field = composer
    const at = field?.selectionStart ?? draft.length
    const end = field?.selectionEnd ?? at
    draft = `${draft.slice(0, at)}${emoji}${draft.slice(end)}`
    const caret = at + emoji.length
    queueMicrotask(() => {
      field?.focus()
      field?.setSelectionRange(caret, caret)
    })
  }

  function onScroll() {
    if (!scroller) return
    pinned = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 60
    // Near the top with more behind it: fetch the page before, keeping what is on screen
    // where it is rather than letting the new rows push it down.
    if (scroller.scrollTop < 120 && chat.hasOlder && !chat.loadingOlder) void loadOlder()
  }

  async function loadOlder() {
    if (!scroller) return
    const fromBottom = scroller.scrollHeight - scroller.scrollTop
    await chat.loadOlder()
    await tick()
    if (scroller) scroller.scrollTop = scroller.scrollHeight - fromBottom
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
      // The tombstone arrives as a change to a message already on screen; asking now
      // rather than at the next tick keeps the text from lingering after the click.
      await chat.reload()
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not remove it'
    }
  }

  // The invite box and room settings are for whoever can actually use them; everyone
  // else gets no dead control. The server enforces the same rule.
  const canInvite = $derived(room.can_manage)
</script>

<!-- The runs of one formatted block: links, mentions, inline code and styled text. -->
{#snippet runs(segments: Segment[], message: ChatMessage)}{#each segments as segment}{#if segment.href}<a
                  href={segment.href}
                  target="_blank"
                  rel="noopener noreferrer"
                  class="text-accent underline decoration-accent/40 underline-offset-2 hover:decoration-accent {styleOf(segment)}"
                  >{segment.text}</a
                >{:else if segment.mention}<!--
                  Both halves are needed. `mentions_me` says the message named my
                  account, which a name comparison cannot know once names can be changed
                  and reused; the name comparison says *this* @ is the one that meant me,
                  which a per-message flag cannot say when a message names two people.
                --><a
                  href="/u/{encodeURIComponent(segment.mention)}"
                  onclick={(e) => link(e, `/u/${encodeURIComponent(segment.mention!)}`)}
                  class="rounded px-0.5 font-medium hover:underline {styleOf(segment)} {message.mentions_me &&
                  segment.mention === me.username
                    ? 'bg-accent/20 text-accent'
                    : 'text-ink-300'}">{segment.text}</a
                >{:else if segment.code}<code class="rounded bg-ink-800 px-1 py-0.5 font-mono text-[0.85em] text-ink-300">{segment.text}</code>{:else}<span class={styleOf(segment)}>{segment.text}</span>{/if}{/each}{/snippet}

<!-- Fixed height on phones, where the page scrolls; from md, the window's height less the
     header above it, so the conversation uses the screen rather than a box within it. -->
<div class="flex h-[34rem] flex-col rounded-xl border border-ink-800 md:h-[calc(100dvh-15rem)] md:min-h-[28rem]">
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
      {#if room.topic}
        <p class="mt-0.5 truncate text-xs text-ink-300" title={room.topic}>{room.topic}</p>
      {/if}
    </div>
    {#if showMembers}
      <Members members={chat.members} roomName={room.name} onClose={() => (showMembers = false)} />
    {/if}
    <div class="relative flex shrink-0 items-center gap-3">
      {#if canInvite}
        <button
          class="text-xs text-ink-500 hover:text-ink-300"
          onclick={() => (inviting = !inviting)}
        >{inviting ? 'Close' : 'Invite'}</button>
      {/if}
      <button
        class="rounded px-1.5 text-sm leading-none text-ink-500 hover:text-ink-300"
        aria-label="Room options"
        aria-expanded={menuOpen}
        onclick={() => (menuOpen = !menuOpen)}
      >⋯</button>
      {#if menuOpen}
        <button class="fixed inset-0 z-30 cursor-default" tabindex="-1" aria-label="Close menu" onclick={() => (menuOpen = false)}></button>
        <div class="absolute right-0 top-full z-40 mt-1 w-44 overflow-hidden rounded-lg border border-ink-700 bg-ink-900 py-1 text-xs shadow-xl">
          {#if room.can_manage}
            <button
              class="block w-full px-3 py-1.5 text-left text-ink-300 hover:bg-ink-800"
              onclick={() => {
                menuOpen = false
                showSettings = true
              }}
            >Room settings</button>
          {/if}
          <button
            class="block w-full px-3 py-1.5 text-left text-ink-300 hover:bg-ink-800 hover:text-red-300"
            onclick={() => {
              menuOpen = false
              confirmingLeave = true
            }}
          >Leave room</button>
        </div>
      {/if}
    </div>
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

  <div class="relative min-h-0 flex-1">
  <div bind:this={scroller} onscroll={onScroll} class="h-full overflow-y-auto px-4 py-3">
    {#if chat.hasOlder}
      <div class="text-center">
        <button
          class="text-xs text-ink-500 hover:text-ink-300 disabled:opacity-40"
          disabled={chat.loadingOlder}
          onclick={loadOlder}
        >{chat.loadingOlder ? 'Loading…' : 'Load earlier messages'}</button>
      </div>
    {/if}
    {#if chat.loadingMessages}
      <p class="text-xs text-ink-500">Loading…</p>
    {:else if chat.messages.length === 0}
      <p class="text-xs text-ink-500">Nothing here yet.</p>
    {/if}

    {#each chat.messages as message, i (message.id)}
      {@const prev = i > 0 ? chat.messages[i - 1] : null}
      {@const newDay = !prev || dayKey(prev.created_at) !== dayKey(message.created_at)}
      {@const firstUnread =
        chat.lastReadAtOpen > 0 &&
        message.id > chat.lastReadAtOpen &&
        (!prev || prev.id <= chat.lastReadAtOpen) &&
        message.author !== me.username}
      <!-- Consecutive messages from one person within five minutes read as one turn: the
           name and picture once, the rest beneath. A day boundary or the unread line
           starts a new turn regardless. -->
      {@const continued =
        !!prev &&
        !newDay &&
        !firstUnread &&
        prev.author === message.author &&
        !!prev.author_departed === !!message.author_departed &&
        message.created_at - prev.created_at < 300}
      {#if newDay}
        <div class="my-3 flex items-center gap-3 text-[11px] text-ink-500" role="separator">
          <span class="h-px flex-1 bg-ink-800"></span>
          <span>{dayLabel(message.created_at)}</span>
          <span class="h-px flex-1 bg-ink-800"></span>
        </div>
      {/if}
      {#if firstUnread}
        <div class="my-2 flex items-center gap-3 text-[11px] font-medium text-accent" role="separator">
          <span class="h-px flex-1 bg-accent/40"></span>
          <span>New messages</span>
          <span class="h-px flex-1 bg-accent/40"></span>
        </div>
      {/if}
      <div class="group relative flex gap-2.5 {continued ? 'mt-0.5' : newDay || firstUnread ? '' : 'mt-3'}">
        {#if continued}
          <span class="tnum w-7 shrink-0 pt-1 text-right text-[9px] text-ink-700 opacity-0 group-hover:opacity-100">
            {clockTime(message.created_at)}
          </span>
        {:else}
          <Avatar
            src={message.author_departed ? undefined : message.author_avatar}
            name={message.author}
          />
        {/if}
        <div class="min-w-0 flex-1">
        {#if !continued}
          <p class="flex items-baseline gap-2 pr-24 text-xs">
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
            <span class="tnum text-[10px] text-ink-700">{clockTime(message.created_at)}</span>
          </p>
        {/if}
        {#if !message.deleted && editingId !== message.id}
          <!-- Shown on hover where there is hover; always on touch screens, which have none. -->
          <span class="absolute right-0 top-0 flex gap-2 rounded bg-ink-950/80 px-1 opacity-0 transition group-hover:opacity-100 [@media(hover:none)]:opacity-100">
            {#if message.author === me.username && !message.author_departed}
              <button class="text-[10px] text-ink-500 hover:text-ink-300" onclick={() => startEdit(message)}>Edit</button>
            {/if}
            {#if message.author === me.username || me.role === 'admin'}
              <button class="text-[10px] text-ink-500 hover:text-red-400" onclick={() => (removing = message)}>Remove</button>
            {/if}
          </span>
        {/if}
        {#if message.deleted}
          <!-- A tombstone, not a gap: quietly reflowing a conversation around what was
               taken out is its own kind of dishonesty. -->
          <p class="mt-0.5 text-sm italic text-ink-700">Removed by {message.deleted_by}</p>
        {:else if editingId === message.id}
          <div class="mt-1 max-w-[70ch]">
            <!-- svelte-ignore a11y_autofocus -->
            <textarea
              bind:value={editText}
              onkeydown={onEditKeydown}
              autofocus
              rows="2"
              maxlength="4000"
              class="w-full resize-y rounded-lg border border-accent/60 bg-ink-900 px-3 py-2 text-sm outline-none"
            ></textarea>
            <p class="mt-1 flex items-center gap-3 text-[11px] text-ink-500">
              <button class="text-accent hover:text-accent-dim disabled:opacity-40" disabled={savingEdit} onclick={saveEdit}>
                {savingEdit ? 'Saving…' : 'Save'}
              </button>
              <button class="hover:text-ink-300" onclick={() => (editingId = null)}>Cancel</button>
              <span>Enter to save · Esc to cancel</span>
            </p>
          </div>
        {:else}
          <!-- Rendered as blocks and runs, never {@html}: parseBlocks returns data precisely
               so that Svelte keeps escaping both the text and the href — this is the one
               place in the app where another person's arbitrary text reaches the DOM. -->
          {#if message.body}
            <!-- 70ch: past that, lines get hard to follow, however wide the window. -->
            <div class="mt-0.5 max-w-[70ch] space-y-1 text-sm text-ink-100 [overflow-wrap:anywhere]">
              {#each parseBlocks(message.body, message.mentions ?? []) as block, b (b)}
                {#if block.kind === 'code'}
                  <pre class="overflow-x-auto rounded-lg border border-ink-800 bg-ink-900 px-3 py-2 font-mono text-xs text-ink-300">{block.text}</pre>
                {:else if block.kind === 'quote'}
                  <blockquote class="whitespace-pre-wrap border-l-2 border-ink-700 pl-3 text-ink-300">{@render runs(block.segments, message)}</blockquote>
                {:else}
                  <p class="whitespace-pre-wrap">{@render runs(block.segments, message)}</p>
                {/if}
              {/each}
            </div>
          {/if}
          {#if message.edited_at}
            <span class="text-[10px] text-ink-700" title="Edited {chatTime(message.edited_at)}">(edited)</span>
          {/if}
          {#if message.shares?.length}
            <div class="mt-1.5 flex flex-col gap-1.5">
              {#each message.shares as card, c (c)}
                <ShareCard {card} />
              {/each}
            </div>
          {/if}
        {/if}
        </div>
      </div>
    {/each}
  </div>
  {#if !pinned && chat.messages.length}
    <!-- Scrolled back: a way down again, saying whether anything arrived meanwhile. -->
    <button
      onclick={jumpToLatest}
      class="absolute bottom-3 right-4 rounded-full border border-ink-700 bg-ink-900 px-3 py-1.5 text-xs shadow-lg transition hover:border-ink-500 {newBelow
        ? 'text-accent'
        : 'text-ink-300'}"
    >↓ {newBelow ? `${newBelow} new` : 'Latest'}</button>
  {/if}
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
    <div class="relative shrink-0">
      <button
        type="button"
        onclick={() => (showEmoji = !showEmoji)}
        title="Emoji"
        aria-label="Emoji"
        aria-expanded={showEmoji}
        class="rounded-lg border border-ink-700 px-2.5 py-2 text-ink-300 transition hover:border-ink-500"
      >
        <!-- Drawn, not ☺: a character depends on the device having a font for it, and
             without one this button would be an empty box. -->
        <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" aria-hidden="true" class="block">
          <circle cx="8" cy="8" r="6.3" />
          <path d="M5.4 9.6c.7 1 1.6 1.5 2.6 1.5s1.9-.5 2.6-1.5" stroke-linecap="round" />
          <circle cx="6" cy="6.6" r=".6" fill="currentColor" stroke="none" />
          <circle cx="10" cy="6.6" r=".6" fill="currentColor" stroke="none" />
        </svg>
      </button>
      {#if showEmoji}
        <EmojiPicker onPick={insertEmoji} onClose={() => (showEmoji = false)} />
      {/if}
    </div>
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
    title={message.author === me.username ? 'Remove your message?' : 'Remove this message?'}
    body="It stays in the room as “Removed by {me.username}”, so the conversation keeps its shape, and its text is erased from the database. Backups taken before now still hold it until they expire."
    confirmLabel="Remove"
    danger={true}
    onConfirm={() => remove(message)}
    onCancel={() => (removing = null)}
  />
{/if}

{#if confirmingLeave}
  <Confirm
    title="Leave “{room.name}”?"
    body="You stop seeing its messages and it stops notifying you. What you said stays. To come back, someone who runs the room has to invite you again."
    confirmLabel="Leave room"
    danger={true}
    onConfirm={leave}
    onCancel={() => (confirmingLeave = false)}
  />
{/if}

{#if showSettings}
  <RoomSettings {room} onClose={() => (showSettings = false)} />
{/if}
