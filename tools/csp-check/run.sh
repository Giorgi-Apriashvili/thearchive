#!/usr/bin/env bash
#
# Runs the app under its Content-Security-Policy in a real browser and fails on any
# violation. Run it after any frontend change, before deploying.
#
#   tools/csp-check/run.sh [path-to-binary]
#
# Why this exists: a CSP fails silently. A script, style or font it blocks simply does
# not happen, and the symptom is a feature that quietly stops working in somebody
# else's browser. The API test suites check that the header is sent; only a browser can
# check that the frontend actually works under it.
#
# The browser walks every part of the app that does something CSP governs — dynamic
# style bindings, Svelte transitions, thumbnails, chat, the control panel — and records
# each securitypolicyviolation event. It ends with a deliberate violation as a control,
# because a detector that never fires proves nothing.
#
# Needs, once:
#   (cd tools/csp-check && npm install && npx playwright install chromium)
# plus the libraries that browser links against. On Arch:
#   pacman -S --needed nspr nss at-spi2-core alsa-lib libxcomposite libxdamage \
#       libxfixes libxrandr libxkbcommon mesa
# and a built frontend (cd web && npm run build) and server binary.
#
# Chromium only. Safari and Firefox are covered in production by the report endpoint
# instead: a violation in any browser is POSTed to /api/csp-report and logged.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BIN="${1:-$HOME/build-thearchive/thearchive}"

[ -x "$BIN" ] || { echo "no server binary at $BIN"; exit 2; }
[ -f "$REPO/web/dist/index.html" ] || { echo "no built frontend; run: cd web && npm run build"; exit 2; }
[ -d "$HERE/node_modules/playwright" ] || { echo "run once: cd tools/csp-check && npm install && npx playwright install chromium"; exit 2; }

DATA=$(mktemp -d)
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
BASE=http://127.0.0.1:$PORT
cleanup() { kill "${PID:-}" 2>/dev/null; wait "${PID:-}" 2>/dev/null; rm -rf "$DATA"; }
trap cleanup EXIT

ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT \
ARCHIVE_WEB_ROOT="$REPO/web/dist" ARCHIVE_PUBLIC_URL=$BASE \
    "$BIN" >"$DATA/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do curl -sf "$BASE/healthz" >/dev/null && break; sleep 0.25; done

# A real JPEG, so libvips renders real thumbnails and the lightbox path runs for real.
vips gaussnoise "$DATA/noise.v" 1200 800 2>/dev/null
vips copy "$DATA/noise.v" "$DATA/photo.jpg" 2>/dev/null
[ -s "$DATA/photo.jpg" ] || { echo "could not generate a test image (is the vips CLI installed?)"; exit 2; }

JAR="$DATA/jar"; PW=correct-horse-battery
curl -s -o /dev/null -c "$JAR" -X POST "$BASE/api/auth/bootstrap" -H 'Content-Type: application/json' \
    -d "{\"username\":\"alice\",\"password\":\"$PW\"}"
INVITE=$(curl -s -b "$JAR" -X POST "$BASE/api/invites" | python3 -c 'import sys,json;print(json.load(sys.stdin)["code"])')
curl -s -o /dev/null -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
    -d "{\"invite\":\"$INVITE\",\"username\":\"bob\",\"password\":\"$PW\"}"

# A link to a fresh upload of the photo, with the given title. Echoes its token.
make_link() {
    local size loc
    size=$(stat -c%s "$DATA/photo.jpg")
    loc=$(curl -s -D - -o /dev/null -b "$JAR" -X POST "$BASE/files" -H "Tus-Resumable: 1.0.0" \
          -H "Upload-Length: $size" -H "Upload-Metadata: filename $(printf photo.jpg | base64 -w0)" \
          | tr -d '\r' | awk 'tolower($1)=="location:"{print $2}')
    curl -s -o /dev/null -b "$JAR" -X PATCH "$BASE$loc" -H "Tus-Resumable: 1.0.0" \
        -H 'Content-Type: application/offset+octet-stream' -H 'Upload-Offset: 0' --data-binary "@$DATA/photo.jpg"
    curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
        -d "{\"uploads\":[\"${loc##*/}\"],\"title\":\"$1\",\"public\":true}" \
        | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])'
}
TOKEN=$(make_link "Night out")
make_link "Day trip" >/dev/null

(cd "$HERE" && BASE=$BASE TOKEN=$TOKEN PHOTO="$DATA/photo.jpg" node check.js)
RC=$?

# The control's report should have reached the server; if it did not, the reporting
# path is broken and production violations would go unseen.
echo
if grep -q "csp violation: style-src-attr" "$DATA/server.log"; then
    echo "  report endpoint: the control's violation reached the server log"
else
    echo "  report endpoint: the control's violation was NOT logged"
    RC=1
fi
exit $RC
