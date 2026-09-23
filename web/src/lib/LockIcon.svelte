<script lang="ts">
  // A padlock, for password-protected links. An inline SVG rather than the 🔒 emoji,
  // which depends on the device having an emoji font — without one it renders as an
  // empty box — and ignores the surrounding text colour.
  let { size = 12 }: { size?: number } = $props()
</script>

<svg
  width={size}
  height={size}
  viewBox="0 0 16 16"
  fill="none"
  stroke="currentColor"
  stroke-width="1.6"
  aria-hidden="true"
  class="inline-block shrink-0"
>
  <rect x="3" y="7" width="10" height="7" rx="1.5" />
  <path d="M5.5 7V5a2.5 2.5 0 0 1 5 0v2" />
</svg>
