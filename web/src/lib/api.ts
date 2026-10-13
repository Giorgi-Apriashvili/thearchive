// Thin wrapper over fetch. The session is an HttpOnly cookie, so there is no token to
// carry around — `credentials: same-origin` is the whole of the auth story here.

export class ApiError extends Error {
  constructor(
    public status: number,
    message: string,
    /** Machine-readable discriminator, where the status alone is ambiguous — a 401 may
     *  mean "needs a password" or "members only". */
    public reason?: string,
  ) {
    super(message)
  }
}

async function request<T>(method: string, path: string, body?: unknown, headers: Record<string, string> = {}): Promise<T> {
  // A Blob — a picture being uploaded — goes as it is, with the browser's own type;
  // everything else is JSON.
  const raw = body instanceof Blob
  const response = await fetch(path, {
    method,
    credentials: 'same-origin',
    headers: body === undefined || raw ? headers : { 'Content-Type': 'application/json', ...headers },
    body: body === undefined ? undefined : raw ? body : JSON.stringify(body),
  })

  if (response.status === 204) return undefined as T

  let payload: any = null
  try {
    payload = await response.json()
  } catch {
    // A non-JSON body on an error path is still an error; fall through to the status.
  }

  if (!response.ok) {
    throw new ApiError(
      response.status,
      payload?.error ?? `request failed (${response.status})`,
      payload?.reason,
    )
  }
  return payload as T
}

export const api = {
  get: <T>(path: string, headers?: Record<string, string>) => request<T>('GET', path, undefined, headers),
  post: <T>(path: string, body?: unknown, headers?: Record<string, string>) => request<T>('POST', path, body, headers),
  patch: <T>(path: string, body?: unknown) => request<T>('PATCH', path, body),
  put: <T>(path: string, body?: unknown) => request<T>('PUT', path, body),
  del: <T>(path: string) => request<T>('DELETE', path),
}

export interface Me {
  username: string
  role: 'user' | 'privileged' | 'admin'
  /** Present only when set. */
  display_name?: string
  /** An opaque URL from the server; present only when a picture is set. */
  avatar?: string
}

/** A member's profile, as other members see it. */
export interface Profile {
  username: string
  role: string
  joined: number
  is_me: boolean
  display_name?: string
  bio?: string
  avatar?: string
  invited_by?: string
  /** Rooms the viewer and this member are both in — never the rest of theirs, since
   *  who is in a room is visible only to that room's members. */
  shared_rooms: { id: number; name: string }[]
}

export interface ShareFile {
  id: number
  /** Present only when there is something to show: a rendered thumbnail, or a
   *  browser-playable video. Absent means the row gets no preview affordance. */
  preview?: 'image' | 'video' 
  filename: string
  size: number
  content_type: string
  relative_path?: string
  client_mtime?: number
  uploaded_by?: string
}

export interface ShareDetail {
  token: string
  /** Signed in: names on the page can link to profiles. Profiles are for members, so for
   *  someone holding only the link they stay plain text. */
  viewer_is_member?: boolean
  visibility: 'private' | 'public' 
  title: string
  created_at: number
  expires_at: number
  download_count: number
  max_downloads?: number
  files: ShareFile[]
}

export interface ShareSummary {
  token: string
  url: string
  title: string
  created_at: number
  expires_at: number
  download_count: number
  max_downloads?: number
  password_protected: boolean
  visibility: 'private' | 'public' 
  file_count: number
  total_bytes: number
}

/** An account's storage as the control panel sees it. `limit` null is no limit; `custom`
 *  means set for this account rather than following its role. */
export interface AccountStorage {
  used: number
  limit: number | null
  custom: boolean
}

export interface StorageInfo {
  stored_bytes: number
  blob_count: number
  /** Files in live shares, counted individually. */
  shared_logical_bytes: number
  /** Distinct blobs backing those files, counted once each. The gap is what dedupe saved. */
  shared_stored_bytes: number
  incoming_bytes: number
  my_shares?: number
  my_bytes?: number
  /** What counts against your storage limit: each file once, unfinished uploads in full. */
  my_usage?: number
  /** Your limit in bytes; null when you have none. */
  my_limit?: number | null
  /** Whole filesystem, shared with anything else on that partition — not a quota. */
  disk_total?: number
  disk_available?: number
}

/** How the signed-in account stands with a room. Every room is listed to everyone —
 *  `none` means visible but not enterable, which is what makes one askable-about. */
export type RoomState = 'none' | 'invited' | 'member' | 'declined' | 'left' | 'removed'

export interface ChatRoom {
  id: number
  name: string
  /** The creator's name at creation time; it outlives their account. */
  created_by: string
  created_at: number
  state: RoomState
  is_creator: boolean
  /** Whether room settings apply to me: the creator while in it, or an administrator. */
  can_manage: boolean
  topic?: string
  member_count: number
  /** Only when `state` is `invited`. */
  invited_by?: string
  /** Only when `state` is `member`. */
  unread?: number
  /** Unread messages that named me. A subset of `unread`, counted separately so the
   *  badge can say "you were asked something" rather than "something happened". */
  mentions_unread?: number
}

