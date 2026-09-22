#!/usr/bin/env bash
#
# Share creation, the public download path, and the expiry sweep.
#
#   ./server/tests/shares.sh [path-to-binary]
#
# The server runs with a 2s GC interval so sweeps are observable, but a realistic blob
# grace period — the test backdates rows to trigger collection rather than shortening
# the grace, because a zero grace would delete freshly uploaded blobs mid-test, which is
# the very hazard the grace exists to prevent.
#
set -u
BIN="${1:-$HOME/build-thearchive/thearchive}"
# Pick a free port rather than a fixed one, so the suite never collides with a dev
# server someone already has running — and never silently tests against it.
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
BASE=http://127.0.0.1:$PORT
DATA=$(mktemp -d)
JAR=$(mktemp)
WORK=$(mktemp -d)
DB="$DATA/db/archive.db"
PASS=0; FAIL=0

cleanup() { kill "${PID:-}" 2>/dev/null; wait "${PID:-}" 2>/dev/null; rm -rf "$DATA" "$WORK" "$JAR"; }
trap cleanup EXIT

ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1  ($2)"; FAIL=$((FAIL+1)); }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1" "expected '$3', got '$2'"; }
hdr()  { local k; k=$(printf '%s' "$1" | tr 'A-Z' 'a-z'); tr -d '\r' \
         | awk -v k="$k:" 'tolower($1)==k {print $2; exit}'; }
jget() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"; }
# Parse properly rather than with a regex: `.*"id":\([0-9]*\)` is greedy, so on one-line
# JSON it yields the LAST id in the document, not the first. Harmless with a single file
# per share, silently wrong the moment there are two.
jfile() { python3 -c "import sys,json;print(json.load(sys.stdin)['files'][$1]['id'])"; }
sql()  { sqlite3 "$DB" "$1"; }
gcwait() { sleep 3; }

ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT \
ARCHIVE_GC_INTERVAL_SECONDS=2 ARCHIVE_BLOB_GRACE_SECONDS=3600 \
ARCHIVE_PUBLIC_URL=https://archive.example.com \
    "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "$BASE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

curl -s -o /dev/null -c "$JAR" -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' \
    -d '{"username":"alice","password":"correct-horse-battery"}'

# Uploads one file and echoes its upload id. Uploads are consumed by share creation,
# so each share needs a fresh one — identical content therefore exercises dedupe.
upload() {
    local file="$1" size meta loc
    size=$(stat -c%s "$file")
    meta="filename $(basename "$file" | base64 -w0),lastModified $(printf '1700000000000' | base64 -w0),relativePath $(printf 'sat/%s' "$(basename "$file")" | base64 -w0)"
    loc=$(curl -s -D - -o /dev/null -b "$JAR" -X POST "$BASE/files" \
        -H "Upload-Length: $size" -H "Upload-Metadata: $meta" | hdr Location)
    curl -s -o /dev/null -b "$JAR" -X PATCH "$BASE$loc" \
        -H 'Content-Type: application/offset+octet-stream' -H 'Upload-Offset: 0' \
        --data-binary "@$file"
    printf '%s' "${loc##*/}"
}

head -c 262144 /dev/urandom > "$WORK/night.bin"
WANT=$(sha256sum "$WORK/night.bin" | cut -d' ' -f1)

echo "=== share creation ==="
U1=$(upload "$WORK/night.bin")
resp=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$U1\"],\"title\":\"Saturday\",\"expires_days\":30,\"public\":true}")
TOK=$(printf '%s' "$resp" | jget token)
[ -n "$TOK" ] && ok "share created" || bad "share created" "$resp"
check "url uses ARCHIVE_PUBLIC_URL" "$(printf '%s' "$resp" | jget url)" \
    "https://archive.example.com/d/$TOK"
check "upload row consumed" "$(sql 'SELECT COUNT(*) FROM uploads;')" "0"
check "refcount taken" "$(sql 'SELECT refcount FROM blobs;')" "1"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/shares" \
    -H 'Content-Type: application/json' -d "{\"uploads\":[\"$U1\"]}")
check "spent upload cannot be reused" "$code" "400"

echo
echo "=== metadata ==="
meta=$(curl -s "$BASE/api/shares/$TOK")
check "title round-trips" "$(printf '%s' "$meta" | jget title)" "Saturday"
FID=$(printf '%s' "$meta" | jfile 0)
[ -n "$FID" ] && ok "file id present" || bad "file id present" "$meta"
printf '%s' "$meta" | grep -q '"uploaded_by":"alice"' && ok "provenance recorded" \
    || bad "provenance recorded" "no uploaded_by"
