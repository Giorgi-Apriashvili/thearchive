// Role → colour, in one place, because a name coloured one way in the message list and
// another in the member popover is worse than no colour at all.
//
// The colours themselves are theme tokens; see app.css for why these three hues.

export type Role = 'user' | 'privileged' | 'admin'

/** Tailwind text colour for a role. An empty or unrecognised role — a departed author,
 *  or a value this build does not know about — falls back to plain body text rather
 *  than guessing at a tier. */
export function roleColor(role: string | undefined): string {
  switch (role) {
    case 'admin':
      return 'text-role-admin'
    case 'privileged':
      return 'text-role-privileged'
    case 'user':
      return 'text-role-user'
    default:
      return 'text-ink-300'
  }
}

/** The word shown beside a name, or null for ordinary members — labelling everyone
 *  would make the label mean nothing. */
export function roleLabel(role: string | undefined): string | null {
  return role === 'admin' || role === 'privileged' ? role : null
}
