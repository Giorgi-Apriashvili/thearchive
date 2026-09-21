# syntax=docker/dockerfile:1

# ---- frontend ---------------------------------------------------------------
FROM node:22-bookworm-slim AS web
WORKDIR /web
# Lockfile first so dependency installation caches independently of source edits.
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web/ ./
RUN npm run build

# ---- backend ----------------------------------------------------------------
# The build and runtime stages deliberately share a Debian release. The binary links
# jsoncpp, OpenSSL, SQLite, argon2 and brotli dynamically, so a glibc or soname mismatch
# between the two stages fails at startup rather than at build time.
FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev libsqlite3-dev \
        libargon2-dev libbrotli-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY server/ ./server/
# Drogon is fetched and built here, which is most of this stage's time. Docker caches
# the layer, so it only repeats when server/ changes.
RUN cmake -S server -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# ---- runtime ----------------------------------------------------------------
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libjsoncpp25 libuuid1 zlib1g libssl3 libsqlite3-0 libargon2-1 libbrotli1 \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/thearchive /usr/local/bin/thearchive
COPY --from=web /web/dist /srv/web

# Binds all interfaces because the only thing that can reach it is Caddy on the
# compose network; the container publishes no ports of its own.
ENV ARCHIVE_BIND=0.0.0.0 \
    ARCHIVE_PORT=8080 \
    ARCHIVE_WEB_ROOT=/srv/web \
    ARCHIVE_DATA_DIR=/data

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=20s \
    CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

ENTRYPOINT ["/usr/local/bin/thearchive"]
