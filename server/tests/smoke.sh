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
echo "=== changing your own password ==="
# `friend` is used for this and nothing else, so the password can move without
# disturbing the sections below. Two sessions, because the interesting part is which of
# them survives.
PWJAR=$(mktemp); PWJAR2=$(mktemp)
login_as() { curl -s -o /dev/null -w '%{http_code}' -c "$2" -X POST "$BASE/api/auth/login" \
             -H 'Content-Type: application/json' \
             -d "{\"username\":\"$1\",\"password\":\"$3\"}"; }
try_login() { curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/login" \
              -H 'Content-Type: application/json' \
              -d "{\"username\":\"$1\",\"password\":\"$2\"}"; }
setpw() { curl -s -o /dev/null -w '%{http_code}' -b "$1" -X POST "$BASE/api/auth/password" \
          -H 'Content-Type: application/json' \
          -d "{\"current_password\":\"$2\",\"new_password\":\"$3\"}"; }

check "first session opened" "$(login_as friend "$PWJAR" correct-horse-battery)" "200"
check "second session opened" "$(login_as friend "$PWJAR2" correct-horse-battery)" "200"

check "a password change needs a session" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/password" \
       -H 'Content-Type: application/json' \
       -d '{"current_password":"correct-horse-battery","new_password":"brand-new-secret"}')" "401"
check "wrong current password rejected" \
    "$(setpw "$PWJAR" not-the-right-one brand-new-secret)" "401"
check "new password below the minimum rejected" \
    "$(setpw "$PWJAR" correct-horse-battery short)" "400"
check "new password identical to the old rejected" \
    "$(setpw "$PWJAR" correct-horse-battery correct-horse-battery)" "400"
# None of those refusals may have half-applied.
check "the old password still works after a refused change" \
    "$(try_login friend correct-horse-battery)" "200"

check "password changed" "$(setpw "$PWJAR" correct-horse-battery brand-new-secret)" "200"
check "the old password no longer works" "$(try_login friend correct-horse-battery)" "401"
check "the new password does" "$(try_login friend brand-new-secret)" "200"
# The purge and its one exception, together.
check "the session that made the change survives" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$PWJAR" "$BASE/api/me")" "200"
check "every other session is ended" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$PWJAR2" "$BASE/api/me")" "401"

echo
echo "=== an admin can reset a password ==="
# The only recovery path in the system: nothing here sends mail, so a forgotten password
# has nowhere else to go.
uid() { curl -s -b "$JAR" "$BASE/api/admin/users" \
        | python3 -c "import sys,json;print(next(u['id'] for u in json.load(sys.stdin) if u['username']=='$1'))"; }
FRIEND_ID=$(uid friend)
ALICE_ID=$(uid alice)

check "an ordinary member cannot reset anyone" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" -X POST \
       "$BASE/api/admin/users/$FRIEND_ID/password")" "403"
check "an admin cannot reset their own this way" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST \
       "$BASE/api/admin/users/$ALICE_ID/password")" "409"

RESET=$(curl -s -b "$JAR" -X POST "$BASE/api/admin/users/$FRIEND_ID/password" \
        | python3 -c "import sys,json;print(json.load(sys.stdin)['password'])")