printf '%s' "$meta" | grep -q '"client_mtime":1700000000' && ok "client mtime preserved" \
    || bad "client mtime preserved" "absent"
printf '%s' "$meta" | grep -q '"relative_path":"sat/night.bin"' && ok "relative path kept" \
    || bad "relative path kept" "absent"
check "content type sniffed, not declared" \
    "$(printf '%s' "$meta" | grep -c 'application/octet-stream')" "1"

echo
echo "=== download ==="
curl -s -D "$WORK/h" -o "$WORK/got.bin" "$BASE/d/$TOK/$FID"
check "bytes are identical" "$(sha256sum "$WORK/got.bin" | cut -d' ' -f1)" "$WANT"
grep -qi 'content-disposition: attachment' "$WORK/h" && ok "served as attachment" \
    || bad "served as attachment" "$(grep -i content-disposition "$WORK/h")"
grep -qi 'x-content-type-options: nosniff' "$WORK/h" && ok "nosniff set" || bad "nosniff set" "absent"
grep -qi 'filename\*=UTF-8' "$WORK/h" && ok "RFC 6266 filename" || bad "RFC 6266 filename" "absent"

code=$(curl -s -o "$WORK/range.bin" -D "$WORK/rh" -w '%{http_code}' -r 0-1023 "$BASE/d/$TOK/$FID")
check "range request honoured" "$code" "206"
check "range length correct" "$(stat -c%s "$WORK/range.bin")" "1024"
grep -qi 'content-range: bytes 0-1023/262144' "$WORK/rh" && ok "Content-Range correct" \
    || bad "Content-Range correct" "$(grep -i content-range "$WORK/rh" | tr -d '\r')"
head -c 1024 "$WORK/night.bin" > "$WORK/expect1k"
check "ranged bytes match the original" \
    "$(sha256sum "$WORK/range.bin" | cut -d' ' -f1)" "$(sha256sum "$WORK/expect1k" | cut -d' ' -f1)"

curl -s -o "$WORK/suffix.bin" -r -512 "$BASE/d/$TOK/$FID"
tail -c 512 "$WORK/night.bin" > "$WORK/expecttail"
check "suffix range returns the tail" \
    "$(sha256sum "$WORK/suffix.bin" | cut -d' ' -f1)" "$(sha256sum "$WORK/expecttail" | cut -d' ' -f1)"

code=$(curl -s -o /dev/null -w '%{http_code}' -r 999999999-  "$BASE/d/$TOK/$FID")
check "unsatisfiable range is 416" "$code" "416"

code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=0-9,20-29' "$BASE/d/$TOK/$FID")
check "multi-range falls back to whole file" "$code" "200"

echo
echo "=== deduplication across shares ==="
U2=$(upload "$WORK/night.bin")
TOK2=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$U2\"],\"public\":true}" | jget token)
check "still one blob on disk" "$(find "$DATA/blobs" -type f | wc -l)" "1"
check "refcount is now two" "$(sql 'SELECT refcount FROM blobs;')" "2"

echo
echo "=== storage reporting ==="
jnum() { python3 -c "import sys,json;print(json.load(sys.stdin).get('$1',''))"; }
st=$(curl -s -b "$JAR" "$BASE/api/storage")
# Two live shares of one 256 KiB blob: counted twice logically, stored once.
check "logical counts each file" "$(printf '%s' "$st" | jnum shared_logical_bytes)" "524288"
check "stored counts the blob once" "$(printf '%s' "$st" | jnum shared_stored_bytes)" "262144"
check "my shares counted" "$(printf '%s' "$st" | jnum my_shares)" "2"
[ "$(printf '%s' "$st" | jnum disk_total)" -gt 0 ] && ok "disk total reported" \
    || bad "disk total reported" "absent"
check "storage requires a session" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/storage")" "401"

echo
echo "=== password and download limits ==="
U3=$(upload "$WORK/night.bin")
TOK3=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$U3\"],\"password\":\"night-out-2026\",\"max_downloads\":1,\"public\":true}" | jget token)
FID3=$(curl -s "$BASE/api/shares/$TOK3" -H 'X-Share-Password: night-out-2026' | jfile 0)

