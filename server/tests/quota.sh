#!/usr/bin/env bash
#
# Storage limits: role defaults, per-account limits, what counts, and the refusal.
#
#   ./server/tests/quota.sh [path-to-binary]
#
# alice is the administrator; bob an ordinary member. The limits are set small through the
# admin endpoint, so the behaviour is tested with kilobytes rather than gigabytes.
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
hdr()  { local k; k=$(printf '%s' "$1" | tr 'A-Z' 'a-z'); tr -d '\r' | awk -v k="$k:" 'tolower($1)==k {print $2; exit}'; }
GiB=1073741824

ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "$BASE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

curl -s -o /dev/null -c "$ALICE" -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' -d '{"username":"alice","password":"correct-horse-battery"}'
INVITE=$(curl -s -b "$ALICE" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
curl -s -o /dev/null -c "$BOB" -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
    -d "{\"invite\":\"$INVITE\",\"username\":\"bob\",\"password\":\"correct-horse-battery\"}"
BOBID=$(sql "SELECT id FROM users WHERE username='bob';")

mine()    { curl -s -b "$1" "$BASE/api/storage" | py "import sys,json;d=json.load(sys.stdin);print(d['my_usage'], d['my_limit'])"; }
storage() { curl -s -b "$ALICE" "$BASE/api/admin/users/$BOBID" | py "import sys,json;s=json.load(sys.stdin)['storage'];print(s['used'], s['limit'], s['custom'])"; }
limit()   { curl -s -o /dev/null -w '%{http_code}' -b "$ALICE" -X POST "$BASE/api/admin/users/$BOBID/limit" \
            -H 'Content-Type: application/json' -d "{\"bytes\":$1}"; }
role()    { curl -s -o /dev/null -b "$ALICE" -X POST "$BASE/api/admin/users/$BOBID/role" \
            -H 'Content-Type: application/json' -d "{\"role\":\"$1\"}"; }
# Starts an upload of <bytes> for <jar>; echoes the status code, and the id on success.
start()   { curl -s -D - -o "$WORK/body" -b "$1" -X POST "$BASE/files" -H "Upload-Length: $2" \
            -H "Upload-Metadata: filename $(printf 'f.bin' | base64 -w0)" | tr -d '\r' | awk 'NR==1{print $2}'; }
finish()  { local size="$2" loc
            head -c "$size" /dev/urandom > "$WORK/f.bin"
            loc=$(curl -s -D - -o /dev/null -b "$1" -X POST "$BASE/files" -H "Upload-Length: $size" \
                  -H "Upload-Metadata: filename $(printf 'f.bin' | base64 -w0)" | hdr Location)
            curl -s -o /dev/null -b "$1" -X PATCH "$BASE$loc" -H 'Content-Type: application/offset+octet-stream' \
                -H 'Upload-Offset: 0' --data-binary "@$WORK/f.bin"
            printf '%s' "${loc##*/}"; }

echo "=== role defaults ==="
check "an ordinary member: 20 GB" "$(mine "$BOB")" "0 $((20*GiB))"
check "an administrator: no limit" "$(mine "$ALICE")" "0 None"
role privileged
check "promoting raises it to 100 GB" "$(mine "$BOB")" "0 $((100*GiB))"
role user
check "and back" "$(mine "$BOB")" "0 $((20*GiB))"
check "the control panel shows it as the role's" "$(storage)" "0 $((20*GiB)) False"

echo
echo "=== a limit for one account ==="
check "an admin sets 100 KB for bob" "$(limit 102400)" "200"
check "his own view follows" "$(mine "$BOB")" "0 102400"
check "marked as set for him" "$(storage)" "0 102400 True"
check "and it is recorded" "$(sql "SELECT event || '|' || detail FROM activity ORDER BY id DESC LIMIT 1;")" "admin.limit|bob: 100 KB"
check "a promotion does not override it" "$(role privileged; mine "$BOB"; role user)" "0 102400"
check "a nonsense limit is refused" "$(limit '"lots"'),$(limit -5)" "400,400"
check "members cannot set limits" "$(curl -s -o /dev/null -w '%{http_code}' -b "$BOB" -X POST "$BASE/api/admin/users/$BOBID/limit" -H 'Content-Type: application/json' -d '{"bytes":-1}')" "403"

echo
echo "=== what counts, and the refusal ==="
check "an upload that fits is accepted" "$(start "$BOB" 60000)" "201"
check "an unfinished one counts at its full size" "$(mine "$BOB" | cut -d' ' -f1)" "60000"
check "one that would go over is refused before anything is sent" "$(start "$BOB" 60000)" "413"
check "saying why, and how much is in use" "$(py "import json;print(json.load(open('$WORK/body'))['error'])")" \
    "this would take you over your 100 KB storage limit — you are using 59 KB. Links that expire or are removed give their space back."
check "and leaves nothing behind" "$(sql "SELECT COUNT(*) FROM uploads WHERE owner_id=$BOBID;")" "1"
curl -s -o /dev/null -b "$BOB" -X DELETE "$BASE$(sql "SELECT '/files/' || id FROM uploads WHERE owner_id=$BOBID;")"
check "discarding it frees the space" "$(mine "$BOB" | cut -d' ' -f1)" "0"
U=$(finish "$BOB" 40000)
TOK=$(curl -s -b "$BOB" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' -d "{\"uploads\":[\"$U\"]}" | py "import sys,json;print(json.load(sys.stdin)['token'])")
check "a file in a link counts" "$(mine "$BOB" | cut -d' ' -f1)" "40000"
# The same content again, in a second link: on disk once, so counted once.
head -c 40000 "$WORK/f.bin" > "$WORK/same.bin"
SZ=40000; LOC=$(curl -s -D - -o /dev/null -b "$BOB" -X POST "$BASE/files" -H "Upload-Length: $SZ" \
    -H "Upload-Metadata: filename $(printf 'same.bin' | base64 -w0)" | hdr Location)
curl -s -o /dev/null -b "$BOB" -X PATCH "$BASE$LOC" -H 'Content-Type: application/offset+octet-stream' \
    -H 'Upload-Offset: 0' --data-binary "@$WORK/same.bin"
curl -s -o /dev/null -b "$BOB" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' -d "{\"uploads\":[\"${LOC##*/}\"]}"
check "the same file in two links counts once" "$(mine "$BOB" | cut -d' ' -f1)" "40000"
check "so there is still room for 60 KB" "$(start "$BOB" 60000)" "201"
curl -s -o /dev/null -b "$BOB" -X DELETE "$BASE$(sql "SELECT '/files/' || id FROM uploads WHERE owner_id=$BOBID AND blob_sha256 IS NULL;")"
curl -s -o /dev/null -b "$BOB" -X DELETE "$BASE/api/shares/$TOK"
check "removing a link gives back what only it held" "$(mine "$BOB" | cut -d' ' -f1)" "40000"
check "lowering a limit below use deletes nothing" "$(limit 1000 >/dev/null; sql "SELECT COUNT(*) FROM share_files sf JOIN shares s ON s.id=sf.share_id WHERE s.owner_id=$BOBID AND s.deleted_at IS NULL;")" "1"
check "it only stops new uploads" "$(start "$BOB" 10)" "413"
check "lifting the limit" "$(limit -1 >/dev/null; mine "$BOB")" "40000 None"
check "back to the role's" "$(limit null >/dev/null; mine "$BOB")" "40000 $((20*GiB))"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -25 "$WORK/server.log"; }
exit $((FAIL > 0))
