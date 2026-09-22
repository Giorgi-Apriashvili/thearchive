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
  if (segment.href !== undefined || mentions.length === 0) return [segment]

  const out: Segment[] = []
  const body = segment.text
  let cursor = 0

  for (const match of body.matchAll(MENTION_PATTERN)) {
    const name = match[2].toLowerCase()
    if (!mentions.includes(name)) continue
    // match.index points at the boundary character, if any; the @ follows it.
    const at = (match.index ?? 0) + match[1].length
    if (at > cursor) out.push({ text: body.slice(cursor, at) })
    out.push({ text: `@${match[2]}`, mention: name })
    cursor = at + 1 + match[2].length
  }

  if (out.length === 0) return [segment]
  if (cursor < body.length) out.push({ text: body.slice(cursor) })
  return out
}

/** Link and mention runs for one message body. */
export function parseMessage(body: string, mentions: string[] = []): Segment[] {
  return linkify(body).flatMap((segment) => markMentions(segment, mentions))
}
