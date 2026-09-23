# TheArchive

A self-hosted file drop for a closed group of friends. Upload files, get a link, share it,
they download. Links expire after 30 days by default and the disk space comes back. There
are chat rooms alongside it, for talking about what was shared.

**Stack:** C++20 / Drogon · SQLite · Svelte 5 + Vite · Caddy · Docker Compose

See [docs/DESIGN.md](docs/DESIGN.md) for the architecture and the reasoning behind it.

## Layout

```
server/      C++ backend (Drogon)
web/         Svelte 5 frontend (Vite)
scripts/     dev environment bootstrap
docs/        design notes
```

## Development

Linux, or WSL on Windows. The bootstrap script handles Arch and Debian/Ubuntu; pick
whichever matches your deployment target so package names and toolchain versions line up.

On Windows, enter WSL **first** — running these in PowerShell configures a Windows build
against MSVC, which is not what this targets and which needs vcpkg to supply jsoncpp,
OpenSSL and zlib.

```bash
# one time
./scripts/dev-setup.sh

# backend — the build directory lives in the WSL filesystem, NOT under /mnt/e.
# Compiling across the Windows/Linux boundary is drastically slower.
cmake -S server -B ~/build-thearchive -G Ninja -DCMAKE_BUILD_TYPE=Release   # once
cmake --build ~/build-thearchive -j$(nproc)                                 # thereafter

ARCHIVE_DATA_DIR=/tmp/archive-test ~/build-thearchive/thearchive
curl -s localhost:8080/healthz                 # {"schema":1,"status":"ok"}

# frontend — Vite serves the app on :5173 and proxies /api, /files, /d and /healthz
# to the backend, so cookies behave exactly as they will in production.
cd web && npm install && npm run dev
```

For a production-shaped run, build the frontend and let the C++ server serve it:

```bash
cd web && npm run build          # -> web/dist
ARCHIVE_WEB_ROOT=$PWD/dist ~/build-thearchive/thearchive
```

Without `ARCHIVE_WEB_ROOT` the server is API-only and `/` returns 404 — which is
correct, not a fault. Unmatched non-API paths fall back to `index.html` so client-side
routes like `/d/<token>` survive a refresh, while `/api/*` keeps its real status code.

> `~` is your Windows profile in PowerShell but your Linux home inside WSL. If CMake
> reports paths starting `C:/`, you are in the wrong shell.

VS Code's CMake Tools extension is pointed at the *same* build tree, so the two never
duplicate a Drogon compile. Pass `-DCMAKE_BUILD_TYPE` only on the first configure —
repeating it on every run would fight the extension's selected variant and reconfigure
the cache, which rebuilds Drogon from scratch each time.

Configuring prints a deprecation warning about `cmake_minimum_required` in trantor.
That is upstream's declared minimum, not a problem here — below 3.5 CMake 4 would
refuse outright, which the `CMAKE_POLICY_VERSION_MINIMUM` shim in `server/CMakeLists.txt`
exists to prevent. Pass `-Wno-deprecated` to silence it.

Configuration is environment-only, so nothing needs mounting into the container:

| Variable | Default | |
| --- | --- | --- |
| `ARCHIVE_DATA_DIR` | `./data` | blobs, in-progress uploads and the SQLite file |
| `ARCHIVE_BIND` | `127.0.0.1` | Caddy is the only thing that should reach this |
| `ARCHIVE_PORT` | `8080` | |
| `ARCHIVE_PUBLIC_URL` | `http://localhost:8080` | used to build share links; cannot be inferred from behind a proxy |
| `ARCHIVE_SECURE_COOKIES` | on | set `0` for plain-HTTP local dev, or login never persists |
| `ARCHIVE_MAX_UPLOAD_BYTES` | 20 GiB | advertised as `Tus-Max-Size` |
| `ARCHIVE_GC_INTERVAL_SECONDS` | 900 | expiry sweep cadence |
| `ARCHIVE_BLOB_GRACE_SECONDS` | 86400 | how long an unreferenced blob is kept — see DESIGN.md |
| `ARCHIVE_WEB_ROOT` | unset | built frontend to serve; unset means API-only |

## Tests

Each drives a real server over HTTP; none needs a fixture or a mock.

```bash
./server/tests/smoke.sh    # auth + tus, incl. an interrupted and resumed upload
./server/tests/shares.sh   # shares, downloads, ranges, expiry sweep
./server/tests/chat.sh     # rooms, invitations, blocks, admin removal
```

On Windows, VS Code's own IntelliSense shows phantom errors in the C++ files: it has
neither `sqlite3.h` nor libstdc++ on its include path. Open the folder through the WSL
remote extension and use clangd, which reads the generated `compile_commands.json`.

## Deployment

A single host with Docker. Caddy terminates TLS and reverse-proxies everything to the
app; the app serves both the API and the built frontend.

First time:

```bash
git clone <repo> /srv/thearchive && cd /srv/thearchive/deploy
cp .env.example .env && $EDITOR .env      # domain, data dir, uid/gid
docker compose up -d --build
```

The domain must already resolve to the host — Caddy obtains a certificate on first
request, and ACME needs port 80 reachable.

Afterwards, every update is:

```bash
cd /srv/thearchive && git pull && docker compose -f deploy/docker-compose.yml up -d --build
```

Schema migrations run automatically at startup, so there is no separate migration step.

**Changes to `deploy/Caddyfile` are the exception.** The file is bind-mounted on its own,
and a single-file bind mount is pinned to the file's inode — but `git pull` replaces a
changed file with a new one, so the running container keeps reading the old config. The
update command above does not help either: `up -d` only recreates containers whose
compose definition changed. Neither fails; the edit is silently ignored. After pulling a
Caddyfile change, validate it and restart Caddy so the mount is re-resolved:

```bash
cd /srv/thearchive/deploy
docker run --rm -e ARCHIVE_DOMAIN=example.com -v "$PWD/Caddyfile:/etc/caddy/Caddyfile:ro" \
    caddy:2-alpine caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
docker compose restart caddy
```

`caddy reload` is not enough on its own, for the same reason — it would re-read the stale
file and report success. The restart costs a second or two without HTTPS; certificates
live in the `caddy_data` volume and are not re-issued.
Only `deploy/.env` is host-specific; nothing else in the repo needs editing per
deployment.

The app container publishes no ports of its own. A published container port is DNAT'd in
prerouting and never traverses the host firewall's input hook, so exposing it directly
would bypass the firewall — only Caddy is reachable.
