<script lang="ts">
  import { api, ApiError } from './api'
  import Confirm from './Confirm.svelte'

  let {
    visibility,
    endpoint,
    onChanged,
  }: {
    visibility: 'private' | 'public'
    /** Where to PATCH: /api/shares/{token} for your own, /api/admin/shares/{token}
     *  for an administrator acting on someone else's. */
    endpoint: string
    onChanged: (next: 'private' | 'public') => void
  } = $props()

  let pending = $state<'private' | 'public' | null>(null)
  let busy = $state(false)
  let error = $state('')

  const isPublic = $derived(visibility === 'public')

  async function apply(next: 'private' | 'public') {
    pending = null
    busy = true
    error = ''
    try {
      await api.patch(endpoint, { visibility: next })
      onChanged(next)
    } catch (e) {
      error = e instanceof ApiError ? e.message : 'could not change visibility'
    } finally {
      busy = false
    }
  }

  // The two directions are different mistakes, so they get different warnings rather
  // than one generic "are you sure".
  const prompt = $derived(
    pending === 'public'
      ? {
          title: 'Make this link public?',
          body:
            'Anyone holding the link will be able to open it, account or not. ' +
            'It stops being limited to members here.',
          confirmLabel: 'Make public',
          danger: false,
        }
      : {
          title: 'Make this link members only?',
          body:
            'Anyone without an account will stop being able to open it — including ' +
            'people you already sent it to. It does not undo downloads they have ' +
            'already made, and a preview they have loaded may stay in their browser ' +
            'cache for up to an hour.',
          confirmLabel: 'Make private',
          danger: true,
        },
  )
</script>

<button
  disabled={busy}
  onclick={() => (pending = isPublic ? 'private' : 'public')}
  title={isPublic ? 'Anyone with the link' : 'Members only'}
  class="shrink-0 text-xs transition disabled:opacity-40 {isPublic
    ? 'text-accent hover:text-accent-dim'
    : 'text-ink-500 hover:text-ink-300'}"
>
  {isPublic ? 'Public' : 'Members only'}
</button>

{#if error}
  <span class="text-xs text-red-400">{error}</span>
{/if}

{#if pending}
  <Confirm
    title={prompt.title}
    body={prompt.body}
    confirmLabel={prompt.confirmLabel}
    danger={prompt.danger}
    onConfirm={() => apply(pending!)}
    onCancel={() => (pending = null)}
  />
{/if}
