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
  const response = await fetch(path, {
    method,
    credentials: 'same-origin',
    headers: body === undefined ? headers : { 'Content-Type': 'application/json', ...headers },
    body: body === undefined ? undefined : JSON.stringify(body),
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
  del: <T>(path: string) => request<T>('DELETE', path),
}

export interface Me {
  username: string
  role: 'user' | 'privileged' | 'admin'
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
  /** Whole filesystem, shared with anything else on that partition — not a quota. */
  disk_total?: number
  disk_available?: number
}

/** How the signed-in account stands with a room. Every room is listed to everyone —
 *  `none` means visible but not enterable, which is what makes one askable-about. */
export type RoomState = 'none' | 'invited' | 'member' | 'declined'

export interface ChatRoom {
  id: number
  name: string
  /** The creator's name at creation time; it outlives their account. */
  created_by: string
  created_at: number
  state: RoomState
  is_creator: boolean
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
  /** Joined, or — for a pending invitation — invited. */
  since: number
  is_creator: boolean
  role: string
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
}

export interface ChatBlocks {
  rooms: { id: number; name: string }[]
  users: { id: number; username: string }[]
}

export interface CreatedShare {
  token: string
  url: string
  visibility: 'private' | 'public' 
  expires_at: number
  file_count: number
  total_bytes: number
}
