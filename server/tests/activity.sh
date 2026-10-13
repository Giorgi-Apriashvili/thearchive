#!/usr/bin/env bash
#
# The activity log: what is recorded, what never is, who can read it, and how long.
#
#   ./server/tests/activity.sh [path-to-binary]
#
# alice is the administrator; bob is an ordinary member. Requests come from loopback,
# which is how production sees Caddy, so X-Forwarded-For names the client — the
# documentation ranges (203.0.113.0/24, 2001:db8::/32) stand in for real addresses.
set -u
BIN="${1:-$HOME/build-thearchive/thearchive}"
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
BASE=http://127.0.0.1:$PORT
DATA=$(mktemp -d)
WORK=$(mktemp -d)
ALICE=$(mktemp); BOB=$(mktemp)
DB="$DATA/db/archive.db"
PASS=0; FAIL=0

cleanup() {
    kill "${PID:-}" 2>/dev/null; wait "${PID:-}" 2>/dev/null
    rm -rf "$DATA" "$WORK" "$ALICE" "$BOB"
}
trap cleanup EXIT

ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1  ($2)"; FAIL=$((FAIL+1)); }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1" "expected '$3', got '$2'"; }
sql()  { sqlite3 "$DB" "$1"; }
py()   { python3 -c "$1"; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
hdr()  { local k; k=$(printf '%s' "$1" | tr 'A-Z' 'a-z'); tr -d '\r' | awk -v k="$k:" 'tolower($1)==k {print $2; exit}'; }
# The newest activity rows, as "event|actor|detail|network", newest first.
latest() { sql "SELECT event || '|' || actor || '|' || detail || '|' || COALESCE(network, '') FROM activity ORDER BY id DESC LIMIT ${1:-1};"; }
# The admin endpoint, with a query string, through a python expression over `items`.
feed() { curl -s -b "$ALICE" "$BASE/api/admin/activity$1" | py "import sys,json;d=json.load(sys.stdin);items=d['items'];print($2)"; }

ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT ARCHIVE_GC_INTERVAL_SECONDS=2 \
    "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "$BASE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

echo "=== signing in ==="
curl -s -o /dev/null -c "$ALICE" -X POST "$BASE/api/auth/bootstrap" -H 'X-Forwarded-For: 203.0.113.77' \
    -H 'Content-Type: application/json' -d '{"username":"alice","password":"correct-horse-battery"}'
check "setting up the site is recorded, from a shortened address" "$(latest)" "auth.bootstrap|alice||203.0.113.0/24"
INVITE=$(curl -s -b "$ALICE" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
check "so is making an invitation" "$(latest | cut -d'|' -f1,2)" "invite.created|alice"
curl -s -o /dev/null -c "$BOB" -X POST "$BASE/api/auth/register" -H 'X-Forwarded-For: 2001:db8:1:2::5' \
    -H 'Content-Type: application/json' -H "User-Agent: $(py "print('A'*500)")" \
    -d "{\"invite\":\"$INVITE\",\"username\":\"bob\",\"password\":\"correct-horse-battery\"}"
check "joining is recorded; IPv6 shortened to /48" "$(latest)" "auth.register|bob||2001:db8:1::/48"
check "the browser string is kept, cut short" "$(sql "SELECT length(user_agent) FROM activity ORDER BY id DESC LIMIT 1;")" "200"

login() { code -X POST "$BASE/api/auth/login" -H 'X-Forwarded-For: 198.51.100.9' \
          -H 'Content-Type: application/json' -d "{\"username\":\"$1\",\"password\":\"$2\"}"; }
login bob wrong-password-1 >/dev/null
check "a wrong password for a real account names it" "$(latest)" "auth.login_failed|bob||198.51.100.0/24"
login hunter2-oops whatever >/dev/null
check "an unknown name is not kept" "$(latest)" "auth.login_failed||no such account|198.51.100.0/24"
check "anywhere in the log" "$(sql "SELECT COUNT(*) FROM activity WHERE actor LIKE '%hunter2%' OR detail LIKE '%hunter2%';")" "0"
login bob correct-horse-battery >/dev/null
check "a successful sign-in is recorded" "$(latest | cut -d'|' -f1,2)" "auth.login|bob"
code -b "$BOB" -X POST "$BASE/api/auth/password" -H 'Content-Type: application/json' \
    -d '{"current_password":"nope","new_password":"another-good-one"}' >/dev/null
check "a failed password change too" "$(latest | cut -d'|' -f1,2)" "auth.password_change_failed|bob"

echo
echo "=== links ==="
head -c 4096 /dev/urandom > "$WORK/a.bin"
upload() {
    local jar="$1" file="$2" size loc
    size=$(stat -c%s "$file")
    loc=$(curl -s -D - -o /dev/null -b "$jar" -X POST "$BASE/files" -H "Upload-Length: $size" \
          -H "Upload-Metadata: filename $(printf '%s' "$(basename "$file")" | base64 -w0)" | hdr Location)
    curl -s -o /dev/null -b "$jar" -X PATCH "$BASE$loc" -H 'Content-Type: application/offset+octet-stream' \
        -H 'Upload-Offset: 0' --data-binary "@$file"
    printf '%s' "${loc##*/}"
}
U=$(upload "$ALICE" "$WORK/a.bin")
check "a finished upload is recorded" "$(latest | cut -d'|' -f1,2,3)" "upload.completed|alice|a.bin · 4096 bytes"
TOK=$(curl -s -b "$ALICE" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
      -d "{\"uploads\":[\"$U\"],\"title\":\"Lake\",\"public\":true,\"password\":\"hunter22\"}" | py "import sys,json;print(json.load(sys.stdin)['token'])")
check "making a link, by its title" "$(latest | cut -d'|' -f1,2,3)" "link.created|alice|“Lake” · 1 file(s) · expires in 30 day(s)"
FID=$(curl -s -b "$ALICE" "$BASE/api/shares/$TOK" | py "import sys,json;print(json.load(sys.stdin)['files'][0]['id'])")
check "the owner opening it is not worth a record" "$(latest | cut -d'|' -f1)" "link.created"
code -H 'X-Share-Password: wrong' "$BASE/api/shares/$TOK" >/dev/null
check "a wrong share password, by someone not signed in" "$(latest | cut -d'|' -f1,2,3)" "link.password_failed||“Lake”"
code -b "$BOB" -H 'X-Share-Password: hunter22' "$BASE/api/shares/$TOK" >/dev/null
check "a member opening it" "$(latest | cut -d'|' -f1,2,3)" "link.opened|bob|“Lake”"
code -b "$BOB" -H 'X-Share-Password: hunter22' "$BASE/d/$TOK/$FID" >/dev/null
check "and downloading a file" "$(latest | cut -d'|' -f1,2,3)" "link.downloaded|bob|“Lake”: a.bin"
code -H 'X-Share-Password: hunter22' "$BASE/d/$TOK/all.zip" >/dev/null
check "someone not signed in downloading everything" "$(latest | cut -d'|' -f1,2,3)" "link.downloaded||“Lake”: everything, as a zip"
code -b "$ALICE" -X PATCH "$BASE/api/shares/$TOK" -H 'Content-Type: application/json' -d '{"expires_days":7,"visibility":"private"}' >/dev/null
check "changing it" "$(latest | cut -d'|' -f1,3)" "link.changed|“Lake”: private, expires in 7 day(s)"
# The point of all the care: nothing here is a way into anything.
check "no link token anywhere in the log" "$(sql "SELECT COUNT(*) FROM activity WHERE detail LIKE '%$TOK%' OR user_agent LIKE '%$TOK%';")" "0"
check "no share password either" "$(sql "SELECT COUNT(*) FROM activity WHERE detail LIKE '%hunter22%' OR detail LIKE '%wrong%';")" "0"
code -b "$ALICE" -X DELETE "$BASE/api/shares/$TOK" >/dev/null
check "removing it" "$(latest | cut -d'|' -f1,2,3)" "link.removed|alice|“Lake”"

echo
echo "=== administrators' actions ==="
BOBID=$(sql "SELECT id FROM users WHERE username='bob';")
code -b "$ALICE" -X POST "$BASE/api/admin/users/$BOBID/role" -H 'Content-Type: application/json' -d '{"role":"privileged"}' >/dev/null
check "a role change" "$(latest | cut -d'|' -f1,2,3)" "admin.role|alice|bob: privileged"
code -b "$ALICE" -X POST "$BASE/api/admin/users/$BOBID/username" -H 'Content-Type: application/json' -d '{"username":"robert"}' >/dev/null
check "a rename, from and to" "$(latest | cut -d'|' -f1,3)" "admin.renamed|bob → robert"
NEWPW=$(curl -s -b "$ALICE" -X POST "$BASE/api/admin/users/$BOBID/password" | py "import sys,json;print(json.load(sys.stdin)['password'])")
check "a password reset" "$(latest | cut -d'|' -f1,3)" "admin.password_reset|robert"
code -b "$ALICE" -X POST "$BASE/api/admin/users/$BOBID/disable" >/dev/null
check "disabling" "$(latest | cut -d'|' -f1,3)" "admin.disabled|robert"
code -b "$ALICE" -X POST "$BASE/api/admin/users/$BOBID/enable" >/dev/null
check "and enabling" "$(latest | cut -d'|' -f1,3)" "admin.enabled|robert"

echo
echo "=== reading it ==="
# A live session, so the refusal below is about the role and not a dead cookie.
curl -s -o /dev/null -c "$BOB" -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
    -d "{\"username\":\"robert\",\"password\":\"$NEWPW\"}"
check "members cannot" "$(code -b "$BOB" "$BASE/api/admin/activity")" "403"
check "nor anyone signed out" "$(code "$BASE/api/admin/activity")" "401"
check "an administrator can, newest first" "$(feed "" "items[0]['event'], items[0]['actor']['username']")" "auth.login robert"
# Bob's early rows were recorded as "bob"; he is shown by the name he has now.
check "people are shown by their current name" \
    "$(feed "?event=auth.register" "items[0]['actor']['username']")" "robert"
check "filtered by person" "$(feed "?user=robert" "sorted({i['event'].split('.')[0] for i in items})")" "['auth', 'link']"
check "filtered by kind" "$(feed "?event=auth" "all(i['event'].startswith('auth.') for i in items) and len(items) > 0")" "True"
check "filtered by network" "$(feed "?network=2001:db8:1" "[i['event'] for i in items]")" "['auth.register']"
FIRST=$(feed "" "items[0]['id']")
check "a page at a time" "$(feed "?before=$FIRST" "items[0]['id'] < $FIRST")" "True"

echo
echo "=== kept 90 days, then deleted ==="
sql "UPDATE activity SET at = strftime('%s','now') - 91*86400 WHERE event = 'auth.bootstrap';"
sql "UPDATE activity SET at = strftime('%s','now') - 89*86400 WHERE event = 'invite.created';"
sleep 3
check "older than 90 days is gone" "$(sql "SELECT COUNT(*) FROM activity WHERE event = 'auth.bootstrap';")" "0"
check "younger stays" "$(sql "SELECT COUNT(*) FROM activity WHERE event = 'invite.created';")" "1"

code -b "$ALICE" -X POST "$BASE/api/auth/logout" >/dev/null
check "signing out is recorded" "$(latest | cut -d'|' -f1,2)" "auth.logout|alice"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -25 "$WORK/server.log"; }
exit $((FAIL > 0))