check "metadata needs the password" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOK3")" "401"
check "wrong password rejected" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOK3" -H 'X-Share-Password: nope')" "401"
check "password via header" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOK3" -H 'X-Share-Password: night-out-2026')" "200"
check "password via query param" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOK3?p=night-out-2026")" "200"
# A seek into the middle of a file is part of one logical download, not a new one —
# otherwise a video player would exhaust max_downloads within seconds of pressing play.
check "mid-file seek allowed" \
    "$(curl -s -o /dev/null -w '%{http_code}' -r 1024-2047 "$BASE/d/$TOK3/$FID3?p=night-out-2026")" "206"
check "seek did not consume the quota" "$(sql "SELECT download_count FROM shares WHERE token='$TOK3';")" "0"

check "first download allowed" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOK3/$FID3?p=night-out-2026")" "200"
check "download limit enforced" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOK3/$FID3?p=night-out-2026")" "410"
check "limit also blocks ranged requests" \
    "$(curl -s -o /dev/null -w '%{http_code}' -r 1024-2047 "$BASE/d/$TOK3/$FID3?p=night-out-2026")" "410"

echo
echo "=== revocation ==="
check "revoke succeeds" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X DELETE "$BASE/api/shares/$TOK2")" "200"
check "revoked link is gone" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOK2")" "404"
check "unknown token is also 404" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/ZZZZZZZZZZZZZZZZZZZZZZ")" "404"

echo
echo "=== revoking reclaims space without waiting out the grace period ==="
# Distinct content, so it is the only share holding this blob. It was uploaded seconds
# ago and the grace period is an hour, so if revocation did not exempt it the file would
# still be here after the sweep.
head -c 65536 /dev/urandom > "$WORK/solo.bin"
SOLO=$(sha256sum "$WORK/solo.bin" | cut -d' ' -f1)
U5=$(upload "$WORK/solo.bin")
TOK5=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$U5\"],\"title\":\"Solo\",\"public\":true}" | jget token)
SOLO_PATH="$DATA/blobs/${SOLO:0:2}/${SOLO:2:2}/$SOLO"
[ -f "$SOLO_PATH" ] && ok "solo blob stored" || bad "solo blob stored" "missing"

gcwait
[ -f "$SOLO_PATH" ] && ok "kept while its share is live" || bad "kept while its share is live" "deleted early"

curl -s -o /dev/null -b "$JAR" -X DELETE "$BASE/api/shares/$TOK5"
gcwait
[ -f "$SOLO_PATH" ] && bad "revoked blob deleted on next sweep" "still present" \
    || ok "revoked blob deleted on next sweep"
check "its row is gone too" "$(sql "SELECT COUNT(*) FROM blobs WHERE sha256='$SOLO';")" "0"

echo
echo "=== private by default ==="
UPV=$(upload "$WORK/night.bin")
respv=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$UPV\"],\"title\":\"Members only\"}")
TOKV=$(printf '%s' "$respv" | jget token)
check "defaults to private" "$(printf '%s' "$respv" | jget visibility)" "private"
check "anonymous cannot read metadata" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOKV")" "401"
check "and is told why, not just refused" \
    "$(curl -s "$BASE/api/shares/$TOKV" | jget reason)" "members_only"
check "a signed-in member can" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" "$BASE/api/shares/$TOKV")" "200"

VFID=$(curl -s -b "$JAR" "$BASE/api/shares/$TOKV" | jfile 0)
check "download blocked anonymously" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKV/$VFID")" "401"
check "zip blocked anonymously" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKV/all.zip")" "401"
check "preview blocked anonymously" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKV/$VFID/thumb")" "401"
check "inline blocked anonymously" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKV/$VFID/inline")" "401"
check "download works for a member" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" "$BASE/d/$TOKV/$VFID")" "200"

UPW=$(upload "$WORK/night.bin")
TOKW=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$UPW\"],\"public\":true}" | jget token)
check "opting out gives a public link" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOKW")" "200"

echo
echo "=== image previews ==="
# A real JPEG via the vips CLI, which ships with the library the server links anyway.
vips black "$WORK/shot.jpg" 1200 900 2>/dev/null
UP1=$(upload "$WORK/shot.jpg")
# Its own non-image file rather than reusing night.bin, so this section does not
# perturb the refcounts the garbage-collection checks below depend on.
head -c 40000 /dev/urandom > "$WORK/doc.bin"
UP2=$(upload "$WORK/doc.bin")
TOKP=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$UP1\",\"$UP2\"],\"title\":\"Previews\",\"public\":true}" | jget token)

