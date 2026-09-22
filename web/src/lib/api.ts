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

export interface CreatedShare {
  token: string
  url: string
  visibility: 'private' | 'public' 
  expires_at: number
  file_count: number
  total_bytes: number
}
