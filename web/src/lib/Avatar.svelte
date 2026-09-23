<script lang="ts">
  // A member's picture, or their initials when they have none.
  //
  // The fallback colour is derived from the username, so a person keeps the same colour
  // everywhere and between visits without anything being stored. It is set through
  // `style:` — applied via the CSSOM, which the Content-Security-Policy allows; an inline
  // style attribute written into markup would be refused.
  let {
    src,
    name,
    size = 'sm',
  }: {
    /** The URL the API gave, or nothing. */
    src?: string
    /** The username: stable, unlike a display name, so the colour does not change when
     *  someone renames themselves. */
    name: string
    size?: 'xs' | 'sm' | 'md' | 'lg'
  } = $props()

  const pixels = { xs: 20, sm: 28, md: 40, lg: 96 } as const

  // FNV-1a over the name, folded into a hue. Any stable hash would do; this one is short.
  const hue = $derived.by(() => {
    let h = 0x811c9dc5
    for (let i = 0; i < name.length; i++) {
      h ^= name.charCodeAt(i)
      h = Math.imul(h, 0x01000193)
    }
    return (h >>> 0) % 360
  })

  const initials = $derived(name.slice(0, 2).toUpperCase())

  // A broken picture — deleted between the page loading and the image being fetched —
  // falls back to initials rather than a broken-image icon.
  let failed = $state(false)
  $effect(() => {
    src
    failed = false
  })
</script>

{#if src && !failed}
  <img
    {src}
    alt=""
    width={pixels[size]}
    height={pixels[size]}
    loading="lazy"
    onerror={() => (failed = true)}
    class="shrink-0 rounded-full bg-ink-800 object-cover"
    style:width="{pixels[size]}px"
    style:height="{pixels[size]}px"
  />
{:else}
  <span
    aria-hidden="true"
    class="inline-flex shrink-0 select-none items-center justify-center rounded-full font-medium text-ink-950"
    style:width="{pixels[size]}px"
    style:height="{pixels[size]}px"
    style:font-size="{Math.round(pixels[size] * 0.4)}px"
    style:background-color="hsl({hue} 45% 62%)"
  >{initials}</span>
{/if}
