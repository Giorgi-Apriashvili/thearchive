<script lang="ts">
  import type { Me } from '../../lib/api'
  import { router, link } from '../../lib/router.svelte'
  import Users from './Users.svelte'
  import UserDetail from './UserDetail.svelte'
  import Invites from './Invites.svelte'
  import Shares from './Shares.svelte'
  import Overview from './Overview.svelte'

  let { me }: { me: Me } = $props()

  // ["admin"], ["admin","users"], ["admin","users","3"], ...
  const section = $derived(router.segments[1] ?? 'overview')
  const userId = $derived(router.segments[1] === 'users' ? router.segments[2] : undefined)

  const nav = [
    { id: 'overview', label: 'Overview', href: '/admin' },
    { id: 'users', label: 'Users', href: '/admin/users' },
    { id: 'invites', label: 'Invites', href: '/admin/invites' },
    { id: 'shares', label: 'Shares', href: '/admin/shares' },
  ]
</script>

<div class="mx-auto max-w-5xl px-4 py-10">
  <header class="flex items-baseline justify-between">
    <div>
      <a
        href="/"
        onclick={(e) => link(e, '/')}
        class="wordmark text-2xl text-ink-300 transition hover:text-ink-100"
      >Weekend<em>Archive</em></a>
      <p class="mt-1 text-sm text-ink-500">Control panel · {me.username}</p>
    </div>
    <a
      href="/"
      onclick={(e) => link(e, '/')}
      class="text-xs text-ink-500 hover:text-ink-300"
    >Back to uploads</a>
  </header>

  <div class="mt-8 gap-8 sm:flex">
    <!-- Horizontal on phones, a rail from the small breakpoint up. -->
    <nav class="flex gap-1 overflow-x-auto sm:w-44 sm:shrink-0 sm:flex-col">
      {#each nav as item (item.id)}
        <a
          href={item.href}
          onclick={(e) => link(e, item.href)}
          class="rounded-lg px-3 py-2 text-sm transition {section === item.id
            ? 'bg-ink-900 text-accent'
            : 'text-ink-500 hover:text-ink-300'}"
        >{item.label}</a>
      {/each}
    </nav>

    <main class="mt-6 min-w-0 flex-1 sm:mt-0">
      {#if section === 'users' && userId}
        <UserDetail id={userId} />
      {:else if section === 'users'}
        <Users />
      {:else if section === 'invites'}
        <Invites />
      {:else if section === 'shares'}
        <Shares />
      {:else}
        <Overview />
      {/if}
    </main>
  </div>
</div>