meta=$(curl -s "$BASE/api/shares/$TOKP")
check "image marked previewable" "$(printf '%s' "$meta" | python3 -c "import sys,json;print(json.load(sys.stdin)['files'][0].get('preview',''))")" "image"
check "non-image has no preview" "$(printf '%s' "$meta" | python3 -c "import sys,json;print(json.load(sys.stdin)['files'][1].get('preview',''))")" ""
check "thumb recorded in the db" "$(sql "SELECT thumb FROM blobs WHERE thumb=1;")" "1"
check "both sizes on disk" "$(find "$DATA/thumbs" -name '*.webp' | wc -l)" "2"

PFID=$(printf '%s' "$meta" | jfile 0)
NFID=$(printf '%s' "$meta" | jfile 1)

h=$(curl -s -D - -o "$WORK/lg.webp" "$BASE/d/$TOKP/$PFID/thumb")
check "thumb served" "$(printf '%s' "$h" | head -1 | tr -d '\r' | awk '{print $2}')" "200"
check "served as webp" "$(printf '%s' "$h" | hdr Content-Type)" "image/webp"
check "served inline, not attachment" "$(printf '%s' "$h" | hdr Content-Disposition)" "inline"
printf '%s' "$h" | grep -qi 'x-content-type-options: nosniff' && ok "thumb sets nosniff" \
    || bad "thumb sets nosniff" "absent"
check "it really is a webp" "$(head -c 12 "$WORK/lg.webp" | tail -c 4)" "WEBP"

curl -s -o "$WORK/sm.webp" "$BASE/d/$TOKP/$PFID/thumb?s=sm"
[ "$(stat -c%s "$WORK/sm.webp")" -lt "$(stat -c%s "$WORK/lg.webp")" ] \
    && ok "sm is smaller than lg" || bad "sm is smaller than lg" "sm >= lg"

# The regression this feature most plausibly introduces: browsing a gallery must not
# consume the share's download allowance.
for _ in 1 2 3 4 5; do curl -s -o /dev/null "$BASE/d/$TOKP/$PFID/thumb"; done
curl -s -o /dev/null "$BASE/d/$TOKP/$PFID/inline"
check "previews do not count as downloads" \
    "$(sql "SELECT download_count FROM shares WHERE token='$TOKP';")" "0"

check "no thumb for a non-image" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKP/$NFID/thumb")" "404"
check "inline refuses a disallowed type" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKP/$NFID/inline")" "415"
check "inline serves an allowed image" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKP/$PFID/inline")" "200"
check "previews respect the share password" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOK3/1/thumb")" "401"

# Backfill: clear the recorded state as though the blob predated the column.
SHOT=$(sql "SELECT sha256 FROM blobs WHERE thumb=1;")
sql "UPDATE blobs SET thumb=0 WHERE sha256='$SHOT';"
find "$DATA/thumbs" -name "$SHOT*" -delete
# An un-rendered image must still be advertised as previewable, or the client never
# asks for the thumbnail and the backfill below can never fire.
check "un-rendered image still advertised" \
    "$(curl -s "$BASE/api/shares/$TOKP" | python3 -c "import sys,json;print(json.load(sys.stdin)['files'][0].get('preview',''))")" \
    "image"
check "backfills on first request" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOKP/$PFID/thumb")" "200"
check "and records it" "$(sql "SELECT thumb FROM blobs WHERE sha256='$SHOT';")" "1"

# The sweep renders leftovers without anyone having to open the page.
sql "UPDATE blobs SET thumb=0 WHERE sha256='$SHOT';"
find "$DATA/thumbs" -name "$SHOT*" -delete
gcwait
check "sweep backfills unattended" "$(sql "SELECT thumb FROM blobs WHERE sha256='$SHOT';")" "1"
[ -n "$(find "$DATA/thumbs" -name "$SHOT*" 2>/dev/null)" ] && ok "sweep wrote the files" \
    || bad "sweep wrote the files" "absent"

# Thumbnails are derived state: they must be reaped with the blob, by both paths.
curl -s -o /dev/null -b "$JAR" -X DELETE "$BASE/api/shares/$TOKP/files/$PFID"
[ -z "$(find "$DATA/thumbs" -name "$SHOT*" 2>/dev/null)" ] \
    && ok "thumbnails removed with their blob" \
    || bad "thumbnails removed with their blob" "$(find "$DATA/thumbs" -name "$SHOT*")"