export interface RoomMember {
  username: string
  display_name?: string
  avatar?: string
  /** Joined, or — for a pending invitation — invited. */
  since: number
  is_creator: boolean
  role: string
  /** How far a member has read, for "seen by". Members only, not invitations. */
  read_up_to?: number
}

/** A pinned message, as the room's pin list shows it. */
export interface PinnedMessage {
  id: number
  author: string
  author_display_name?: string
  excerpt: string
  created_at: number
  pinned_at: number
  pinned_by: string
}

export interface RoomMembers {
  members: RoomMember[]
  /** Outstanding invitations. Declines are never listed: whether someone turned an
   *  invitation down is their business, not a status the room displays about them. */
  invited: RoomMember[]
}

export interface ChatMessage {
  id: number
  author: string
  created_at: number
  /** Absent on a removed message — the server never sends the text again. */
  body?: string
  deleted?: boolean
  deleted_by?: string
  /** The author's account has since been deleted; the message stays attributed. */
  author_departed?: boolean
  /** The author's role *now*, not at send time — it is an identity badge rather than
   *  history. Absent when the account is gone. */
  author_role?: string
  /** Usernames this message named with @, as the server resolved them at send time.
   *  The client highlights these rather than re-deriving them from the text, so what is
   *  highlighted is exactly what a notifier would act on. */
  mentions?: string[]
  /** The author's display name and picture, live like the role. `author` stays the
   *  username, which is what attributes the message. */
  author_display_name?: string
  author_avatar?: string
  /** Whether this message named *me*, answered by account rather than by comparing
   *  names — a name can be changed and then taken by someone else, so an old `@bob`
   *  may not mean today's bob. */
  mentions_me?: boolean
  /** Set when the author has edited it; the text served is the edited one. */
  edited_at?: number
  /** A removed message to show as nothing at all: its author removed it, or an
   *  administrator cleared the marker their removal left. */
  hidden?: boolean
  /** What this message answers. `deleted` once that has been removed. */
  reply_to?: {
    id: number
    author?: string
    author_display_name?: string
    excerpt?: string
    has_links?: boolean
    deleted?: boolean
  }
  /** One entry per emoji, in the order first used. */
  reactions?: { emoji: string; count: number; mine: boolean; names: string[] }[]
  pinned?: { by: string; at: number }
  /** Links attached to the message, resolved when it was read. */
  shares?: ShareCard[]
}

/** A link as a card, in chat and the inbox. Resolved when fetched, so it says whether the
 *  link works *now*; `token` is present only while it does. */
export interface ShareCard {
  state: 'live' | 'expired' | 'revoked' | 'used_up' | 'gone'
  title?: string
  token?: string
  file_count?: number
  total_bytes?: number
  expires_at?: number
  password_protected?: boolean
  visibility?: 'private' | 'public'
  /** Thumbnail URLs; always empty for a password-protected link. */
  previews?: string[]
  /** Photos posted straight into chat: shown as the photos, not as a link card. */
  inline?: boolean
  /** Every photo of an inline card, while its link works. `thumb` only where a preview
   *  exists — a video without one shows as a play tile. */
  photos?: { id: number; kind: 'image' | 'video'; thumb?: string }[]
}

/** A link a member sent you. */
/** Who downloaded one of your links, as its owner sees it. `files` holds what each
 *  member took, latest first; null stands for the whole link as a zip. */
export interface ShareDownloads {
  people: {
    username: string
    display_name?: string
    avatar?: string
    role: string
    downloads: number
    last_at: number
    files: (string | null)[]
  }[]
  anonymous: { downloads: number; last_at?: number }
}

/** Something waiting for you, as GET /api/notifications reports it. `key` is stable per
 *  item, so successive polls (and several tabs) can tell what is new. */
export interface NotificationItem {
  key: string
  kind: 'mention' | 'link' | 'invitation'
  at: number
  from?: { username: string; display_name?: string }
  room?: { id: number; name: string }
  excerpt?: string
  titles?: string[]
  note?: string
}

export interface Notifications {
  counts: { mentions: number; links: number; invitations: number }
  items: NotificationItem[]
}

/** One send: the links someone sent you together, with their note once. */
export interface InboxItem {
  id: string
  sent_at: number
  seen: boolean
  note?: string
  sender: { username: string; role: string; display_name?: string; avatar?: string }
  cards: ShareCard[]
}

export interface ChatBlocks {
  rooms: { id: number; name: string }[]
  users: { id: number; username: string }[]
}

/** What the privacy notice needs from the deployment. Every retention figure is the one
 *  the server enforces, not a copy — see server/src/privacy.cc. */
export interface PrivacyInfo {
  /** Null when this deployment has not named anyone. */
  operator: { name: string; contact: string } | null
  hosting_location?: string
  retention: {
    session_days: number
    link_default_days: number
    link_max_days: number
    sweep_minutes: number
    unshared_upload_hours: number
    backup_days: number
    /** How long access records are kept: the web server's log and the activity log. */
    access_days: number
    /** Whether each backup is also kept, encrypted, off the server. */
    backup_offsite: boolean
    /** How much longer the off-site storage's own snapshots can keep a copy. */
    backup_offsite_extra_days: number
  }
}

export interface CreatedShare {
  token: string
  url: string
  visibility: 'private' | 'public' 
  expires_at: number
  file_count: number
  total_bytes: number
}
