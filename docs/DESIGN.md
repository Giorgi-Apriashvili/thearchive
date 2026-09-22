# TheArchive — Design

A self-hosted file drop for a small, closed group of friends. Upload files, get a link,
share the link, recipient downloads. Links expire (default 30 days). Storage reclaims itself.

Explicitly **not**: a photo gallery, a backup target, or a public file host.

## Constraints that shaped this

| Constraint | Consequence |
| --- | --- |
| A single fixed-size volume | Expiry + GC are load-bearing, not a nice-to-have |
| Content is ephemeral (30d default) | Storage is a *flow*, not a stock — it reaches steady state |
| Dedicated host, public IPv4 | Direct exposure; no tunnel needed, but abuse policy matters |
| ~6 known users | SQLite is sufficient; no Postgres, no horizontal scale |
| C++ backend by preference | Hand-roll as little protocol as possible; let the edge do TLS/HTTP |

## Architecture

```
                    :443
  browser  ─────►  Caddy  ─┬─►  static SPA          (built assets, served directly)
                           │
                           └─►  127.0.0.1:8080  ──►  Drogon (C++20)
                                                       │
                                            ┌──────────┴──────────┐
                                            │                     │
                                      SQLite (WAL)          blob store
```

**Caddy terminates TLS** and auto-renews Let's Encrypt certs. This is deliberate: TLS
termination and HTTP parsing are the two places where hand-written C++ gets you owned.
The C++ process only ever sees plaintext HTTP/1.1 on loopback.

### Why Drogon

Async C++17/20 framework with routing, filters, sessions, range-aware file responses, and
a SQLite ORM. The alternative — Boost.Beast — gives correct HTTP parsing but no routing,
no multipart, no sessions, so you'd rebuild all of it. Drogon is pulled in via CMake
`FetchContent`, so the dev build and the server build are byte-identical in their deps.

## Upload: the tus protocol

