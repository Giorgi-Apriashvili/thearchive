#!/usr/bin/env bash
#
# End-to-end test against a running binary. Exercises invite-only registration, session
# handling, and a tus upload that is deliberately interrupted and resumed from the
# offset the server reports — which is the behaviour the whole protocol exists for.
#
#   ./server/tests/smoke.sh [path-to-binary]
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
PASS=0; FAIL=0

cleanup() { kill "${PID:-}" 2>/dev/null; wait "${PID:-}" 2>/dev/null; rm -rf "$DATA" "$WORK" "$JAR"; }
trap cleanup EXIT

ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1  ($2)"; FAIL=$((FAIL+1)); }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1" "expected '$3', got '$2'"; }

# Drogon lowercases response header names (legal, and mandatory in HTTP/2), so every
# lookup here is case-insensitive.
hdr() { local k; k=$(printf '%s' "$1" | tr 'A-Z' 'a-z'); tr -d '\r' \
        | awk -v k="$k:" 'tolower($1)==k {print $2; exit}'; }

# Secure cookies are discarded by clients over plain http, so login would never persist.
ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT \
    "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "$BASE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

echo "=== auth ==="
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' -d '{"username":"alice","password":"short"}')
check "password below the minimum rejected" "$code" "400"

code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR" -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' -d '{"username":"alice","password":"correct-horse-battery"}')
check "bootstrap first admin" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' -d '{"username":"someone","password":"correct-horse-battery"}')
check "bootstrap refused once initialised" "$code" "403"

check "session works" "$(curl -s -b "$JAR" "$BASE/api/me")" '{"role":"admin","username":"alice"}'
check "no cookie is rejected" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/me")" "401"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/login" \
    -H 'Content-Type: application/json' -d '{"username":"alice","password":"wrong-password-here"}')
check "wrong password rejected" "$code" "401"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"invite":"totally-made-up","username":"mallory","password":"correct-horse-battery"}')
check "bogus invite rejected" "$code" "403"

invite=$(curl -s -b "$JAR" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"invite\":\"$invite\",\"username\":\"friend\",\"password\":\"correct-horse-battery\"}")
check "real invite accepted" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"invite\":\"$invite\",\"username\":\"second\",\"password\":\"correct-horse-battery\"}")
check "invite cannot be reused" "$code" "403"

echo
echo "=== usernames are case-insensitive ==="
mixed=$(curl -s -b "$JAR" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
CASEJAR=$(mktemp)
code=$(curl -s -o /dev/null -w '%{http_code}' -c "$CASEJAR" -X POST "$BASE/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"invite\":\"$mixed\",\"username\":\"  MixedCase  \",\"password\":\"correct-horse-battery\"}")
check "mixed case accepted at registration" "$code" "200"
check "stored lowercased and trimmed" "$(curl -s -b "$CASEJAR" "$BASE/api/me")" \
    '{"role":"user","username":"mixedcase"}'

for attempt in mixedcase MIXEDCASE MixedCase; do
    check "login as '$attempt'" \
        "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/login" \
           -H 'Content-Type: application/json' \
           -d "{\"username\":\"$attempt\",\"password\":\"correct-horse-battery\"}")" "200"
done

