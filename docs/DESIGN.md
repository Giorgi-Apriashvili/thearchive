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
├── thumbs/ab/cd/<sha256>-{sm,lg}.webp   previews, same fan-out as blobs
└── db/archive.db              SQLite (WAL mode)
```

Deployment configuration is not in here. `docker-compose.yml`, `.env` and
`caddy/Caddyfile` live in the repository's `deploy/`, so they arrive with `git pull`
while the data directory is only ever written by the running app.

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
       deleted_at, released_at, visibility)              -- private | public

share_files(id, share_id, blob_sha256, filename, content_type, size,
            uploaded_by, client_mtime, relative_path)

uploads(id PK, owner_id, filename, content_type, total_size, offset_bytes,
        created_at, expires_at, blob_sha256, completed_at,
        client_mtime, relative_path, user_agent)

rooms(id, name, created_by → users SET NULL, creator_name, created_at)
room_members(room_id, user_id, state, invited_by, invited_at,
             responded_at, last_read_id, PK(room_id, user_id))  -- invited|member|declined
messages(id, room_id → rooms CASCADE, user_id → users SET NULL,
         author_name, body, created_at, deleted_at, deleted_by)
room_blocks(user_id, room_id, created_at, PK(user_id, room_id))
user_blocks(user_id, blocked_id, created_at, PK(user_id, blocked_id))

message_mentions(message_id → messages CASCADE, user_id → users SET NULL,
                 mentioned_name, PK(message_id, mentioned_name))
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
- Password guessing is limited on every check — sign-in, change-password and share
  passwords — per client and per target. See [Limiting password guessing](#limiting-password-guessing).
- No directory listing, no enumerable IDs.

Two things this list used to claim, and which are **not** in place: a per-user storage
quota (`users.quota_bytes` exists, defaulting to 50 GiB, and nothing reads it) and
retained access logs (Caddy has no `log` directive and the app keeps no access log —
only failed password checks and admin actions are logged with any context). Both are
listed under [Deliberately deferred](#deliberately-deferred). If an abuse report arrives
today, there is little to answer it with.

Sessions are a random 256-bit token in an `HttpOnly; Secure; SameSite=Lax` cookie.
Passwords are hashed with **Argon2id** (`libargon2`).

### Changing a password

`POST /api/auth/password` takes the current password as well as the new one. Requiring it
is what stops a borrowed browser from becoming a locked-out account, and it is checked
*before* the new password is validated — otherwise a validation message would tell
someone holding a stolen session what the password rules are before they had shown they
belong here.

A successful change **ends every other session for that account**, in the same
transaction as the hash update: a new password with the old cookies still live is exactly
the state the purge exists to prevent. `sessions.token` stores a SHA-256 of the real
token, so the "keep this one" comparison hashes the caller's cookie to match.

**The caller's own session deliberately survives.** Changing your password should not
sign you out of the tab you changed it in, and the guard against a stolen session is the
current-password check rather than the purge. Rotating the surviving token was considered
and skipped: every other session is already gone, so it is one only this client holds.

The endpoint needs a session, so it adds no anonymous brute-force surface — and its
current-password check shares the account's guessing limit with sign-in, so a stolen
session is not a way to guess the password unthrottled.

### Limiting password guessing

Every password check — sign-in, change-password and share passwords — goes through one
limiter (`throttle.h`), checked **before** the password is verified. A refused attempt
therefore costs no Argon2, which matters twice: the hash is deliberately expensive, so
unbounded attempts would also be a way to burn the server's CPU, and a guesser learns
nothing even on the attempt where they happen to be right. Only failures count; a viewer
fetching forty thumbnails with the correct share password is never limited.

Two layers, because they stop different attackers:

| | limit | stops |
|---|---|---|
| per client address | 10 failures in 15 minutes, then refused until the oldest ages out | one machine guessing quickly |
| per target (account or share) | 5 free, then each attempt waits 1s, 2s, 4s … capped at 60s | many machines guessing one password slowly |

The per-target layer **slows rather than locks**. Anyone who knows a username can fail
against it, and a hard lockout would hand them a way to keep its owner out; a delay
capped at a minute cannot become that. It still bounds a distributed attack to roughly one
guess a minute per account — which is what makes the six-character minimum tolerable.
Names that do not exist are slowed exactly like real ones, or the limiter would reveal
which usernames are real after five tries.

One client has **one budget** across sign-in and share passwords: they are the same
activity, and splitting them would double what a guesser gets.

**Where the client address comes from** decides whether any of this works. Behind Caddy
the TCP peer is Caddy, so the address comes from `X-Forwarded-For` — trusted only when the
peer is private or loopback, i.e. the proxy — and the rightmost entry is used. Caddy
discards any `X-Forwarded-For` a client sends and writes its own; that was tested against
the production image with spoofed headers, not assumed. Trusting the header from anyone
else would let a client name itself.

The same test showed the other trap: arriving through Docker's port publishing, a client
can appear as the Docker gateway. A limit on that address would put every visitor in one
bucket and turn a guessing limit into a lockout of everybody. So **a resolved address that
is private is not treated as a client at all** — per-address limiting stands aside and
per-target limiting still applies. Weaker, never a global lockout. Private is written out
rather than taken from trantor's `isIntranetIp`, which omits IPv6 unique-local
(`fc00::/7`, the range Docker's IPv6 network on the host uses) and all of `127/8` but one
address.

IPv6 clients are held to account by `/64`, since a subscriber is routinely handed 2^64
addresses and a limit on anything narrower limits nothing.

State is in memory: a restart forgets, which is acceptable for limits measured in
minutes. Both maps are capped at 10,000 entries — expired entries are pruned first, then
the least recently active — so a flood of new addresses or usernames cannot grow them
without bound. Keys are truncated, since usernames reach the limiter unvalidated.

Failed checks are logged with the resolved address, or `(no public address)`. That line is
how to confirm, on a given deployment, whether the app is seeing real client addresses.

**On the production host, checked after deploying:**

- **IPv4 clients are seen correctly.** A failed sign-in from outside logged the real
  public address: Docker forwards published IPv4 ports in the kernel, which keeps the
  source address, and Caddy passes it on. Both layers apply.
- **IPv6 clients are not.** The site has an AAAA record, but the compose network
  (`deploy_edge`) has IPv6 disabled, so Docker hands IPv6 connections to its userland
  proxy and Caddy sees the proxy's private address instead of the client's. A request
  from the host to its own IPv6 address — which crosses the same forwarding — logged
  `(no public address)`. For IPv6 visitors the per-address layer therefore stands aside,
  exactly as designed, and only the per-target layer applies. Enabling IPv6 on the compose
  network would let Docker forward those in the kernel too; that is a change to
  production networking, and has not been made.

### Renaming an account

`POST /api/admin/users/{id}/username` normalises and validates the new name exactly as
registration does, so a rename cannot produce a name that could not have been registered.
No guard refuses it — not on yourself, not on the last admin — because a rename removes
no privilege and ends no session. Sessions key on `user_id`, so nobody is signed out.

**The rename carries into the chat snapshots**, `messages.author_name` and
`rooms.creator_name`, in the same transaction. Those columns exist so a *deleted*
member's history stays attributed, not to freeze a display name; leaving them behind
would mean someone's messages carried one name while their uploads carried another,
because `share_files.uploaded_by` and `blobs.first_uploader` are foreign keys and follow
a rename for free.

`message_mentions.mentioned_name` deliberately does **not** follow. It mirrors the
literal `@name` in a message body, which nobody may rewrite, and the client matches the
two against each other to decide what to highlight. The link to the account is `user_id`,
and that is what a notifier reads.

One consequence, worth stating because it is the kind of thing that silently goes wrong:
once a name can be changed, it can also be **reused**, so an old `@bob` may not mean
today's bob. "Did this message name me" is therefore answered server-side by account
(`mentions_me` on each message) rather than by the client comparing strings. The client
still compares the name to decide *which* `@` in a message naming several people is the
one that meant you — the flag cannot answer that, and the comparison cannot answer the
first question. Both are needed.

### Recovery, such as it is

There is no email anywhere in this system and no intention to add SMTP, so nothing can be
sent to someone who has forgotten their password. `POST /api/admin/users/{id}/password`
is the whole recovery story: an admin triggers it, the server generates a 12-character
token, stores only its hash, and returns the plaintext **once**. Every session for that
account ends — whoever is asking for the reset is not the person holding those cookies.

An admin cannot reset their own password this way (`409`, pointing at the account page).
The one account that can reach the endpoint should not use it to skip the
current-password check. There is no `requireAnotherAdminRemains` guard, because a reset
does not reduce the number of admins and the new password is handed straight back.

## Frontend

Vite + **Svelte 5** (runes) + TypeScript + Tailwind v4 + `tus-js-client`. Builds to pure static
assets that Caddy serves directly — no SSR, no Node process on the server. That keeps the
deployment a single C++ binary plus a web server, which is the main payoff of this stack.

Screens: login/redeem-invite, the workspace (uploads and chat as two tabs), the public
download page, the account page (`/account`, where you change your password), and the
control panel. A ~40-line router matches the path and navigates
with `pushState`; the tab and the open chat room are both **the URL**, so a room is
linkable, the back button works, and a reload lands where you were.

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
| `POST` | `/api/auth/password` | `{current_password, new_password}`; ends other sessions |
| `GET` | `/api/me` | |
| `POST` | `/api/invites` | issue a code — **privileged or admin** |
| `GET` | `/api/admin/users`, `/users/{id}` | list and per-user detail |
| `POST` | `/api/admin/users/{id}/{role,disable,enable,revoke}` | |
| `POST` | `/api/admin/users/{id}/password` | generate one, shown once; ends their sessions |
| `POST` | `/api/admin/users/{id}/username` | rename; carries into the chat snapshots |
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
| `GET` | `/d/{token}/all.zip` | every file, streamed as one archive (public) |
| `GET`/`POST` | `/api/chat/rooms` | every room annotated with my state; create one |
| `GET` | `/api/chat/rooms/{id}/members` | members, plus outstanding invitations |
| `POST` | `/api/chat/rooms/{id}/invite` | `{username}` — **creator or admin** |
| `POST` | `/api/chat/rooms/{id}/respond` | `accept` \| `decline` \| `block_room` \| `block_user` |
| `GET`/`POST` | `/api/chat/rooms/{id}/messages` | members only; `?since=` is a cursor |
| `POST` | `/api/chat/rooms/{id}/read` | `{last_id}` — drives the unread badges |
| `DELETE` | `/api/chat/messages/{id}` | **admin only**; soft delete |
| `GET` | `/api/chat/blocks` | what I have blocked |
| `DELETE` | `/api/chat/blocks/{room\|user}/{id}` | undo one |
| `POST` | `/api/csp-report` | browsers report CSP violations here; logged (public) |

Every endpoint that checks a password — sign-in, change-password, and any share endpoint
given `X-Share-Password` or `?p=` — answers `429` with `Retry-After` when the guessing
limit applies. See [Limiting password guessing](#limiting-password-guessing).

Downloads are always `Content-Disposition: attachment` with `X-Content-Type-Options:
nosniff`, and any type a browser might execute in our origin is downgraded to
`application/octet-stream`. Range requests are honoured, so large downloads resume and
video seeks work — Drogon's `newFileResponse` does not parse `Range` itself, so the
handler computes the byte window and passes explicit offset/length.

## Response headers

Split by who owns the decision. **Caddy** sets what holds whatever the app does —
`Strict-Transport-Security`, `X-Frame-Options`, `Referrer-Policy`,
`X-Content-Type-Options` — deferred, so a header the app also sends is replaced rather
than duplicated (tested: without `defer`, a download carried `nosniff` twice). **The
app** sets the Content-Security-Policy, because a CSP describes what *this frontend* may
do and has to change whenever the frontend does.

The app sends no `Server` header. Drogon announces `drogon/<version>` by default, which
turns "does this host run something with a known hole" into a lookup.

`/robots.txt` disallows everything for every crawler (`web/public/robots.txt`). Before it
existed the SPA fallback answered that path with the app's HTML and a `200`, which a
crawler parses as a rules file with no rules — so, in effect, everything allowed. A
single `User-agent: *` group covers GPTBot and every other AI or search crawler, including
ones that do not exist yet, where naming bots individually would not. It is a request
rather than a control, and it stops crawling, not listing: a search engine can still show
a bare share URL it found linked elsewhere without fetching it. What actually protects a
share is that it is private by default and its link is an unguessable token.

Fingerprinted assets under `/assets/` are `public, max-age=31536000, immutable`: their
names change whenever their content does. `index.html` is revalidated on every load,
because it names the current hashes — caching it would hide a deploy.

### Content-Security-Policy

Every source is `'self'` or `'none'`: no `'unsafe-inline'`, no `'unsafe-eval'`, nothing
external. That was established from the build rather than assumed — `index.html` has no
inline script or style, the bundle contains no `eval` or `new Function`, no workers,
websockets or object URLs, and the fonts are self-hosted.

The one real question was styles. Svelte's dynamic `style="width: {x}%"` bindings (the
storage bar, upload progress) and its transitions could have needed `'unsafe-inline'`.
Reading the runtime suggested they go through `style.cssText` and `element.animate()`,
which `style-src` does not govern — but CSP enforcement belongs to the browser, so that
was confirmed in one rather than trusted.

`tools/csp-check/` does it: a headless Chromium walks every CSP-sensitive part of the app
— both style bindings, every transition, thumbnails and the lightbox, chat, the control
panel — and records each `securitypolicyviolation` event. It ends with a deliberate
violation as a control, and checks that its report reached the server log, because a
detector that never fires proves nothing. Run it after any frontend change: the policy
is only correct for a given frontend, and the next feature can break it silently.

**Failure is silent, so violations are reported.** A blocked script or style simply
does not happen; the symptom is a feature that stops working in someone else's browser.
The policy carries `report-uri /api/csp-report`, and the endpoint logs one line per
violation. It needs no session, since anonymous recipients use the download page, which
makes its input attacker-controlled on its way into the log: every field is capped and
control characters are replaced, so a crafted report cannot forge a log line (tested).
`report-uri` rather than `report-to`, because it is the one all three engines send.

Coverage has limits worth stating. The browser check is Chromium only, and video
playback — `media-src` for `/inline` — is not exercised by it. Safari and Firefox are
covered in production by the report endpoint instead.

## Who can open a link

Shares are **private by default**: only a signed-in member can read the metadata,
download, preview or fetch the archive. Unticking "private" at creation makes the link
work for anyone who holds it, which is what every share did before this existed.

The default is that way round deliberately. Publishing to the whole internet should be
something you chose, not something you got by not noticing a checkbox.

All five public entry points — metadata, download, thumbnail, inline and the zip — go
through one `authoriseShare` gate that checks existence, then visibility, then the
password. A check spread across five handlers is a check that will eventually be missing
from one of them. The owner and admins bypass the password, since one set it and the
other can already reach everything.

A members-only refusal and a password-required refusal are both `401` — signing in
genuinely resolves the former, so `403` would misdescribe it. They are distinguished by
a `reason` field on the error body rather than by matching on prose, which lets the
download page offer a sign-in prompt instead of a password box that could never work.

Visibility is changeable after the fact, by the owner (`PATCH /api/shares/{token}`) or an
admin (`PATCH /api/admin/shares/{token}`), rather than requiring the link be destroyed
and re-sent. The value is validated against exactly `private` or `public` rather than
stored as given: `authoriseShare` treats anything that is not `"public"` as private, so
an unexpected value would fail closed — safely, but silently, leaving an owner believing
they had published something they had not.

**Making a share private is not a recall.** It stops future requests; it does not undo
downloads already taken, and a preview already fetched may sit in the recipient's browser
cache for up to an hour. The confirmation says so.

**Existing shares were migrated to `public`.** They were created under link-is-enough
semantics and handed to people who may have no account; silently tightening them would
have broken links already in circulation.

## Roles and the control panel

Three tiers replace what was an `is_admin` boolean: `user`, `privileged`, `admin`.
`privileged` can issue invites; `admin` additionally reaches the control panel. The old
boolean was dropped rather than kept alongside, since two representations of one fact
are exactly how they drift apart.

Checks go through `requireRole(req, auth, minimum)` against an ordered rank — `user` <
`privileged` < `admin` — rather than a predicate per capability. `roleRank` returns -1
for anything unrecognised, so an unexpected value in the database fails closed instead
of satisfying every comparison.

Invites remain gated because an open signup on a public IP becomes a phishing host
within days. Widening them to `privileged` means vouching for a newcomer no longer has
to route through one person, while the gate itself stays shut.

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

## Chat

Rooms on the main page, beside uploads. The rule that shapes the whole feature: **every
room's name is visible to every signed-in member, and nothing else about it is.** A room
nobody can see is a room nobody can ask to join, so the list is public and entry is not.
Membership is checked in one `requireRoomMember` helper, the same reasoning as
`authoriseShare` — a check spread across seven handlers is a check that will eventually
be missing from one of them.

A non-member gets `404`, not `403`. They already know the room exists, from the list; a
`403` would add nothing except a way to probe which rooms a given account belongs to.

### History outlives its author

Every other foreign key to `users` in this schema is `ON DELETE CASCADE`, and the control
panel can delete accounts. Following that pattern here would mean deleting a member
**erases their side of every conversation** — which contradicts the point of permanent
history, and leaves everyone else's replies answering nothing.

So `messages.user_id` and `rooms.created_by` are `ON DELETE SET NULL`, alongside an
`author_name` / `creator_name` snapshot taken at write time. The account goes; the words
stay, still attributed, marked *former member*. `server/tests/chat.sh` deletes an author
mid-suite and asserts their messages survive, because this is exactly the kind of
property a later "clean up the cascades" change would helpfully undo.

Admin removal keeps the row and erases its contents: `deleted_at` / `deleted_by` are set,
the body is overwritten and the message's mention records are deleted, so the client
renders *"Removed by alice"* in place. Keeping the row keeps the conversation's shape —
closing the gap would silently reflow a conversation around what was taken out, which is
its own kind of dishonesty. Erasing the text is what "removed" has to mean.

It did not always. Removal first only set `deleted_at`, leaving the text in the database —
withheld by the API, but present in every backup — while the confirmation dialog told the
admin it was gone for good. Migration 12 erases the text of messages removed before the
fix; backups taken earlier still hold it until retention deletes them.

### Invitations and blocks

Only a room's creator (or an admin) can invite, matching "invited by a group creator".
Widening that to any member is one line in `chat.cc`.

An invitation can be accepted, declined, or blocked — and blocking splits in two, because
declining a topic and avoiding a person are different intentions: `room_blocks` hides one
room, `user_blocks` stops that person inviting you anywhere. Both are listed under
**Blocked** and can be undone; a block you cannot find is a block you cannot undo.

**Inviting someone who has blocked you reports success.** The invitation is silently
dropped. Returning an error would disclose the block to the one person it was made
against, which is not the blocker's decision to have made for them.

Admins can invite to any room, including themselves — moderation is not possible in a
conversation you cannot read. That is the only door: nothing lets an admin read a room
without joining it, and joining shows up in the member count like anyone else's.

### Delivery: polling

Two cadences. Messages poll every 2s while a room is open; the room list polls every 10s
for as long as anyone is signed in, since its unread badge is how someone on the uploads
tab learns a message arrived. Both stop while `document.visibilityState` is `hidden`, so a
forgotten tab does not keep a laptop awake, and both poll immediately on waking.

`idx_messages_room (room_id, id)` makes `WHERE room_id = ? AND id > ?` an index range
scan, which is what keeps an idle poll close to free. Nothing in the API assumes polling
— `since` is a cursor — so SSE later would be additive rather than a rewrite.

The 200-message cap takes the **newest** 200, not the oldest: opening a room should land
on the current conversation. The cost is a gap rather than a duplicate — a client more
than 200 behind skips what it missed — and that only happens to a tab that was closed,
which reopens at `since=0` anyway.

One consequence of an incremental cursor: a tombstone lands on an id the cursor has
already passed, so it would never arrive. Every fifth message poll therefore re-reads the
room in full, which picks up removals and self-heals any drift.

### Who is in a room

`GET /api/chat/rooms/{id}/members` is members-only like everything else, and returns
members plus **outstanding invitations** — the pending list is what stops a creator
re-inviting someone who simply has not answered yet.

Declines are never returned. Whether someone turned an invitation down is their
business, not a status the room displays about them; the creator learns it the only way
that matters anyway, which is that the pending entry stops being listed.

### @mentions

Mentions are resolved **once, at send time**, against the room's membership, and stored
in `message_mentions`. They are not re-parsed from the body on every read. Three things
follow, and all three are the reason it is done this way:

- a mention is a fact the server holds, so a notifier is a query rather than a migration
  plus a backfill over every message ever sent;
- what the client highlights is exactly what a notifier would act on, because it renders
  the resolved list rather than re-deriving one from the text;
- an `@name` typed before that person joined stays text forever, rather than quietly
  becoming a live mention the day they accept an invitation.

An `@name` matching nobody *in that room* records nothing — treating it as a mention
would let a message claim to have notified someone it never could. `@` mid-word is not a
mention either, or every email address would name its mail host. The client's
`MENTION_PATTERN` and the server's `isNameChar` have to agree on where a name ends; they
are commented as a pair.

`user_id` drops to `NULL` when an account goes, with `mentioned_name` keeping the
rendering stable — the same split as `messages`. A notifier reads rows with a live
`user_id` and ignores the rest, since there is nobody left to notify.

`GET /api/chat/rooms` returns `mentions_unread` beside `unread`, counting unread messages
that named you and excluding removed ones — a badge pointing at a tombstone is a summons
to nothing. The UI renders that differently (`@3` rather than `3`), because being named
is a different event from something having happened. **Nothing is delivered yet**; the
count is the substrate a notifier will read.

### Who is speaking

Names in chat are coloured by role — admin, privileged, ordinary member — from one
mapping in `lib/roles.ts`, because a name coloured one way in the message list and
another in the member popover is worse than no colour at all. The hues sit at the same
saturation and lightness as the accent so they read as a family; `privileged` *is* the
accent, since a second amber would be a distinction without a difference. None of them
is the red used for destructive actions: an administrator speaking is not a warning.

Colour is the whole of it. No tier is written out beside a name, and nothing marks your
own messages as yours — a chat where every line is captioned with its author's rank
stops reading as a conversation. The member popover keeps one word, *creator*, because
that says something about the room rather than about the account, and it is the one
thing colour cannot carry.

Note which way each field points. `author_name` is a **snapshot**, because it is what the
message was signed with and that is history. `author_role` is read **live**, because it
is identity — promote someone and they should read as an admin everywhere, including in
what they said last week. A departed account reports no role at all rather than a
guessed one.

### Rendering

Message bodies go through `{message.body}`, never `{@html}`. Svelte escapes by default,
and this is the one place in the app where another person's arbitrary text reaches the
DOM, so it is worth naming rather than leaving to habit.

The uploads pane stays mounted and is merely hidden when the chat tab is showing.
Unmounting it would discard the queue, the progress and the upload ids of anything in
flight — switching tabs mid-upload would quietly cost someone a 3 GB video.

## Privacy

The notice lives at `/privacy`. It is linked from the sign-in page, the download page and
the workspace, and readable without an account, since link recipients are part of its
audience. It was written from the code rather than from intentions. Every statement in it
was checked against what the server stores, logs and deletes, and working through it found
three things that made it untrue until they were fixed:

- **Backups had no retention.** One was taken, by hand, before each deploy, and none was
  ever deleted. `deploy/backup.sh` now takes a verified backup and prunes anything past
  `ARCHIVE_BACKUP_DAYS` (14), and a systemd user timer (`deploy/install-backup-timer.sh`)
  runs it daily. The schedule is what makes the period true: pruning only on deploy would
  leave old backups in place through any quiet stretch. A backup is the database only —
  files expire with their links, and copying them would keep what the notice says is
  deleted.
- **Log retention was an accident.** Docker kept container logs without limit, and since
  the guessing limit the app's log holds addresses and attempted usernames. Compose now
  rotates both services' logs at three files of 10 MB.
- **Removed chat messages were not removed** — see above.

**Who runs the site is configuration, not source.** `ARCHIVE_OPERATOR_NAME`,
`ARCHIVE_OPERATOR_CONTACT` and the optional `ARCHIVE_HOSTING_LOCATION` come from
`deploy/.env` through `GET /api/privacy`. The repository is public, and a name and email
address committed to it would stay in its history for good. It also means every deployment
states its own operator; one with none configured says so, rather than rendering blanks.

**Every period the notice states is the one the server enforces.** `/api/privacy` reports
the session length, link expiry and cap, sweep interval, unshared-upload lifetime and
backup retention from the same constants and settings the code acts on. Several constants
moved into headers for this. An upload never made into a link is stated at the later of
its own expiry and the blob grace period, plus a sweep. The notice cannot say 30 days
while the code does something else, because there is only one 30.

What the notice leans on as evidence rather than promise: the strict CSP is why it can say
"no third-party scripts, fonts or embeds", and `robots.txt` is why it can say crawlers are
asked to stay away. Its legal-basis sentence is plain language, not legal advice; that is
the part to have someone qualified read.

## Deliberately deferred

- **Per-user quota.** `users.quota_bytes` exists and nothing enforces it.
- **Access logs.** None are kept. Turning on Caddy's `log` directive is one line, but it
  is a decision rather than a default: it means retaining every visitor's address, and
  choosing where the file lives, how it rotates and how long it is kept.
- **A general request-rate limit.** Password checks are limited; nothing else is. The
  real exposure is bandwidth rather than guessing: anyone holding a public link can
  download it, or its streamed ZIP, as often as they like.
- **Chat**: editing messages, attachments, typing indicators, per-room notification
  settings, renaming or deleting a room, and message search. Each is additive; none was
  needed to know whether the core works. Leaving a room is not there either — the only
  way out today is never having accepted.
- **Delivering notifications.** `message_mentions` and `mentions_unread` exist and are
  populated; nothing acts on them yet. Whatever comes — web push, email, a digest —
  reads those rows rather than needing new ones.
- **EXIF extraction.** `client_mtime` gives a usable timestamp today; capture time,
  camera and orientation would need libexif and matter mainly to an archival mode.
- Email. Invites are codes you paste into a chat; no SMTP anywhere.