echo
echo "=== download all as a streaming zip ==="
# Two files sharing a name, so the writer has to disambiguate rather than let one
# silently overwrite the other on extraction.
mkdir -p "$WORK/z"
head -c 120000 /dev/urandom > "$WORK/z/photo.jpg"
head -c  80000 /dev/urandom > "$WORK/z/clip.mp4"
UZ1=$(upload "$WORK/z/photo.jpg")
UZ2=$(upload "$WORK/z/clip.mp4")
# Same name, different bytes — two phones both producing photo.jpg.
cp "$WORK/z/photo.jpg" "$WORK/z/first-photo.jpg"
head -c 55000 /dev/urandom > "$WORK/z/photo.jpg"
UZ3=$(upload "$WORK/z/photo.jpg")
TOKZ=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$UZ1\",\"$UZ2\",\"$UZ3\"],\"title\":\"Night Out!\",\"public\":true}" | jget token)

hdrs=$(curl -s -D - -o "$WORK/all.zip" "$BASE/d/$TOKZ/all.zip")
check "zip served" "$(printf '%s' "$hdrs" | head -1 | tr -d '\r' | awk '{print $2}')" "200"
declared=$(printf '%s' "$hdrs" | hdr Content-Length)
actual=$(stat -c%s "$WORK/all.zip")
check "Content-Length matches the body exactly" "$actual" "$declared"
printf '%s' "$hdrs" | grep -qi 'content-disposition: attachment' && ok "zip is an attachment" \
    || bad "zip is an attachment" "absent"
printf '%s' "$hdrs" | grep -qi 'filename\*=UTF-8.*Night' && ok "archive named after the share" \
    || bad "archive named after the share" "$(printf '%s' "$hdrs" | grep -i disposition)"

unzip -t "$WORK/all.zip" >/dev/null 2>&1 && ok "archive passes unzip -t" \
    || bad "archive passes unzip -t" "$(unzip -t "$WORK/all.zip" 2>&1 | tail -2)"
check "holds three entries" "$(unzip -l "$WORK/all.zip" | tail -1 | awk '{print $2}')" "3"
unzip -l "$WORK/all.zip" | grep -q 'photo (2).jpg' && ok "duplicate name disambiguated" \
    || bad "duplicate name disambiguated" "$(unzip -l "$WORK/all.zip" | sed -n '4,7p')"

rm -rf "$WORK/out" && mkdir -p "$WORK/out" && unzip -qq "$WORK/all.zip" -d "$WORK/out"
check "extracted clip is byte-identical" \
    "$(sha256sum "$WORK/out/sat/clip.mp4" | cut -d' ' -f1)" \
    "$(sha256sum "$WORK/z/clip.mp4" | cut -d' ' -f1)"

check "the whole archive counts as one download" \
    "$(sql "SELECT download_count FROM shares WHERE token='$TOKZ';")" "1"
check "zip of a password-protected link needs the password" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/d/$TOK3/all.zip")" "401"

echo
echo "=== tus termination ==="
head -c 131072 /dev/urandom > "$WORK/cancel.bin"
CANCEL=$(sha256sum "$WORK/cancel.bin" | cut -d' ' -f1)
opts=$(curl -s -D - -o /dev/null -X OPTIONS "$BASE/files")
printf '%s' "$opts" | hdr Tus-Extension | grep -q termination && ok "termination advertised" \
    || bad "termination advertised" "$(printf '%s' "$opts" | hdr Tus-Extension)"

# Partial upload: created, one chunk sent, never finished.
PLOC=$(curl -s -D - -o /dev/null -b "$JAR" -X POST "$BASE/files" \
    -H "Upload-Length: 262144" -H "Upload-Metadata: filename $(printf 'cancel.bin' | base64 -w0)" \
    | hdr Location)
PID_UP="${PLOC##*/}"
curl -s -o /dev/null -b "$JAR" -X PATCH "$BASE$PLOC" \
    -H 'Content-Type: application/offset+octet-stream' -H 'Upload-Offset: 0' \
    --data-binary "@$WORK/cancel.bin"
[ -f "$DATA/incoming/$PID_UP" ] && ok "partial upload on disk" || bad "partial upload on disk" "missing"

check "terminate returns 204" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X DELETE "$BASE$PLOC")" "204"
[ -f "$DATA/incoming/$PID_UP" ] && bad "partial file removed at once" "still present" \
    || ok "partial file removed at once"
check "upload row gone" "$(sql "SELECT COUNT(*) FROM uploads WHERE id='$PID_UP';")" "0"
check "terminating twice is 404" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X DELETE "$BASE$PLOC")" "404"

