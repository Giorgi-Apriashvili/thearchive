// Splits a message into plain runs, link runs and mention runs, so the component can
// render each with the right element and `{segment.text}` for the text itself.
//
// It deliberately returns data rather than markup. The tempting version of this — a
// regex replace producing `<a href="...">` fed to {@html} — hands every member of the
// archive a script injection into everyone else's browser, and it does so in the one
// place in the app where another person's arbitrary text reaches the DOM. Segments keep
// Svelte's escaping in play for both the text and the attribute.
//
// Only `http://` and `https://` are recognised. Requiring an explicit scheme is what
// makes this safe: there is no path here that produces a `javascript:` or `data:` href,
// because such a URL is never matched in the first place. The parse below re-checks the
// protocol anyway, since a lone regex is a thin thing to rest that on.

export interface Segment {
  text: string
  /** Present on a link run. */
  href?: string
  /** The username on a mention run. Runs with neither are plain text. */
  mention?: string
  // Light formatting, as flags on a run rather than markup — see parseBlocks.
  bold?: boolean
  italic?: boolean
  strike?: boolean
  code?: boolean
}

/** A paragraph, a `>` quote, or a fenced code block. */
export interface Block {
  kind: 'text' | 'quote' | 'code'
  /** Runs for text and quotes; a code block is shown exactly as typed, in `text`. */
  segments: Segment[]
  text: string
}

const URL_PATTERN = /https?:\/\/[^\s<>]+/gi

// Trailing punctuation almost always belongs to the sentence, not the URL: "see
// https://example.com." should not link the full stop. Closing brackets are only
// trimmed when unmatched, so a URL that genuinely contains a balanced pair survives —
// Wikipedia articles being the usual victim.
function trimTrailing(raw: string): string {
  let url = raw
  while (url.length > 0) {
    const last = url[url.length - 1]
    if ('.,;:!?\'"'.includes(last)) {
      url = url.slice(0, -1)
      continue
    }
    const closing = last === ')' ? '(' : last === ']' ? '[' : null
    if (closing !== null) {
      const opens = url.split(closing).length - 1
      const closes = url.split(last).length - 1
      if (closes > opens) {
        url = url.slice(0, -1)
        continue
      }
    }
    break
  }
  return url
}

function safeHref(candidate: string): string | null {
  try {
    const url = new URL(candidate)
    return url.protocol === 'http:' || url.protocol === 'https:' ? url.href : null
  } catch {
    return null
  }
}

function linkify(body: string): Segment[] {
  const segments: Segment[] = []
  let cursor = 0

  for (const match of body.matchAll(URL_PATTERN)) {
    const start = match.index ?? 0
    const url = trimTrailing(match[0])
    const href = safeHref(url)
    if (href === null) continue

    if (start > cursor) segments.push({ text: body.slice(cursor, start) })
    // The visible text is what they typed, not the normalised href: `new URL` adds a
    // trailing slash to a bare domain, and showing someone a link they did not write is
    // the beginning of how a link becomes untrustworthy.
    segments.push({ text: url, href })
    cursor = start + url.length
  }

  if (cursor < body.length) segments.push({ text: body.slice(cursor) })
  return segments
}

// Matches the server's idea of where an @name ends — see isNameChar in chat.cc. The two
// have to agree, or the client highlights something the server did not record, or misses
// something it did.
const MENTION_PATTERN = /(^|[^\w.-])@([\w.-]+)/g

// `mentions` is the list the server resolved at send time, not a guess. A name that is
// not in it stays plain text, which is what makes `@someone-who-left` read as the text
// it now is rather than as a live reference to nobody.
function markMentions(segment: Segment, mentions: string[]): Segment[] {
  if (segment.href !== undefined || segment.code || mentions.length === 0) return [segment]

  const out: Segment[] = []
  const body = segment.text
  let cursor = 0

  for (const match of body.matchAll(MENTION_PATTERN)) {
    const name = match[2].toLowerCase()
    if (!mentions.includes(name)) continue
    // match.index points at the boundary character, if any; the @ follows it.
    const at = (match.index ?? 0) + match[1].length
    // Spread, so a mention inside **bold** stays bold.
    if (at > cursor) out.push({ ...segment, text: body.slice(cursor, at) })
    out.push({ ...segment, text: `@${match[2]}`, mention: name })
    cursor = at + 1 + match[2].length
  }

  if (out.length === 0) return [segment]
  if (cursor < body.length) out.push({ ...segment, text: body.slice(cursor) })
  return out
}