# The reason the restriction exists: two accounts differing only by case would be
# distinct rows, and files carry uploaded_by.
dupe=$(curl -s -b "$JAR" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
check "cannot register a case variant of an existing name" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/register" \
       -H 'Content-Type: application/json' \
       -d "{\"invite\":\"$dupe\",\"username\":\"MIXEDCASE\",\"password\":\"correct-horse-battery\"}")" "409"
echo
echo "=== control panel ==="
ME=$(curl -s -b "$JAR" "$BASE/api/me" | python3 -c "import sys,json;print(json.load(sys.stdin)['role'])")
check "bootstrap account is admin" "$ME" "admin"

check "members cannot reach the panel" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" "$BASE/api/admin/users")" "403"
check "anonymous cannot either" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/admin/users")" "401"

users=$(curl -s -b "$JAR" "$BASE/api/admin/users")
X
printf '%s' "$users" | grep -q '"invited_by":"alice"' && ok "invite chain surfaced" \
    || bad "invite chain surfaced" "no invited_by"

MIXED=$(printf '%s' "$users" | python3 -c "import sys,json;print([u['id'] for u in json.load(sys.stdin) if u['username']=='mixedcase'][0])")
detail=$(curl -s -b "$JAR" "$BASE/api/admin/users/$MIXED")
printf '%s' "$detail" | grep -q '"shares"' && ok "user detail includes their shares" \
    || bad "user detail includes their shares" "$detail"

# The placeholder tier: assignable now, inert until someone decides what it means.
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/admin/users/$MIXED/role" \
    -H 'Content-Type: application/json' -d '{"role":"privileged"}')
check "can promote to privileged" "$code" "200"
check "role persisted" "$(curl -s -b "$JAR" "$BASE/api/admin/users/$MIXED" | python3 -c 'import sys,json;print(json.load(sys.stdin)["role"])')" "privileged"
check "privileged still cannot mint invites" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" -X POST "$BASE/api/invites")" "403"
check "bogus role rejected" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/admin/users/$MIXED/role" \
       -H 'Content-Type: application/json' -d '{"role":"superuser"}')" "400"

# Disabling must take effect against a session that already exists.
check "disable succeeds" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/admin/users/$MIXED/disable")" "200"
check "their live session stops working" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" "$BASE/api/me")" "401"
check "and they cannot sign back in" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/login" \
       -H 'Content-Type: application/json' -d '{"username":"mixedcase","password":"correct-horse-battery"}')" "403"
check "re-enabling restores login" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/admin/users/$MIXED/enable")" "200"
# Re-establishes the cookie jar as well: disabling deleted their sessions, so later
# checks using this jar would otherwise see 401 rather than the 403 they are asserting.
check "login works again" \
    "$(curl -s -o /dev/null -w '%{http_code}' -c "$CASEJAR" -X POST "$BASE/api/auth/login" \
       -H 'Content-Type: application/json' -d '{"username":"mixedcase","password":"correct-horse-battery"}')" "200"

# The lockout guards. Losing the last admin has no recovery short of editing the database.
MYID=$(printf '%s' "$users" | python3 -c "import sys,json;print([u['id'] for u in json.load(sys.stdin) if u['username']=='alice'][0])")
check "cannot demote the only admin" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/admin/users/$MYID/role" \
       -H 'Content-Type: application/json' -d '{"role":"user"}')" "409"
check "cannot disable yourself" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/admin/users/$MYID/disable")" "409"
check "cannot delete yourself" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X DELETE "$BASE/api/admin/users/$MYID")" "409"

echo
echo "=== invites are admin-only ==="
# mixedcase joined by invite, so it is an ordinary member, not an administrator.
check "member cannot mint an invite" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" -X POST "$BASE/api/invites")" "403"
check "administrator can" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/api/invites")" "200"
check "anonymous is still 401, not 403" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/invites")" "401"
rm -f "$CASEJAR"

echo
# Exactly at the minimum, so the boundary is pinned rather than merely "long enough".
sixchar=$(curl -s -b "$JAR" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"invite\":\"$sixchar\",\"username\":\"sixer\",\"password\":\"abcdef\"}")
check "six-character password accepted" "$code" "200"

echo
echo "=== tus upload, interrupted and resumed ==="
head -c 5242880 /dev/urandom > "$WORK/night-out.bin"
WANT=$(sha256sum "$WORK/night-out.bin" | cut -d' ' -f1)
dd if="$WORK/night-out.bin" of="$WORK/part1" bs=1M count=2 status=none
dd if="$WORK/night-out.bin" of="$WORK/part2" bs=1M skip=2 status=none