# A completed-but-unshared upload: its blob must go too, without waiting out the grace.
U6=$(upload "$WORK/cancel.bin")
CPATH="$DATA/blobs/${CANCEL:0:2}/${CANCEL:2:2}/$CANCEL"
[ -f "$CPATH" ] && ok "completed upload produced a blob" || bad "completed upload produced a blob" "missing"
curl -s -o /dev/null -b "$JAR" -X DELETE "$BASE/files/$U6"
[ -f "$CPATH" ] && bad "its blob reclaimed immediately" "still present" \
    || ok "its blob reclaimed immediately"

echo
echo "=== removing one file from a share ==="
head -c 32768 /dev/urandom > "$WORK/a.bin"
head -c 49152 /dev/urandom > "$WORK/b.bin"
A=$(sha256sum "$WORK/a.bin" | cut -d' ' -f1)
UA=$(upload "$WORK/a.bin"); UB=$(upload "$WORK/b.bin")
TOK6=$(curl -s -b "$JAR" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
    -d "{\"uploads\":[\"$UA\",\"$UB\"],\"title\":\"Pair\",\"password\":\"hunter2\",\"public\":true}" | jget token)

# The owner is exempt from their own share password.
detail=$(curl -s -b "$JAR" "$BASE/api/shares/$TOK6")
printf '%s' "$detail" | grep -q '"is_owner":true' && ok "owner skips the share password" \
    || bad "owner skips the share password" "$detail"
check "a stranger still needs it" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOK6")" "401"

FA=$(printf '%s' "$detail" | jfile 0)
APATH="$DATA/blobs/${A:0:2}/${A:2:2}/$A"
res=$(curl -s -b "$JAR" -X DELETE "$BASE/api/shares/$TOK6/files/$FA")
check "one file removed, one remains" "$(printf '%s' "$res" | jnum files_remaining)" "1"
check "share not revoked yet" "$(printf '%s' "$res" | jnum share_revoked)" "False"
[ -f "$APATH" ] && bad "its blob reclaimed" "still present" || ok "its blob reclaimed"
check "share still resolves" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" "$BASE/api/shares/$TOK6")" "200"

FB=$(curl -s -b "$JAR" "$BASE/api/shares/$TOK6" | jfile 0)
res=$(curl -s -b "$JAR" -X DELETE "$BASE/api/shares/$TOK6/files/$FB")
check "removing the last file revokes the share" "$(printf '%s' "$res" | jnum share_revoked)" "True"
check "link is gone" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" "$BASE/api/shares/$TOK6")" "404"

check "cannot remove from someone else's share" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE/api/shares/$TOK/files/$FID")" "401"

echo
echo "=== garbage collection ==="
gcwait
NIGHT_PATH="$DATA/blobs/${WANT:0:2}/${WANT:2:2}/$WANT"
check "revoked share released its reference" \
    "$(sql "SELECT refcount FROM blobs WHERE sha256='$WANT';")" "4"
[ -f "$NIGHT_PATH" ] && ok "blob survives while still referenced" \
    || bad "blob survives while still referenced" "deleted early"

# The hazard the grace period exists for: a completed upload sits at refcount 0 until a
# share is made. A sweep must not collect it in that window.
U4=$(upload "$WORK/night.bin")
gcwait
check "unshared upload survives the sweep" "$(sql "SELECT COUNT(*) FROM uploads WHERE id='$U4';")" "1"
[ -f "$NIGHT_PATH" ] && ok "its blob is still on disk" \
    || bad "its blob is still on disk" "deleted"

# Backdate everything past expiry and past the grace period, then let the sweep run.
sql "UPDATE shares SET expires_at = strftime('%s','now') - 10;"
sql "UPDATE uploads SET expires_at = strftime('%s','now') - 10;"
sql "UPDATE blobs SET created_at = strftime('%s','now') - 100000;"
gcwait
check "all shares released" "$(sql 'SELECT COUNT(*) FROM shares WHERE released_at IS NULL;')" "0"
check "refcount drained" "$(sql 'SELECT COALESCE(SUM(refcount),0) FROM blobs;')" "0"
check "expired link 404s" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/shares/$TOK")" "404"
check "blob row deleted" "$(sql 'SELECT COUNT(*) FROM blobs;')" "0"
check "blob file deleted" "$(find "$DATA/blobs" -type f | wc -l)" "0"
check "share records kept for history" "$(sql 'SELECT COUNT(*) FROM shares;')" "9"
check "incoming left clean" "$(find "$DATA/incoming" -type f | wc -l)" "0"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -30 "$WORK/server.log"; }
exit $((FAIL > 0))