/** Link and mention runs for one message body. */
export function parseMessage(body: string, mentions: string[] = []): Segment[] {
  return linkify(body).flatMap((segment) => markMentions(segment, mentions))
}

// ---- light formatting -------------------------------------------------------------
//
// The handful people type without thinking — **bold**, *italic* or _italic_, ~~struck~~,
// `code`, a ```fenced block```, and > quoted lines — and nothing else: no headings, no
// lists, no HTML, no images. Same rule as everything above: the result is data, flags on
// runs and blocks, so Svelte still escapes every character another person typed.
//
// Inline code is taken out first and left alone — formatting inside it is literal, which
// is the point of code. Emphasis is then found only in plain runs, after links: a URL with
// underscores or asterisks in it stays a URL. Underscores count only at word edges, so
// snake_case_names are not italicised.

const INLINE_CODE = /`([^`\n]+)`/g
const EMPHASIS: [RegExp, keyof Segment][] = [
  [/\*\*(?=\S)([\s\S]*?\S)\*\*/g, 'bold'],
  [/~~(?=\S)([\s\S]*?\S)~~/g, 'strike'],
  [/(?<![\w*])\*(?=[^\s*])([^*]*?[^\s*])\*(?![\w*])/g, 'italic'],
  [/(?<!\w)_(?=[^\s_])([^_]*?[^\s_])_(?!\w)/g, 'italic'],
]

function emphasise(segment: Segment): Segment[] {
  if (segment.href !== undefined || segment.code) return [segment]
  for (const [pattern, flag] of EMPHASIS) {
    pattern.lastIndex = 0
    const match = pattern.exec(segment.text)
    if (!match) continue
    const before = segment.text.slice(0, match.index)
    const after = segment.text.slice(match.index + match[0].length)
    const out: Segment[] = []
    if (before) out.push(...emphasise({ ...segment, text: before }))
    out.push(...emphasise({ ...segment, text: match[1], [flag]: true }))
    if (after) out.push(...emphasise({ ...segment, text: after }))
    return out
  }
  return [segment]
}

function inline(text: string, mentions: string[]): Segment[] {
  const out: Segment[] = []
  let cursor = 0
  const plain = (from: number, to: number) => {
    if (to <= from) return
    // Emphasis before mentions, so **bold @bob** is one bold run with a mention in it.
    for (const run of linkify(text.slice(from, to))) {
      for (const styled of emphasise(run)) out.push(...markMentions(styled, mentions))
    }
  }
  for (const match of text.matchAll(INLINE_CODE)) {
    const start = match.index ?? 0
    plain(cursor, start)
    out.push({ text: match[1], code: true })
    cursor = start + match[0].length
  }
  plain(cursor, text.length)
  return out
}

/** A message as blocks of formatted runs, for display. */
export function parseBlocks(body: string, mentions: string[] = []): Block[] {
  const blocks: Block[] = []
  const lines = body.split('\n')
  let text: string[] = []
  let quote: string[] = []
  const flush = () => {
    if (text.length) blocks.push({ kind: 'text', text: text.join('\n'), segments: inline(text.join('\n'), mentions) })
    if (quote.length) blocks.push({ kind: 'quote', text: quote.join('\n'), segments: inline(quote.join('\n'), mentions) })
    text = []
    quote = []
  }
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]
    if (line.trimStart().startsWith('```')) {
      // A fence runs to the next fence line, or to the end if it is never closed.
      const end = lines.findIndex((l, j) => j > i && l.trimStart().startsWith('```'))
      const stop = end === -1 ? lines.length : end
      flush()
      const opener = line.trimStart().slice(3).trim()
      const content = [...(opener ? [opener] : []), ...lines.slice(i + 1, stop)].join('\n')
      blocks.push({ kind: 'code', text: content, segments: [] })
      i = stop
      continue
    }
    if (/^>\s?/.test(line)) {
      if (text.length) flush()
      quote.push(line.replace(/^>\s?/, ''))
    } else {
      if (quote.length) flush()
      text.push(line)
    }
  }
  flush()
  return blocks
}
