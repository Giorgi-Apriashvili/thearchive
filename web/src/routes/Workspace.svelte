<script lang="ts">
  import { api, type Me } from '../lib/api'
  import { chat } from '../lib/chat.svelte'
  import { link, router } from '../lib/router.svelte'
  import Confirm from '../lib/Confirm.svelte'
  import Uploads from './Uploads.svelte'
  import Chat from './chat/Chat.svelte'

  let { me, onSignOut }: { me: Me; onSignOut: () => void } = $props()

  // The tab is the URL, not a variable: a room is then linkable, the back button works
  // between the two halves of the app, and a reload lands where you were.
  const tab = $derived(router.segments[0] === 'chat' ? 'chat' : 'uploads')

  // The room list keeps ticking whichever tab is showing: the unread badge on the Chat
  // tab is the only way someone on Uploads learns a message arrived. One request every
  // ten seconds against an indexed count is the price.
  $effect(() => chat.watch())

  let inviteCode = $state('')
  let confirmingInvite = $state(false)

  async function makeInvite() {
    confirmingInvite = false
    const result = await api.post<{ code: string }>('/api/invites')
    inviteCode = result.code
  }

  async function signOut() {
    await api.post('/api/auth/logout')
    onSignOut()
  }
</script>

<div class="mx-auto max-w-3xl px-4 py-10">
  <header class="flex items-baseline justify-between">
    <div>
      <h1>
        <!-- A real navigation, not router.go(): clicking this while already on "/"
             should reload the page, and the router short-circuits an unchanged path. -->
        <a href="/" class="wordmark text-3xl transition hover:opacity-80" title="Reload">
          Weekend<em>Archive</em>
        </a>
      </h1>
      <p class="mt-1 text-sm text-ink-500">Signed in as {me.username}</p>
    </div>
    <div class="flex items-center gap-4 text-xs">
      <!-- Minting is admin-only server-side; showing the button to everyone would just
           offer a 403. -->
      {#if me.role !== 'user'}
        <button class="text-ink-500 hover:text-ink-300" onclick={() => (confirmingInvite = true)}>Invite</button>
      {/if}
      {#if me.role === 'admin'}
        <a href="/admin" onclick={(e) => link(e, '/admin')} class="text-ink-500 hover:text-ink-300">
          Control panel
        </a>
      {/if}
      <a href="/account" onclick={(e) => link(e, '/account')} class="text-ink-500 hover:text-ink-300">
        Account
      </a>
      <button class="text-ink-500 hover:text-ink-300" onclick={signOut}>Sign out</button>
    </div>
  </header>

  {#if inviteCode}
    <div class="mt-4 flex items-center justify-between rounded-lg border border-ink-700 bg-ink-900 px-3 py-2">
      <code class="text-sm text-accent">{inviteCode}</code>
      <button
        class="text-xs text-ink-500 hover:text-ink-300"
        onclick={() => navigator.clipboard.writeText(inviteCode)}
      >Copy</button>
    </div>
  {/if}

  <nav class="mt-6 flex gap-1 border-b border-ink-800 text-sm">
    <a
      href="/"
      onclick={(e) => link(e, '/')}
      class="-mb-px border-b-2 px-3 py-2 transition {tab === 'uploads'
        ? 'border-accent text-ink-100'
        : 'border-transparent text-ink-500 hover:text-ink-300'}"
    >Uploads</a>
    <a
      href="/chat"
      onclick={(e) => link(e, '/chat')}
      class="-mb-px flex items-center gap-1.5 border-b-2 px-3 py-2 transition {tab === 'chat'
        ? 'border-accent text-ink-100'
        : 'border-transparent text-ink-500 hover:text-ink-300'}"
    >
      Chat
      {#if chat.totalUnread > 0 && tab !== 'chat'}
        <span
          class="tnum rounded-full px-1.5 text-[10px] font-medium {chat.totalMentions
            ? 'bg-accent text-ink-950'
            : 'bg-ink-700 text-ink-100'}"
          title={chat.totalMentions ? `${chat.totalMentions} mention you` : 'unread messages'}
        >
          {chat.totalMentions ? `@${chat.totalMentions}` : chat.totalUnread}
        </span>
      {/if}
    </a>
  </nav>

  <!-- Uploads stays mounted and is merely hidden. Unmounting it would throw away the
       queue, the progress and the upload ids of anything in flight, so switching to
       Chat mid-upload would silently cost someone a 3 GB video. Chat, by contrast, is
       unmounted when it is not shown, which is what stops its polling. -->
  <div class:hidden={tab !== 'uploads'}>
    <Uploads />
  </div>

  {#if tab === 'chat'}
    <Chat {me} />
  {/if}
</div>

{#if confirmingInvite}
  <Confirm
    title="Create an invite code?"
    body="Creates a code that lets one person register an account. It is valid for 14 days and works for whoever holds it, so treat it like a password — anyone it reaches can join."
    confirmLabel="Create invite"
    onConfirm={makeInvite}
    onCancel={() => (confirmingInvite = false)}
  />
{/if}
