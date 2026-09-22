<script lang="ts">
  import { fade, scale } from 'svelte/transition'

  let {
    title,
    body,
    confirmLabel = 'Confirm',
    danger = false,
    onConfirm,
    onCancel,
  }: {
    title: string
    body: string
    confirmLabel?: string
    danger?: boolean
    onConfirm: () => void
    onCancel: () => void
  } = $props()

  let confirmButton = $state<HTMLButtonElement | null>(null)

  // Focus the action rather than the dialog, so Enter confirms and Tab stays inside a
  // two-button dialog without needing a full focus trap.
  $effect(() => confirmButton?.focus())

  $effect(() => {
    const previous = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    return () => {
      document.body.style.overflow = previous
    }
  })
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && onCancel()} />

<div
  class="fixed inset-0 z-50 flex items-center justify-center p-4"
  transition:fade={{ duration: 120 }}
>
  <!-- A real button behind the panel, so clicking away cancels without putting a
       handler on a non-interactive element. -->
  <button
    onclick={onCancel}
    aria-label="Cancel"
    tabindex="-1"
    class="absolute inset-0 cursor-default bg-ink-950/80 backdrop-blur-sm"
  ></button>

  <div
    role="alertdialog"
    aria-modal="true"
    aria-label={title}
    class="relative z-10 w-full max-w-sm rounded-xl border border-ink-700 bg-ink-900 p-5"
    transition:scale={{ duration: 120, start: 0.97 }}
  >
    <h2 class="text-sm font-medium text-ink-100">{title}</h2>
    <p class="mt-2 text-xs leading-relaxed text-ink-500">{body}</p>

    <div class="mt-5 flex justify-end gap-2">
      <button
        onclick={onCancel}
        class="rounded-lg border border-ink-700 px-3 py-1.5 text-xs text-ink-300 hover:border-ink-500"
      >Cancel</button>
      <button
        bind:this={confirmButton}
        onclick={onConfirm}
        class="rounded-lg px-3 py-1.5 text-xs font-medium transition {danger
          ? 'border border-red-900 text-red-300 hover:bg-red-950'
          : 'bg-accent text-ink-950 hover:bg-accent-dim'}"
      >{confirmLabel}</button>
    </div>
  </div>
</div>
