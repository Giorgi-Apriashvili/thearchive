#!/usr/bin/env bash
#
# Sending links: attaching them to chat messages, and sending one to a member's inbox.
#
#   ./server/tests/sending.sh [path-to-binary]
#
# alice owns most of the links; bob receives; carol exists to have her account deleted.
set -u
BIN="${1:-$HOME/build-thearchive/thearchive}"
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
BASE=http://127.0.0.1:$PORT
DATA=$(mktemp -d)
WORK=$(mktemp -d)
ALICE=$(mktemp); BOB=$(mktemp); CAROL=$(mktemp); DAVE=$(mktemp)
DB="$DATA/db/archive.db"
PASS=0; FAIL=0

cleanup() {
    kill "${PID:-}" 2>/dev/null; wait "${PID:-}" 2>/dev/null
    rm -rf "$DATA" "$WORK" "$ALICE" "$BOB" "$CAROL" "$DAVE"
}
trap cleanup EXIT

ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1  ($2)"; FAIL=$((FAIL+1)); }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1" "expected '$3', got '$2'"; }
sql()  { sqlite3 "$DB" "$1"; }
py()   { python3 -c "$1"; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
post() {  # <jar> <path> <json>  -> body
    curl -s -b "$1" -X POST "$BASE$2" -H 'Content-Type: application/json' --data-binary "$3"; }
post_code() { curl -s -o /dev/null -w '%{http_code}' -b "$1" -X POST "$BASE$2" \
              -H 'Content-Type: application/json' --data-binary "$3"; }
hdr() { local k; k=$(printf '%s' "$1" | tr 'A-Z' 'a-z'); tr -d '\r' | awk -v k="$k:" 'tolower($1)==k {print $2; exit}'; }

ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "$BASE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

curl -s -o /dev/null -c "$ALICE" -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' -d '{"username":"alice","password":"correct-horse-battery"}'
join() {
    local invite; invite=$(curl -s -b "$ALICE" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
    curl -s -o /dev/null -c "$1" -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
        -d "{\"invite\":\"$invite\",\"username\":\"$2\",\"password\":\"correct-horse-battery\"}"
}
join "$BOB" bob
join "$CAROL" carol

# A link owned by <jar>, from <file>, with extra JSON fields. Echoes its token.
make_share() {
    local jar="$1" file="$2" extra="$3" size meta loc
    size=$(stat -c%s "$file")
    meta="filename $(basename "$file" | base64 -w0)"
    loc=$(curl -s -D - -o /dev/null -b "$jar" -X POST "$BASE/files" \
          -H "Upload-Length: $size" -H "Upload-Metadata: $meta" | hdr Location)
    curl -s -o /dev/null -b "$jar" -X PATCH "$BASE$loc" -H 'Content-Type: application/offset+octet-stream' \
        -H 'Upload-Offset: 0' --data-binary "@$file"
    post "$jar" /api/shares "{\"uploads\":[\"${loc##*/}\"]$extra}" | py "import sys,json;print(json.load(sys.stdin)['token'])"
}
python3 "$HERE/img.py" png 300 200 > "$WORK/photo.png"
head -c 2000 /dev/urandom > "$WORK/notes.bin"
PHOTOS=$(make_share "$ALICE" "$WORK/photo.png" ',"title":"Kazbegi photos","public":true')
LOCKED=$(make_share "$ALICE" "$WORK/photo.png" ',"title":"Private stuff","password":"hunter22"')
DOOMED=$(make_share "$ALICE" "$WORK/notes.bin" ',"title":"To be revoked"')
AGEING=$(make_share "$ALICE" "$WORK/notes.bin" ',"title":"To expire"')
ONCE=$(make_share "$ALICE" "$WORK/notes.bin" ',"title":"One download","max_downloads":1,"public":true')
BOBS=$(make_share "$BOB" "$WORK/notes.bin" ',"title":"Bob owns this"')
PACKING=$(make_share "$ALICE" "$WORK/notes.bin" ',"title":"Packing list"')
CAROLS=$(make_share "$CAROL" "$WORK/notes.bin" ',"title":"Carol was here"')

ROOM=$(post "$ALICE" /api/chat/rooms '{"name":"Trip"}' | py "import sys,json;print(json.load(sys.stdin)['id'])")
for who in bob carol; do
    post "$ALICE" "/api/chat/rooms/$ROOM/invite" "{\"username\":\"$who\"}" >/dev/null
done
post "$BOB" "/api/chat/rooms/$ROOM/respond" '{"action":"accept"}' >/dev/null
post "$CAROL" "/api/chat/rooms/$ROOM/respond" '{"action":"accept"}' >/dev/null

say() { post "$1" "/api/chat/rooms/$ROOM/messages" "$2"; }
say_code() { post_code "$1" "/api/chat/rooms/$ROOM/messages" "$2"; }
# The cards on message <id>, as seen by <jar>, run through a Python expression over `cards`.
cards() { curl -s -b "$1" "$BASE/api/chat/rooms/$ROOM/messages" \
    | py "import sys,json;m=[x for x in json.load(sys.stdin)['messages'] if x['id']==$2];cards=m[0].get('shares',[]) if m else None;print($3)"; }

echo "=== links in chat ==="
M1=$(say "$ALICE" "{\"body\":\"the good ones\",\"shares\":[\"$PHOTOS\"]}" | py "import sys,json;print(json.load(sys.stdin)['id'])")
[ -n "$M1" ] && ok "a message with a link is posted" || bad "a message with a link is posted" "no id"
check "it carries one card" "$(cards "$BOB" "$M1" 'len(cards)')" "1"
check "which works" "$(cards "$BOB" "$M1" "cards[0]['state']")" "live"
check "and leads to the link" "$(cards "$BOB" "$M1" "cards[0]['token']")" "$PHOTOS"
check "with its title, file count and size" \
    "$(cards "$BOB" "$M1" "cards[0]['title'], cards[0]['file_count'], cards[0]['total_bytes'] > 0")" \
    "Kazbegi photos 1 True"
PREVIEW=$(cards "$BOB" "$M1" "cards[0]['previews'][0]")
case "$PREVIEW" in /d/$PHOTOS/*/thumb*) ok "and a preview of its image" ;; *) bad "and a preview of its image" "$PREVIEW" ;; esac
check "which a room member can load" "$(code -b "$BOB" "$BASE$PREVIEW")" "200"

M2=$(say "$ALICE" "{\"shares\":[\"$LOCKED\"]}" | py "import sys,json;print(json.load(sys.stdin)['id'])")
[ -n "$M2" ] && ok "a link alone, with no text, is a message" || bad "a link alone is a message" "refused"
check "a password-protected link says so" "$(cards "$BOB" "$M2" "cards[0]['password_protected']")" "True"
# Loading a preview would need the password, which a card must never carry.
check "and shows no previews, though it holds an image" "$(cards "$BOB" "$M2" "len(cards[0]['previews'])")" "0"
check "its recipients still need the password to open it" \
    "$(code -b "$BOB" "$BASE/api/shares/$LOCKED")" "401"

M3=$(say "$ALICE" "{\"shares\":[\"$PHOTOS\",\"$PHOTOS\",\"$DOOMED\"]}" | py "import sys,json;print(json.load(sys.stdin)['id'])")
check "the same link twice is attached once" "$(cards "$BOB" "$M3" 'len(cards)')" "2"
check "neither text nor a link is refused" "$(say_code "$ALICE" '{"body":"   "}')" "400"
check "you cannot post someone else's link" "$(say_code "$ALICE" "{\"shares\":[\"$BOBS\"]}")" "404"
check "nor one that does not exist" "$(say_code "$ALICE" '{"shares":["no-such-token"]}')" "404"
check "shares must be a list" "$(say_code "$ALICE" '{"shares":"nope"}')" "400"
ELEVEN=$(py "import json;print(json.dumps(['$PHOTOS']*11))")
check "at most ten links to a message" "$(say_code "$ALICE" "{\"shares\":$ELEVEN}")" "400"

echo
echo "=== a card tells the truth later ==="
# Resolved when read, not when posted: a link posted last month must say it expired.
curl -s -o /dev/null -b "$ALICE" -X DELETE "$BASE/api/shares/$DOOMED"
check "revoking the link shows on the card" "$(cards "$BOB" "$M3" "cards[1]['state']")" "revoked"
check "which then carries no address" "$(cards "$BOB" "$M3" "'token' in cards[1]")" "False"
M4=$(say "$ALICE" "{\"shares\":[\"$AGEING\",\"$ONCE\"]}" | py "import sys,json;print(json.load(sys.stdin)['id'])")
sql "UPDATE shares SET expires_at = strftime('%s','now') - 1 WHERE token = '$AGEING';"
check "an expired link reads as expired" "$(cards "$BOB" "$M4" "cards[0]['state']")" "expired"
FID=$(curl -s "$BASE/api/shares/$ONCE" | py "import sys,json;print(json.load(sys.stdin)['files'][0]['id'])")
curl -s -o /dev/null "$BASE/d/$ONCE/$FID"
check "a link whose download limit is spent reads as used up" "$(cards "$BOB" "$M4" "cards[1]['state']")" "used_up"
check "and none of those can be posted again" \
    "$(say_code "$ALICE" "{\"shares\":[\"$DOOMED\"]}"),$(say_code "$ALICE" "{\"shares\":[\"$AGEING\"]}"),$(say_code "$ALICE" "{\"shares\":[\"$ONCE\"]}")" \
    "409,409,409"

echo
echo "=== removal and departure ==="
check "an admin removes the message with the links" "$(code -b "$ALICE" -X DELETE "$BASE/api/chat/messages/$M1")" "200"
check "its links go with it" "$(sql "SELECT COUNT(*) FROM message_shares WHERE message_id = $M1;")" "0"
check "and are no longer served" "$(cards "$BOB" "$M1" "'shares' in (m[0] if m else {})")" "False"
M5=$(say "$CAROL" "{\"shares\":[\"$CAROLS\"]}" | py "import sys,json;print(json.load(sys.stdin)['id'])")
CAROL_ID=$(sql "SELECT id FROM users WHERE username='carol';")
check "carol's account is deleted, and her links with it" \
    "$(code -b "$ALICE" -X DELETE "$BASE/api/admin/users/$CAROL_ID")" "200"
# Chat is permanent: the message keeps a trace of what it carried rather than a hole.
check "her message still says a link was here" "$(cards "$BOB" "$M5" "cards[0]['state']")" "gone"
check "and what it was called" "$(cards "$BOB" "$M5" "cards[0]['title']")" "Carol was here"

echo
echo "=== sending a link to someone ==="
send() { post_code "$1" "/api/shares/$2/send" "$3"; }
inbox() { curl -s -b "$1" "$BASE/api/inbox" | py "import sys,json;items=json.load(sys.stdin)['items'];print($2)"; }
unread() { curl -s -b "$1" "$BASE/api/inbox/unread" | py "import sys,json;print(json.load(sys.stdin)['count'])"; }

check "sending needs a session" "$(code -X POST "$BASE/api/shares/$PHOTOS/send" -H 'Content-Type: application/json' -d '{"username":"bob"}')" "401"
check "alice sends bob her photos" "$(send "$ALICE" "$PHOTOS" '{"username":"Bob","note":"the good ones are at the end"}')" "200"
check "bob has one unread" "$(unread "$BOB")" "1"
check "it is in his inbox" "$(inbox "$BOB" 'len(items)')" "1"
check "from alice, with her note" "$(inbox "$BOB" "items[0]['sender']['username'], items[0]['note']")" "alice the good ones are at the end"
check "as a working card" "$(inbox "$BOB" "items[0]['cards'][0]['state'], items[0]['cards'][0]['token']")" "live $PHOTOS"
check "alice's own inbox is empty" "$(inbox "$ALICE" 'len(items)')" "0"

check "marking seen" "$(post_code "$BOB" /api/inbox/seen '{}')" "200"
check "clears the badge" "$(unread "$BOB")" "0"
check "and the item is marked seen" "$(inbox "$BOB" "items[0]['seen']")" "True"
send "$ALICE" "$PHOTOS" '{"username":"bob","note":"did you see these?"}' >/dev/null
check "sending it again does not make a second copy" "$(inbox "$BOB" 'len(items)')" "1"
check "but brings it back as unread with the new note" "$(unread "$BOB"),$(inbox "$BOB" "items[0]['note']")" "1,did you see these?"

check "you cannot send to yourself" "$(send "$ALICE" "$PHOTOS" '{"username":"alice"}')" "400"
check "nor to nobody" "$(send "$ALICE" "$PHOTOS" '{"username":"nobody"}')" "404"
check "nor someone else's link" "$(send "$ALICE" "$BOBS" '{"username":"bob"}')" "404"
check "nor a link that has stopped working" "$(send "$ALICE" "$AGEING" '{"username":"bob"}')" "409"
check "a note has a limit" "$(send "$ALICE" "$PHOTOS" "{\"username\":\"bob\",\"note\":\"$(py "print('n'*301)")\"}")" "400"
check "and no text-direction tricks" "$(send "$ALICE" "$PHOTOS" '{"username":"bob","note":"a\u202Eb"}')" "400"

# The link keeps its own rules: sending grants nothing.
send "$ALICE" "$LOCKED" '{"username":"bob"}' >/dev/null
check "a password-protected link arrives" "$(inbox "$BOB" "[c['password_protected'] for i in items for c in i['cards']].count(True)")" "1"
check "and still asks bob for its password" "$(code -b "$BOB" "$BASE/api/shares/$LOCKED")" "401"
check "which, given, opens it" "$(code -b "$BOB" -H 'X-Share-Password: hunter22' "$BASE/api/shares/$LOCKED")" "200"

echo
echo "=== the inbox is its owner's ==="
ITEM=$(inbox "$BOB" "items[0]['id']")
check "someone else cannot dismiss your item" "$(code -b "$ALICE" -X DELETE "$BASE/api/inbox/items/$ITEM")" "404"
check "you can" "$(code -b "$BOB" -X DELETE "$BASE/api/inbox/items/$ITEM")" "200"
check "and it is gone" "$(inbox "$BOB" "'$ITEM' in [i['id'] for i in items]")" "False"

echo
echo "=== several links at once ==="
join "$DAVE" dave
many() { post_code "$1" "/api/users/$2/links" "$3"; }
titles() { inbox "$1" "';'.join(','.join(c['title'] for c in i['cards']) for i in items)"; }

check "needs a session" "$(code -X POST "$BASE/api/users/dave/links" -H 'Content-Type: application/json' -d "{\"links\":[\"$PHOTOS\"]}")" "401"
check "needs at least one link" "$(many "$ALICE" dave '{"links":[]}')" "400"
check "as a list" "$(many "$ALICE" dave "{\"links\":\"$PHOTOS\"}")" "400"
check "of at most ten" "$(many "$ALICE" dave "{\"links\":$ELEVEN}")" "400"
# All or nothing: one stale link refuses the send, rather than delivering the rest.
check "one link that stopped working refuses the whole send" "$(many "$ALICE" dave "{\"links\":[\"$PHOTOS\",\"$AGEING\"]}")" "409"
check "and delivers none of it" "$(inbox "$DAVE" 'len(items)')" "0"
check "as does one that is not yours" "$(many "$ALICE" dave "{\"links\":[\"$PHOTOS\",\"$BOBS\"]}"),$(inbox "$DAVE" 'len(items)')" "404,0"

check "alice sends dave three, one of them twice" \
    "$(many "$ALICE" Dave "{\"links\":[\"$PACKING\",\"$PHOTOS\",\"$PACKING\"],\"note\":\"for saturday\"}")" "200"
check "they arrive as one item" "$(inbox "$DAVE" 'len(items)')" "1"
check "counted once on the badge" "$(unread "$DAVE")" "1"
check "in the order she picked, each once" "$(titles "$DAVE")" "Packing list,Kazbegi photos"
check "with the note once" "$(inbox "$DAVE" "items[0]['note']")" "for saturday"

many "$ALICE" dave "{\"links\":[\"$PHOTOS\"],\"note\":\"this one especially\"}" >/dev/null
check "sending one of them again moves it into a new item" "$(titles "$DAVE")" "Kazbegi photos;Packing list"
check "with its own note, the old item keeping its" "$(inbox "$DAVE" "items[0]['note'], items[1]['note']")" "this one especially for saturday"
check "two sends, two unread" "$(unread "$DAVE")" "2"
check "the single-link route files into the same inbox" \
    "$(send "$ALICE" "$LOCKED" '{"username":"dave"}'),$(inbox "$DAVE" 'len(items)')" "200,3"

many "$ALICE" dave "{\"links\":[\"$PACKING\",\"$LOCKED\"]}" >/dev/null
GROUP=$(inbox "$DAVE" "items[0]['id']")
check "dismissing an item" "$(code -b "$DAVE" -X DELETE "$BASE/api/inbox/items/$GROUP")" "200"
check "dismisses every link in it" "$(inbox "$DAVE" "len(items), sum(len(i['cards']) for i in items)")" "1 1"

echo
echo "=== blocks hold ==="
BEFORE=$(inbox "$BOB" 'len(items)')
ALICE_ID=$(sql "SELECT id FROM users WHERE username='alice';")
BOB_ID=$(sql "SELECT id FROM users WHERE username='bob';")
sql "INSERT INTO user_blocks (user_id, blocked_id, created_at) VALUES ($BOB_ID, $ALICE_ID, 0);"
# Success, exactly as an invitation to a blocker reports it: the sender is not told.
check "sending to someone who blocked you reports success" "$(send "$ALICE" "$PHOTOS" '{"username":"bob"}')" "200"
check "and delivers nothing" "$(inbox "$BOB" 'len(items)')" "$BEFORE"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -25 "$WORK/server.log"; }
exit $((FAIL > 0))
