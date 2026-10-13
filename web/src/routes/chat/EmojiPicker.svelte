<script lang="ts">
  // A short, curated set rather than the whole of Unicode: the faces and signs a group of
  // friends actually reaches for, in a grid that fits above the composer. No library and
  // no remote sprite — the emoji are the device's own, which is also why they look like
  // the rest of what that device shows.
  import { fade } from 'svelte/transition'

  let { onPick, onClose }: { onPick: (emoji: string) => void; onClose: () => void } = $props()

  const groups: { label: string; emoji: string[] }[] = [
    { label: 'Faces', emoji: ['😀', '😄', '😂', '🤣', '😊', '😍', '🥰', '😘', '😎', '🤩', '🥳', '😏', '😅', '🙃', '😉', '🤔', '🤨', '😐', '😴', '🤤', '😬', '😮', '😱', '😢', '😭', '😤', '😡', '🤯', '🥶', '🤢', '🤡', '💀'] },
    { label: 'Hands', emoji: ['👍', '👎', '👏', '🙌', '🙏', '🤝', '👋', '✌️', '🤞', '👌', '🤙', '💪', '👀', '🫡'] },
    { label: 'Hearts', emoji: ['❤️', '🧡', '💛', '💚', '💙', '💜', '🖤', '💔', '🔥', '✨', '⭐', '💯'] },
    { label: 'Things', emoji: ['🎉', '🎂', '🍕', '🍺', '🍷', '☕', '🍖', '🎵', '📸', '🎮', '⚽', '🚗', '✈️', '🏔️', '🏖️', '⛺', '🌙', '☀️', '🌧️', '❄️'] },
    { label: 'Signs', emoji: ['✅', '❌', '❓', '❗', '⚠️', '👉', '🆗', '💤'] },
  ]
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && onClose()} />

<button class="fixed inset-0 z-30 cursor-default" tabindex="-1" aria-label="Close emoji" onclick={onClose}></button>
<div
  role="dialog"
  aria-label="Emoji"
  class="absolute bottom-full left-0 z-40 mb-2 max-h-72 w-72 overflow-y-auto rounded-xl border border-ink-700 bg-ink-900 p-2 shadow-xl"
  transition:fade={{ duration: 100 }}
>
  {#each groups as group (group.label)}
    <p class="px-1 pb-1 pt-1.5 text-[10px] uppercase tracking-wide text-ink-500">{group.label}</p>
    <div class="grid grid-cols-8 gap-0.5">
      {#each group.emoji as emoji (emoji)}
        <button
          type="button"
          onmousedown={(e) => {
            // mousedown, as with @-completion: the composer must not lose its caret first.
            e.preventDefault()
            onPick(emoji)
          }}
          class="rounded-md p-1 text-lg leading-none transition hover:bg-ink-800"
          aria-label={emoji}
        >{emoji}</button>
      {/each}
    </div>
  {/each}
</div>