Phone uploads over mobile data fail constantly. A plain `multipart/form-data` POST of a
3 GB video means one dropped connection wastes the whole transfer. So uploads use
[tus](https://tus.io) — a small, well-specified resumable upload protocol:

| Method | Path | Purpose |
| --- | --- | --- |
| `OPTIONS` | `/files/` | Capability discovery |
| `POST` | `/files/` | Create upload, returns `Location: /files/<id>` |
| `HEAD` | `/files/<id>` | Returns `Upload-Offset` — where to resume from |
| `PATCH` | `/files/<id>` | Append bytes at `Upload-Offset` |

The client side is `tus-js-client`, which handles retry and resume. Chunks are capped at
16 MiB, comfortably under the server's 64 MiB request-body limit — left to its default the
client sends the whole file in one `PATCH`, which fails on the first large video.

## Storage layout

```
<data dir>/
├── incoming/                  in-progress uploads, named by upload id
├── blobs/ab/cd/<sha256>       completed, content-addressed
├── db/archive.db              SQLite (WAL mode)
└── compose/                   docker-compose.yml, Caddyfile, .env
```

Blobs are **content-addressed by SHA-256**, which buys deduplication for free. When four
people upload the same 800 MB video from the same night, it occupies 800 MB once. A
refcount in the DB tracks how many live shares point at each blob.

## Data model

```sql
users(id, username, password_hash, created_at, quota_bytes,
      role, disabled_at)                                   -- user | privileged | admin
invites(code, created_by, used_by, created_at, expires_at)
sessions(token, user_id, created_at, expires_at)          -- token stored hashed

blobs(sha256 PK, size, refcount, created_at,
      content_type, first_uploader, last_referenced_at, pinned)

shares(id, token UNIQUE, owner_id, title, created_at, expires_at,
       password_hash NULL, max_downloads NULL, download_count,
       deleted_at, released_at)

share_files(id, share_id, blob_sha256, filename, content_type, size,
            uploaded_by, client_mtime, relative_path)

uploads(id PK, owner_id, filename, content_type, total_size, offset_bytes,
        created_at, expires_at, blob_sha256, completed_at,
        client_mtime, relative_path, user_agent)
```

A **share** holds one or more files — a whole night goes out as one link. `token` is 128
random bits, base64url-encoded, giving URLs like `https://<host>/d/8fK2mQ...`.

### Metadata, and why there is more of it than this feature needs

The columns above carry more than an ephemeral file drop requires, because most of it is
only knowable at upload time and unrecoverable afterwards. Keeping an archival mode open
costs a few columns now and a migration over live data later.

The split is deliberate:

- **`blobs` holds content-level facts** — what the bytes are, who first uploaded them,
  when they were first seen. True regardless of which share delivered them, and therefore
  worth outliving every share that pointed at them.
- **`share_files` holds contribution-level facts** — who put this file in *this* share,
  under what name, and what its timestamp was on their device. When four people upload
  the same video from the same night, there is one blob and four contributions.

`client_mtime` is the file's own modification time, not the upload time; for a backup
that distinction is the whole point. `content_type` on a blob is **sniffed from magic
bytes**, never the client's claim — it is what the download endpoint serves, and echoing
back a client-chosen type is a stored-XSS vector.

**`pinned`** is the hook that makes an archival mode possible without reworking expiry: a
pinned blob is never collected, however many shares referencing it have lapsed.

### Expiry and garbage collection

An in-process timer sweeps every 15 minutes, and once at startup so a restart clears
whatever accumulated while the process was down. Each step is separately transactional:

1. **Release lapsed shares** — expired or revoked. Decrement each referenced blob's
   refcount, delete the `share_files` rows, stamp `released_at`. The share row itself is
   kept as a record of what was shared and when.
2. **Expire uploads** — abandoned mid-transfer, or completed but never shared.
3. **Delete orphan blobs** — refcount 0, not pinned, past the grace period.
4. **Remove stray `incoming/` files** with no surviving upload row.
5. **Purge expired sessions.**

Two ordering constraints matter, and both are tested:

**`released_at` is what makes the sweep idempotent.** Without it, "expired" and "expired
and already accounted for" are indistinguishable, and a second pass decrements every
refcount again — silently deleting live blobs.

**The grace period is not tidiness.** A blob sits at refcount 0 between its upload
completing and a share being created. A sweep without a grace window would delete files
out from under a user mid-flow. Default 24h, matching the upload TTL.

**It applies only to blobs that have never been shared.** `blobs.released_at` is stamped
when a share gives up its reference, and a blob carrying it is collected as soon as
nothing points at it — it is by definition past the upload-to-share window the grace
protects. Without that distinction, revoking a link would leave the bytes on disk for up
to a day. (`last_referenced_at` cannot serve this purpose: it is stamped at upload time
too, so it is non-NULL for blobs that never belonged to any share.)

Revocation is therefore immediate in the sense that matters — the link 404s the moment
`deleted_at` is set — and the disk comes back on the next sweep rather than a day later.

**An explicit delete bypasses the sweep entirely.** Terminating an upload or removing a
file from a share reclaims the blob in the same request, via `storage::deleteIfOrphaned`.
The grace period protects windows the *user did not choose*; when someone says "discard
this", waiting is just surprising. Without termination the Remove button could only stop
the browser sending, leaving the partial file on disk for a full day — which is exactly
how 673 MB of abandoned uploads accumulated during development.

Orphan deletion removes the database row *before* unlinking the file. The reverse order
would leave a row pointing at a missing file if the process died in between, surfacing as
a broken download; this order can only leak a file, which is harmless and recoverable.

Expiry is 1–365 days, defaulting to 30. Optional `max_downloads`, optional per-share
password (argon2id). Only a request starting at byte zero counts as a download, so a
video player seeking does not exhaust the limit — which makes `max_downloads` a courtesy
limit rather than a hard control.

## Auth, and why uploads are invite-only

**Uploads require an account. Downloads are public by token.**

This is not paranoia about your friends — it's about the host. An open upload endpoint on
a public IP gets found by scanners and used to serve warez, phishing kits and worse.
Providers' abuse teams are strict, and repeated reports end with the server nullrouted. So:

- Registration is by invite code only; no open signup.
- Per-user storage quota (default 50 GB) and rate limits.
- Access logs retained, so an abuse ticket can actually be answered.
- No directory listing, no enumerable IDs.

Sessions are a random 256-bit token in an `HttpOnly; Secure; SameSite=Lax` cookie.
Passwords are hashed with **Argon2id** (`libargon2`).

## Frontend

Vite + **Svelte 5** (runes) + TypeScript + Tailwind v4 + Uppy. Builds to pure static
assets that Caddy serves directly — no SSR, no Node process on the server. That keeps the
deployment a single C++ binary plus a web server, which is the main payoff of this stack.

Three screens: login/redeem-invite, upload (drag-drop + expiry picker → link), and the
public download page.

## Deployment

Multi-stage Docker build (builder compiles Drogon + app; runtime carries only shared libs),
orchestrated by `docker compose` alongside Caddy. Application data bind-mounts to
the data directory. Deploy is `git pull && docker compose up -d --build` on the server.

Containerising matters more than usual on a rolling-release host, where a system update
can move glibc, OpenSSL and GCC at any time. If the binary linked against host libraries,
a routine update could break the app at an inconvenient moment. Pinning the container
base image means a bad host update breaks the host, not TheArchive.

Host packages needed are just `docker` and `docker-compose`.

### Deployment target

A single Linux host with Docker. The container runs as a non-root uid that owns the data
directory, so the bind mount needs no permission gymnastics.

Two host-level details are worth planning around rather than discovering:

- **Assume `sudo` needs a password.** Privileged steps are commands an operator runs, not
  something a deploy script can self-elevate into.
- **Check `systemctl --user` as well as the system units.** On a host where services were
  set up as lingering user units, a system-level `systemctl is-enabled` reports
  `disabled` for something that nonetheless survives reboots. TheArchive itself runs as a
  system service — `docker.service` enabled plus `restart: unless-stopped` — rather than
  relying on lingering.

### Networking notes

- `A` and `AAAA` records at the apex and `www`; Caddy provisions certs on first request.
  ACME needs :80 reachable, so it cannot be firewalled off.
- If the domain goes behind Cloudflare, keep the records **DNS-only (grey cloud)**.
  Cloudflare's free tier isn't intended for proxying large file downloads, and a
  dedicated host's bandwidth is usually unmetered anyway — proxying only adds a
  bottleneck and a TOS problem.
- Set rDNS on the host's IP.
- **Verify the DNSSEC chain before the first certificate request.** If a `DS` record is
  published at the registry but the zone isn't correspondingly signed, every validating
  resolver returns SERVFAIL while non-validating ones answer fine — so the domain looks
  healthy from some machines and dead from most. Let's Encrypt validates DNSSEC, so
  issuance fails too. Quick check: if `1.1.1.1`, `8.8.8.8` and `9.9.9.9` all SERVFAIL but
  `208.67.222.222` (non-validating) resolves, that is the failure, not propagation.
  On a freshly registered domain this can appear for a while and then clear on its own,
  as the registry publishes the `DS` before the provider's signing catches up — so
  re-test before changing any registrar settings.
- **Name the hosts in the Caddyfile only once they resolve.** ACME failure for one name
  fails the whole certificate, so a not-yet-propagated `www` would take the apex down
  with it. Start apex-only and add `www` afterwards.
- **Put a host firewall in front before going public.** Inbound should be limited to
  SSH, 80 and 443. Note that a published Docker port is DNAT'd in prerouting and never
  traverses the input hook, so the firewall will not protect it — container exposure is
  decided by the bind address in compose.

## HTTP surface

| | | |
| --- | --- | --- |
| `POST` | `/api/auth/bootstrap` | first admin; refuses once any user exists |
| `POST` | `/api/auth/register` | invite-only |
| `POST` | `/api/auth/login` / `logout` | |
| `GET` | `/api/me` | |
| `POST` | `/api/invites` | issue a code — **admin only** |
| `GET` | `/api/admin/users`, `/users/{id}` | list and per-user detail |
| `POST` | `/api/admin/users/{id}/{role,disable,enable,revoke}` | |
| `DELETE` | `/api/admin/users/{id}` | destructive, cascades |
| `GET`/`DELETE` | `/api/admin/invites[/{code}]` | list; revoke an unredeemed code |
| `GET`/`DELETE` | `/api/admin/shares[/{token}]` | every live share; revoke any |
| `GET` | `/api/admin/overview` | totals, per-uploader usage, disk |
| `OPTIONS` | `/files` | tus capability discovery |
| `POST` | `/files` | create upload → `Location` |
| `HEAD` | `/files/{id}` | `Upload-Offset` — where to resume |
| `PATCH` | `/files/{id}` | append at offset; on completion → blob |
| `DELETE` | `/files/{id}` | tus termination — discard now, not at TTL |
| `POST` | `/api/shares` | bundle completed uploads into a link |
| `GET` | `/api/shares` | list your own |
| `DELETE` | `/api/shares/{token}` | revoke early |
| `DELETE` | `/api/shares/{token}/files/{id}` | remove one file; revokes the share if it was the last |
| `GET` | `/api/storage` | usage and free space |
| `GET` | `/api/shares/{token}` | share metadata (public) |
| `GET` | `/d/{token}/{fileId}` | the bytes (public) |
| `GET` | `/d/{token}/{fileId}/thumb?s=sm\|lg` | rendered preview, `inline` (public) |
| `GET` | `/d/{token}/{fileId}/inline` | the original served `inline`, for `<video>` (public) |

Downloads are always `Content-Disposition: attachment` with `X-Content-Type-Options:
nosniff`, and any type a browser might execute in our origin is downgraded to
`application/octet-stream`. Range requests are honoured, so large downloads resume and
video seeks work — Drogon's `newFileResponse` does not parse `Range` itself, so the
handler computes the byte window and passes explicit offset/length.

## Roles and the control panel

Three tiers replace what was an `is_admin` boolean: `user`, `privileged`, `admin`.
`privileged` grants nothing beyond `user` — it exists so the tier can be assigned before
anyone decides what it should mean, without a second migration over live data. The old
boolean was dropped rather than kept alongside, since two representations of one fact
are exactly how they drift apart.

Every `/api/admin/*` route is behind `requireAdmin`. The frontend hiding a link is
presentation, not access control.

Two guards matter more than they look. **The last administrator cannot be demoted,
disabled or deleted**, and nobody can perform those actions on themselves: there is no
recovery from an instance with no admin short of editing the database by hand. And
**disabling deletes the account's sessions**, because `userForSession` rejecting a
disabled user would otherwise leave an already-issued cookie working until it expired.

Disabling is the reversible option and deletion is not — deleting cascades to shares and
sessions, and the user's blobs lose their last reference and leave on the next sweep. The
UI requires the username typed to confirm it.

## Previews

Images are thumbnailed at upload with libvips, which shrinks during decode rather than
loading the full raster, and applies the EXIF orientation tag — without which portrait
phone photos arrive on their side. Two sizes: 320px for the list rows, 1600px for the
viewer. Roughly 175 KB per image against an 8 MB original.

libvips also brings HEIC and AVIF decode. That is not a nicety: browsers cannot render
HEIC at all, so an iPhone upload would otherwise be invisible to everyone who received
the link.

**Previews never increment `download_count`.** Browsing a gallery of forty photos would
otherwise exhaust a share's `max_downloads` before the recipient downloaded anything —
the same reasoning that already exempts ranged requests.

`/inline` is the only endpoint that serves user content without `attachment`, so it uses
an explicit **allowlist** of types rather than `mime::isRiskyToRender`'s denylist: a type
nobody anticipated is refused rather than executed in our origin.

Thumbnails are derived state, which makes them easy to leak. Both deletion paths — the
expiry sweep and explicit removal — route through `storage::removeBlobFiles`, the single
place that knows what a blob owns on disk.

## Deliberately deferred

- **Streaming ZIP for "download all"** — store-only (no compression; photos and video don't
  compress), streamed so nothing hits disk. Worth doing, but v1.1.
- **Per-user quota.** `users.quota_bytes` exists and nothing enforces it.
- **EXIF extraction.** `client_mtime` gives a usable timestamp today; capture time,
  camera and orientation would need libexif and matter mainly to an archival mode.
- Email. Invites are codes you paste into a chat; no SMTP anywhere.