check "a password comes back, long enough to satisfy the minimum" \
    "$([ "${#RESET}" -ge 6 ] && echo yes || echo "no: '$RESET'")" "yes"
check "it works" "$(try_login friend "$RESET")" "200"
check "the password it replaced does not" "$(try_login friend brand-new-secret)" "401"
check "and their remaining session is gone too" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$PWJAR" "$BASE/api/me")" "401"
rm -f "$PWJAR" "$PWJAR2"

echo
echo "=== an admin can rename someone ==="
rename() { curl -s -o /dev/null -w '%{http_code}' -b "$1" -X POST \
           "$BASE/api/admin/users/$2/username" \
           -H 'Content-Type: application/json' -d "{\"username\":\"$3\"}"; }
NAMEJAR=$(mktemp)
check "a session to rename out from under" "$(login_as friend "$NAMEJAR" "$RESET")" "200"

check "an ordinary member cannot rename anyone" \
    "$(rename "$CASEJAR" "$FRIEND_ID" nope)" "403"
check "an invalid name is refused" "$(rename "$JAR" "$FRIEND_ID" ab)" "400"
check "so is one already taken" "$(rename "$JAR" "$FRIEND_ID" alice)" "409"
check "an unknown account 404s" "$(rename "$JAR" 999999 whoever)" "404"
check "renaming still 'friend'" "$(curl -s -b "$JAR" "$BASE/api/admin/users/$FRIEND_ID" \
    | python3 -c 'import sys,json;print(json.load(sys.stdin)["username"])')" "friend"

# Normalised on the way in, exactly as at registration.
check "renamed" "$(rename "$JAR" "$FRIEND_ID" '  GioRgi  ')" "200"
check "stored lowercased and trimmed" \
    "$(curl -s -b "$JAR" "$BASE/api/admin/users/$FRIEND_ID" \
       | python3 -c 'import sys,json;print(json.load(sys.stdin)["username"])')" "giorgi"
check "renaming to the name they already have is refused" \
    "$(rename "$JAR" "$FRIEND_ID" giorgi)" "400"

check "the old name no longer signs in" "$(try_login friend "$RESET")" "401"
check "the new one does" "$(try_login giorgi "$RESET")" "200"
# Sessions key on user_id, so a rename is not a reason to sign anyone out.
check "their existing session still works" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$NAMEJAR" "$BASE/api/me")" "200"
check "and reports the new name" \
    "$(curl -s -b "$NAMEJAR" "$BASE/api/me" \
       | python3 -c 'import sys,json;print(json.load(sys.stdin)["username"])')" "giorgi"
# The freed name is a name again.
check "the name they left behind can be taken" \
    "$(rename "$JAR" "$ALICE_ID" friend)" "200"
check "and alice is restored" "$(rename "$JAR" "$ALICE_ID" alice)" "200"
rm -f "$NAMEJAR"

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
check "privileged can mint invites" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" -X POST "$BASE/api/invites")" "200"
check "but still cannot reach the panel" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" "$BASE/api/admin/users")" "403"
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

# Back to an ordinary member for the checks below.
curl -s -o /dev/null -b "$JAR" -X POST "$BASE/api/admin/users/$MIXED/role" \
    -H 'Content-Type: application/json' -d '{"role":"user"}'

echo
echo "=== invites need privileged or above ==="
# mixedcase was demoted back to plain user below, so this is an ordinary member.
check "member cannot mint an invite" \
    "$(curl -s -o /dev/null -w '%{http_code}' -b "$CASEJAR" -X POST "$BASE/api/invites")" "403"
check "administrator can too" \
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
echo "=== password guessing is limited ==="
# Every request here comes from loopback, which is how production sees Caddy, so
# X-Forwarded-For is trusted and names the client. The addresses are from the
# documentation ranges (203.0.113.0/24, 198.51.100.0/24, 2001:db8::/32): public as far as
# the limiter is concerned, and nobody's real address.
#
# The two layers are tested apart, because either could mask the other. Per-address tests
# spread their failures over many usernames so no account is slowed; per-account tests
# spread theirs over many addresses so no address is refused.
login_from() {  # <client-address> <username> <password>  -> status code
    curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/login" \
        -H 'Content-Type: application/json' -H "X-Forwarded-For: $1" \
        -d "{\"username\":\"$2\",\"password\":\"$3\"}"
}
retry_after() {  # same arguments  -> the Retry-After header, or empty
    curl -s -D - -o /dev/null -X POST "$BASE/api/auth/login" \
        -H 'Content-Type: application/json' -H "X-Forwarded-For: $1" \
        -d "{\"username\":\"$2\",\"password\":\"$3\"}" | hdr Retry-After
}
fail_many() {  # <client-address> <count> <username-prefix>
    for n in $(seq 1 "$2"); do login_from "$1" "$3$n" wrong >/dev/null; done
}

# A dedicated account, so slowing it cannot disturb anything above.
tinvite=$(curl -s -b "$JAR" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
curl -s -o /dev/null -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
    -d "{\"invite\":\"$tinvite\",\"username\":\"target\",\"password\":\"right-password\"}"

echo "  -- per client address --"
fail_many 203.0.113.10 10 ipuser
check "the 11th failure from one address is refused" \
    "$(login_from 203.0.113.10 ipuser11 wrong)" "429"
# Refused before the password is looked at: no Argon2 is spent, and a guesser learns
# nothing even when they happen to be right.
check "even with the right password" "$(login_from 203.0.113.10 target right-password)" "429"
RA=$(retry_after 203.0.113.10 target right-password)
[ -n "$RA" ] && [ "$RA" -gt 800 ] && [ "$RA" -le 900 ] && ok "Retry-After says when (${RA}s of a 15-minute window)" \
    || bad "Retry-After says when" "got '$RA'"
check "another address is unaffected" "$(login_from 203.0.113.11 target right-password)" "200"
# Successes are not guesses, however many there are.
for n in $(seq 1 12); do c=$(login_from 203.0.113.12 target right-password); [ "$c" = 200 ] || break; done
check "twelve correct sign-ins from one address are all allowed" "$c" "200"

# The rightmost entry is the one the proxy added; anything to its left came from the client.
check "the rightmost forwarded address is the one held to account" \
    "$(login_from '198.51.100.1, 203.0.113.10' target right-password)" "429"
check "a client cannot borrow another address by prepending it" \
    "$(login_from '203.0.113.10, 198.51.100.1' target right-password)" "200"

# IPv6: a subscriber is handed a whole /64, so the /64 is what is held to account.
for n in $(seq 1 10); do login_from "2001:db8:aa:1::$n" "v6user$n" wrong >/dev/null; done
check "a different address in the same IPv6 /64 is refused" \
    "$(login_from 2001:db8:aa:1::ffff target right-password)" "429"
check "the neighbouring /64 is not" "$(login_from 2001:db8:aa:2::1 target right-password)" "200"
check "an IPv4 client on a dual-stack socket is the IPv4 address" \
    "$(login_from ::ffff:203.0.113.10 target right-password)" "429"

# The safe degradation. A private address is a network position, not a client — behind
# Docker it can be the gateway every visitor shares — so limiting on it would lock
# everybody out at once. Per-address limiting stands aside instead.
fail_many 10.0.0.5 12 privuser
check "failures from a private address never trigger the per-address limit" \
    "$(login_from 10.0.0.5 privuser13 wrong)" "401"
for n in $(seq 1 12); do curl -s -o /dev/null -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
    -d "{\"username\":\"nofwd$n\",\"password\":\"wrong\"}"; done
check "nor from loopback with no forwarded address at all" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
       -d '{"username":"nofwd13","password":"wrong"}')" "401"

echo "  -- per account --"
# Five failures from five addresses: no address is anywhere near its limit, so what
# follows is the per-account layer alone.
for n in 1 2 3 4 5; do login_from "198.51.100.$((20 + n))" target wrong >/dev/null; done
check "after five failures the account is slowed, even for the right password" \
    "$(login_from 198.51.100.30 target right-password)" "429"
check "for one second" "$(retry_after 198.51.100.31 target right-password)" "1"
sleep 1.2
check "then the right password gets in" "$(login_from 198.51.100.32 target right-password)" "200"
check "and success spends the slowdown" "$(login_from 198.51.100.33 target wrong)" "401"

for n in 1 2 3 4 5; do login_from "198.51.100.$((40 + n))" target wrong >/dev/null; done
sleep 1.2
login_from 198.51.100.46 target wrong >/dev/null
check "each further failure doubles the wait" "$(retry_after 198.51.100.47 target right-password)" "2"
sleep 2.2
check "which also passes" "$(login_from 198.51.100.48 target right-password)" "200"

# Throttling only real accounts would tell a guesser which names exist.
for n in 1 2 3 4 5; do login_from "198.51.100.$((60 + n))" nobody-by-this-name wrong >/dev/null; done
check "a name that does not exist is slowed exactly the same" \
    "$(login_from 198.51.100.70 nobody-by-this-name wrong)" "429"

echo "  -- change password shares the account's limit --"
TJAR=$(mktemp)
curl -s -o /dev/null -c "$TJAR" -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
    -H 'X-Forwarded-For: 198.51.100.80' -d '{"username":"target","password":"right-password"}'
chpw() {  # <client-address> <current>
    curl -s -o /dev/null -w '%{http_code}' -b "$TJAR" -X POST "$BASE/api/auth/password" \
        -H 'Content-Type: application/json' -H "X-Forwarded-For: $1" \
        -d "{\"current_password\":\"$2\",\"new_password\":\"another-password\"}"
}
for n in 1 2 3 4 5; do chpw "198.51.100.$((80 + n))" wrong >/dev/null; done
check "a stolen session cannot guess the current password unthrottled" \
    "$(chpw 198.51.100.90 right-password)" "429"
check "and it is the same limit as signing in" "$(login_from 198.51.100.91 target right-password)" "429"
rm -f "$TJAR"

echo "  -- the log --"
# The username reaches the log unvalidated; a newline in it must not start a line.
curl -s -o /dev/null -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
    -H 'X-Forwarded-For: 198.51.100.99' -d '{"username":"x\nFORGED-LINE admin signed in","password":"no"}'
sleep 0.3
grep -qi '^forged-line' "$WORK/server.log" && bad "a username cannot forge a log line" "it did" \
    || ok "a username cannot forge a log line"
grep -q 'failed password for user:x?forged-line' "$WORK/server.log" \
    && ok "the failure is logged with the newline neutralised" \
    || bad "the failure is logged with the newline neutralised" "$(grep -i 'forged' "$WORK/server.log" | head -2)"
grep -q 'failed password for user:ipuser1 from 203.0.113.10' "$WORK/server.log" \
    && ok "failures are logged with the client address" \
    || bad "failures are logged with the client address" "$(grep 'failed password' "$WORK/server.log" | head -2)"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -25 "$WORK/server.log"; }
exit $((FAIL > 0))