opts=$(curl -s -D - -o /dev/null -X OPTIONS "$BASE/files")
check "OPTIONS advertises tus version" "$(printf '%s' "$opts" | hdr Tus-Version)" "1.0.0"
check "OPTIONS advertises extensions" "$(printf '%s' "$opts" | hdr Tus-Extension)" \
    "creation,termination"
[ -n "$(printf '%s' "$opts" | hdr Tus-Max-Size)" ] && ok "OPTIONS advertises max size" \
    || bad "OPTIONS advertises max size" "absent"

meta="filename $(printf 'night-out.bin' | base64 -w0)"
loc=$(curl -s -D - -o /dev/null -b "$JAR" -X POST "$BASE/files" \
    -H "Upload-Length: 5242880" -H "Upload-Metadata: $meta" | hdr Location)
[ -n "$loc" ] && ok "POST /files returns Location" || bad "POST /files returns Location" "empty"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X PATCH "$BASE$loc" \
    -H 'Content-Type: application/offset+octet-stream' -H 'Upload-Offset: 0' \
    --data-binary "@$WORK/part1")
check "first chunk accepted" "$code" "204"

# The interruption: the client does not know how much landed, so it asks.
offset=$(curl -s -D - -o /dev/null -b "$JAR" -X HEAD "$BASE$loc" | hdr Upload-Offset)
check "HEAD reports resume offset" "$offset" "2097152"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X PATCH "$BASE$loc" \
    -H 'Content-Type: application/offset+octet-stream' -H 'Upload-Offset: 0' \
    --data-binary "@$WORK/part2")
check "stale offset rejected with 409" "$code" "409"

blob=$(curl -s -D - -o /dev/null -b "$JAR" -X PATCH "$BASE$loc" \
    -H 'Content-Type: application/offset+octet-stream' -H "Upload-Offset: $offset" \
    --data-binary "@$WORK/part2" | hdr X-Archive-Blob)
check "resumed upload completes, hash matches" "$blob" "$WANT"

STORED="$DATA/blobs/${WANT:0:2}/${WANT:2:2}/$WANT"
[ -f "$STORED" ] && ok "blob stored content-addressed" || bad "blob stored content-addressed" "missing"
check "stored bytes byte-identical" "$(sha256sum "$STORED" | cut -d' ' -f1)" "$WANT"

echo
echo "=== deduplication ==="
loc2=$(curl -s -D - -o /dev/null -b "$JAR" -X POST "$BASE/files" \
    -H "Upload-Length: 5242880" -H "Upload-Metadata: $meta" | hdr Location)
blob2=$(curl -s -D - -o /dev/null -b "$JAR" -X PATCH "$BASE$loc2" \
    -H 'Content-Type: application/offset+octet-stream' -H 'Upload-Offset: 0' \
    --data-binary "@$WORK/night-out.bin" | hdr X-Archive-Blob)
check "same content yields same blob" "$blob2" "$WANT"
check "stored exactly once" "$(find "$DATA/blobs" -type f | wc -l)" "1"
check "no incoming files left behind" "$(find "$DATA/incoming" -type f | wc -l)" "0"
check "blob row count" "$(sqlite3 "$DATA/db/archive.db" 'SELECT COUNT(*) FROM blobs;')" "1"

echo
echo "=== limits and isolation ==="
code=$(curl -s -o /dev/null -w '%{http_code}' -X PATCH "$BASE$loc" \
    -H 'Content-Type: application/offset+octet-stream' -H 'Upload-Offset: 0' --data-binary 'x')
check "unauthenticated PATCH rejected" "$code" "401"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST "$BASE/files" \
    -H "Upload-Length: 999999999999999")
check "oversized upload refused" "$code" "413"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X HEAD "$BASE/files/does-not-exist")
check "unknown upload is 404" "$code" "404"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -25 "$WORK/server.log"; }
exit $((FAIL > 0))
