#!/usr/bin/env bash
#
# Chat rooms, invitations, blocks and permanent history.
#
#   ./server/tests/chat.sh [path-to-binary]
#
# Four accounts: alice (admin and room creator), bob, carol and dave. The rules under
# test are mostly negative — who *cannot* read, post, invite or delete — so most checks
# assert a status code rather than a body.
#
set -u
BIN="${1:-$HOME/build-thearchive/thearchive}"
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

# POST/GET/DELETE as a given cookie jar, with an optional JSON body. Built as an array
# rather than an unquoted expansion, so a body containing spaces stays one argument.
req() {
    local jar="$1" method="$2" path="$3"; shift 3
    local args=(-s -b "$jar" -X "$method" "$BASE$path")
    [ $# -gt 0 ] && args+=(-H 'Content-Type: application/json' -d "$1")
    curl "${args[@]}"
}
# Echoing the body.
as()   { req "$@"; }
# Echoing only the status code.
code() {
    local jar="$1" method="$2" path="$3"; shift 3
    local args=(-s -o /dev/null -w '%{http_code}' -b "$jar" -X "$method" "$BASE$path")
    [ $# -gt 0 ] && args+=(-H 'Content-Type: application/json' -d "$1")
    curl "${args[@]}"
}

py()   { python3 -c "$1"; }
# Pull one field out of a JSON object on stdin.
jf()   { py "import sys,json;print(json.load(sys.stdin)['$1'])"; }
# The room object for a given name, out of GET /api/chat/rooms.
room() { py "
import sys, json
name = '$1'
for r in json.load(sys.stdin):
    if r['name'] == name:
        print(json.dumps(r)); break
else:
    print('{}')
"; }
# A field of that room object, or '-' when the room is not listed at all.
rf()   { py "
import sys, json
r = json.load(sys.stdin)
print(r.get('$1', '-') if r else '-')
"; }
# Bodies of the messages in a fetch, one per line; a removed one prints as a tombstone.
bodies() { py "
import sys, json
for m in json.load(sys.stdin)['messages']:
    print('[removed by %s]' % m['deleted_by'] if m.get('deleted') else m['body'])
"; }

ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT \
    "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "$BASE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

check "schema migrated to v11" "$(curl -s "$BASE/healthz" | jf schema)" "11"

curl -s -o /dev/null -c "$ALICE" -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' \
    -d '{"username":"alice","password":"correct-horse-battery"}'

# Three ordinary members, each through a single-use invite.
join() {
    local jar="$1" name="$2" invite
    invite=$(curl -s -b "$ALICE" -X POST "$BASE/api/invites" \
             | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
    curl -s -o /dev/null -c "$jar" -X POST "$BASE/api/auth/register" \
        -H 'Content-Type: application/json' \
        -d "{\"invite\":\"$invite\",\"username\":\"$name\",\"password\":\"correct-horse-battery\"}"
}
join "$BOB" bob
join "$CAROL" carol
join "$DAVE" dave

echo "=== creating a room ==="
resp=$(as "$ALICE" POST /api/chat/rooms '{"name":"Saturday"}')
ROOM=$(printf '%s' "$resp" | jf id)
[ -n "$ROOM" ] && ok "room created" || bad "room created" "$resp"
check "creator is a member at once" \
    "$(as "$ALICE" GET /api/chat/rooms | room Saturday | rf state)" "member"
check "and is marked as its creator" \
    "$(as "$ALICE" GET /api/chat/rooms | room Saturday | rf is_creator)" "True"
check "an empty name is refused" "$(code "$ALICE" POST /api/chat/rooms '{"name":"   "}')" "400"
check "signing in is required to list rooms" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/chat/rooms")" "401"

echo
echo "=== a non-member sees the name and nothing else ==="
# The core access rule: discoverable, not readable.
check "bob sees the room in the list" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf name)" "Saturday"
check "with no membership" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf state)" "none"
check "and no unread count" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf unread)" "-"
check "bob cannot read the messages" "$(code "$BOB" GET "/api/chat/rooms/$ROOM/messages")" "404"
check "bob cannot post" "$(code "$BOB" POST "/api/chat/rooms/$ROOM/messages" '{"body":"hi"}')" "404"
check "bob cannot mark it read" "$(code "$BOB" POST "/api/chat/rooms/$ROOM/read" '{"last_id":1}')" "404"
check "bob cannot invite anyone to it" \
    "$(code "$BOB" POST "/api/chat/rooms/$ROOM/invite" '{"username":"carol"}')" "403"
check "a room that does not exist reads the same as one you cannot enter" \
    "$(code "$BOB" GET "/api/chat/rooms/999999/messages")" "404"

echo
echo "=== invitation and acceptance ==="
check "creator can invite" \
    "$(code "$ALICE" POST "/api/chat/rooms/$ROOM/invite" '{"username":"bob"}')" "200"
check "inviting an unknown account 404s" \
    "$(code "$ALICE" POST "/api/chat/rooms/$ROOM/invite" '{"username":"nobody"}')" "404"
# An admin passes the creator check, so only an explicit existence check stands between
# this and a foreign key violation surfacing as a 500.
check "inviting to a room that does not exist 404s" \
    "$(code "$ALICE" POST "/api/chat/rooms/999999/invite" '{"username":"bob"}')" "404"
check "bob's state is now invited" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf state)" "invited"
check "and shows who invited him" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf invited_by)" "alice"
check "an invitation alone does not open the room" \
    "$(code "$BOB" GET "/api/chat/rooms/$ROOM/messages")" "404"

check "bob accepts" "$(code "$BOB" POST "/api/chat/rooms/$ROOM/respond" '{"action":"accept"}')" "200"
check "bob can now read" "$(code "$BOB" GET "/api/chat/rooms/$ROOM/messages")" "200"
check "bob can now post" \
    "$(code "$BOB" POST "/api/chat/rooms/$ROOM/messages" '{"body":"made it"}')" "201"
check "an unknown action is refused before the room state is even consulted" \
    "$(code "$BOB" POST "/api/chat/rooms/$ROOM/respond" '{"action":"maybe"}')" "400"
check "responding twice has nothing to respond to" \
    "$(code "$BOB" POST "/api/chat/rooms/$ROOM/respond" '{"action":"accept"}')" "404"
check "carol is still shut out" "$(code "$CAROL" GET "/api/chat/rooms/$ROOM/messages")" "404"

echo
echo "=== messages ==="
as "$ALICE" POST "/api/chat/rooms/$ROOM/messages" '{"body":"who is bringing the tent"}' >/dev/null
M3=$(as "$BOB" POST "/api/chat/rooms/$ROOM/messages" '{"body":"i am"}' | jf id)
check "history is in send order" \
    "$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages" | bodies | tr '\n' '|')" \
    "made it|who is bringing the tent|i am|"
check "an author is recorded" \
    "$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages" \
       | py "import sys,json;print(json.load(sys.stdin)['messages'][0]['author'])")" "bob"
check "since= returns only what is newer" \
    "$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages?since=$((M3-1))" | bodies)" "i am"
check "an idle poll returns nothing" \
    "$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages?since=$M3" | bodies | wc -l)" "0"
check "an empty message is refused" \
    "$(code "$BOB" POST "/api/chat/rooms/$ROOM/messages" '{"body":"  "}')" "400"
check "an oversized message is refused" \
    "$(code "$BOB" POST "/api/chat/rooms/$ROOM/messages" \
       "{\"body\":\"$(py 'print("x"*4001)')\"}")" "400"
check "a body is required" "$(code "$BOB" POST "/api/chat/rooms/$ROOM/messages" '{}')" "400"

echo
echo "=== a long room returns its tail, not its beginning ==="
# 250 messages straight into the database of a room of its own, so the cap is actually
# reached without disturbing the counts asserted below. Opening a room should land on
# the current conversation; the first 200 messages of a two-year-old room are not what
# anyone came for.
BULK=$(as "$ALICE" POST /api/chat/rooms '{"name":"Bulk"}' | jf id)
sql "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 250)
     INSERT INTO messages (room_id, user_id, author_name, body, created_at)
     SELECT $BULK, NULL, 'bulk', 'line ' || i, strftime('%s','now') FROM n;"
check "the cap is 200" \
    "$(as "$ALICE" GET "/api/chat/rooms/$BULK/messages" \
       | py "import sys,json;print(len(json.load(sys.stdin)['messages']))")" "200"
check "and it is the newest 200" \
    "$(as "$ALICE" GET "/api/chat/rooms/$BULK/messages" | bodies | tail -1)" "line 250"
check "still oldest-first within the page" \
    "$(as "$ALICE" GET "/api/chat/rooms/$BULK/messages" | bodies | head -1)" "line 51"

echo
echo "=== unread counts ==="
check "alice has bob's message unread" \
    "$(as "$ALICE" GET /api/chat/rooms | room Saturday | rf unread)" "1"
# Sending advances your own read mark, which also clears anything above it: you were
# looking at the room to type in it.
check "sending clears your own unread count" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf unread)" "0"
as "$ALICE" POST "/api/chat/rooms/$ROOM/read" "{\"last_id\":$M3}" >/dev/null
check "marking read clears it" \
    "$(as "$ALICE" GET /api/chat/rooms | room Saturday | rf unread)" "0"
# A second tab scrolled further back must not resurrect the badge.
as "$ALICE" POST "/api/chat/rooms/$ROOM/read" '{"last_id":1}' >/dev/null
check "the read mark never moves backwards" \
    "$(as "$ALICE" GET /api/chat/rooms | room Saturday | rf unread)" "0"
check "member count is visible" \
    "$(as "$CAROL" GET /api/chat/rooms | room Saturday | rf member_count)" "2"

echo
echo "=== admin removal leaves a tombstone ==="
check "a member cannot remove a message" "$(code "$BOB" DELETE "/api/chat/messages/$M3")" "403"
check "an admin can" "$(code "$ALICE" DELETE "/api/chat/messages/$M3")" "200"
check "removing it twice finds nothing to remove" \
    "$(code "$ALICE" DELETE "/api/chat/messages/$M3")" "404"
check "the row stays" "$(sql "SELECT COUNT(*) FROM messages WHERE id=$M3;")" "1"
check "the body is replaced by a tombstone, and the gap is not closed" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/messages" | bodies | tr '\n' '|')" \
    "made it|who is bringing the tent|[removed by alice]|"
check "the text is no longer served" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/messages" | grep -c '"i am"')" "0"

echo
echo "=== the member list ==="
# Who is in the room, for the header's "N members" and for the composer's @ autocomplete.
check "members are listed" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/members" \
       | py "import sys,json;print(','.join(sorted(m['username'] for m in json.load(sys.stdin)['members'])))")" \
    "alice,bob"
check "the creator is marked" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/members" \
       | py "import sys,json;print(','.join(m['username'] for m in json.load(sys.stdin)['members'] if m['is_creator']))")" \
    "alice"
check "a non-member cannot see who is in the room" \
    "$(code "$DAVE" GET "/api/chat/rooms/$ROOM/members")" "404"
# An outstanding invitation is worth showing — it stops the creator re-inviting someone
# who simply has not answered yet.
as "$ALICE" POST "/api/chat/rooms/$ROOM/invite" '{"username":"dave"}' >/dev/null
check "a pending invitation is listed separately" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/members" \
       | py "import sys,json;print(','.join(m['username'] for m in json.load(sys.stdin)['invited']))")" \
    "dave"
check "and is not counted as a member" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/members" \
       | py "import sys,json;print(len(json.load(sys.stdin)['members']))")" "2"
# Whether someone turned an invitation down is their business, not a status the room
# displays about them.
as "$DAVE" POST "/api/chat/rooms/$ROOM/respond" '{"action":"decline"}' >/dev/null
check "a decline is shown to nobody" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/members" \
       | py "import sys,json;d=json.load(sys.stdin);print(len(d['members']),len(d['invited']))")" \
    "2 0"

echo
echo "=== @mentions ==="
MM=$(as "$ALICE" POST "/api/chat/rooms/$ROOM/messages" '{"body":"@bob can you bring the tent"}' | jf id)
check "the mention is resolved and stored" \
    "$(sql "SELECT mentioned_name FROM message_mentions WHERE message_id = $MM;")" "bob"
check "and reported to the client" \
    "$(as "$BOB" GET "/api/chat/rooms/$ROOM/messages?since=$((MM-1))" \
       | py "import sys,json;print(','.join(json.load(sys.stdin)['messages'][0]['mentions']))")" "bob"
# Case is normalised: writing a name with a capital should not fail to reach someone.
M=$(as "$ALICE" POST "/api/chat/rooms/$ROOM/messages" '{"body":"@Bob again"}' | jf id)
check "a capitalised mention still resolves" \
    "$(sql "SELECT mentioned_name FROM message_mentions WHERE message_id = $M;")" "bob"
# A name that belongs to nobody in the room is text. Recording it would let a message
# claim to have notified someone it never could.
M=$(as "$ALICE" POST "/api/chat/rooms/$ROOM/messages" '{"body":"@nobody @dave hello"}' | jf id)
check "an unresolvable name records nothing" \
    "$(sql "SELECT COUNT(*) FROM message_mentions WHERE message_id = $M;")" "0"
# An @ inside a word is not a mention; otherwise an email address names its mail host.
M=$(as "$ALICE" POST "/api/chat/rooms/$ROOM/messages" '{"body":"mail me at me@bob.example"}' | jf id)
check "an email address is not a mention" \
    "$(sql "SELECT COUNT(*) FROM message_mentions WHERE message_id = $M;")" "0"
M=$(as "$ALICE" POST "/api/chat/rooms/$ROOM/messages" '{"body":"@bob @bob @bob"}' | jf id)
check "a repeated name is recorded once" \
    "$(sql "SELECT COUNT(*) FROM message_mentions WHERE message_id = $M;")" "1"

echo
echo "=== the mention badge ==="
check "bob has unread mentions" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf mentions_unread)" "3"
check "alice, who wrote them, has none" \
    "$(as "$ALICE" GET /api/chat/rooms | room Saturday | rf mentions_unread)" "0"
LAST=$(as "$BOB" GET "/api/chat/rooms/$ROOM/messages" \
       | py "import sys,json;print(json.load(sys.stdin)['messages'][-1]['id'])")
as "$BOB" POST "/api/chat/rooms/$ROOM/read" "{\"last_id\":$LAST}" >/dev/null
check "reading clears them" \
    "$(as "$BOB" GET /api/chat/rooms | room Saturday | rf mentions_unread)" "0"

echo
echo "=== declining ==="
as "$ALICE" POST "/api/chat/rooms/$ROOM/invite" '{"username":"carol"}' >/dev/null
check "carol declines" \
    "$(code "$CAROL" POST "/api/chat/rooms/$ROOM/respond" '{"action":"decline"}')" "200"
check "the room stays visible" \
    "$(as "$CAROL" GET /api/chat/rooms | room Saturday | rf state)" "declined"
check "but not enterable" "$(code "$CAROL" GET "/api/chat/rooms/$ROOM/messages")" "404"
# Declining a topic is not a permanent refusal; being re-asked has to work.
as "$ALICE" POST "/api/chat/rooms/$ROOM/invite" '{"username":"carol"}' >/dev/null
check "a re-invitation reopens the choice" \
    "$(as "$CAROL" GET /api/chat/rooms | room Saturday | rf state)" "invited"
as "$CAROL" POST "/api/chat/rooms/$ROOM/respond" '{"action":"accept"}' >/dev/null
check "carol is in" "$(code "$CAROL" GET "/api/chat/rooms/$ROOM/messages")" "200"
as "$ALICE" POST "/api/chat/rooms/$ROOM/invite" '{"username":"carol"}' >/dev/null
check "re-inviting a member does not demote them" \
    "$(as "$CAROL" GET /api/chat/rooms | room Saturday | rf state)" "member"

echo
echo "=== blocking a room ==="
SUN=$(as "$ALICE" POST /api/chat/rooms '{"name":"Sunday"}' | jf id)
as "$ALICE" POST "/api/chat/rooms/$SUN/invite" '{"username":"dave"}' >/dev/null
check "dave blocks the room" \
    "$(code "$DAVE" POST "/api/chat/rooms/$SUN/respond" '{"action":"block_room"}')" "200"
check "it disappears from his list entirely" \
    "$(as "$DAVE" GET /api/chat/rooms | room Sunday | rf name)" "-"
check "others still see it" \
    "$(as "$BOB" GET /api/chat/rooms | room Sunday | rf name)" "Sunday"
as "$ALICE" POST "/api/chat/rooms/$SUN/invite" '{"username":"dave"}' >/dev/null
check "re-inviting cannot pull him back in" \
    "$(as "$DAVE" GET /api/chat/rooms | room Sunday | rf name)" "-"
check "the block is listed" \
    "$(as "$DAVE" GET /api/chat/blocks \
       | py "import sys,json;print(json.load(sys.stdin)['rooms'][0]['name'])")" "Sunday"
check "dave unblocks it" "$(code "$DAVE" DELETE "/api/chat/blocks/room/$SUN")" "200"
check "and it returns" "$(as "$DAVE" GET /api/chat/rooms | room Sunday | rf name)" "Sunday"
check "unblocking twice finds no block" "$(code "$DAVE" DELETE "/api/chat/blocks/room/$SUN")" "404"

echo
echo "=== blocking a person ==="
MON=$(as "$ALICE" POST /api/chat/rooms '{"name":"Monday"}' | jf id)
as "$ALICE" POST "/api/chat/rooms/$MON/invite" '{"username":"dave"}' >/dev/null
check "dave blocks alice herself" \
    "$(code "$DAVE" POST "/api/chat/rooms/$MON/respond" '{"action":"block_user"}')" "200"
check "the block is listed" \
    "$(as "$DAVE" GET /api/chat/blocks \
       | py "import sys,json;print(json.load(sys.stdin)['users'][0]['username'])")" "alice"
# The point of blocking a person rather than a room: it follows them elsewhere.
TUE=$(as "$ALICE" POST /api/chat/rooms '{"name":"Tuesday"}' | jf id)
check "alice's invite to a different room reports success" \
    "$(code "$ALICE" POST "/api/chat/rooms/$TUE/invite" '{"username":"dave"}')" "200"
check "but dave never receives it" \
    "$(as "$DAVE" GET /api/chat/rooms | room Tuesday | rf state)" "none"
# Blocking one person does not cut him off from everyone.
as "$BOB" POST /api/chat/rooms '{"name":"Wednesday"}' >/dev/null
WED=$(as "$BOB" GET /api/chat/rooms | room Wednesday | rf id)
check "someone else can still invite him" \
    "$(code "$BOB" POST "/api/chat/rooms/$WED/invite" '{"username":"dave"}')" "200"
check "and that invitation arrives" \
    "$(as "$DAVE" GET /api/chat/rooms | room Wednesday | rf state)" "invited"
ALICEID=$(sql "SELECT id FROM users WHERE username='alice';")
check "dave unblocks alice" "$(code "$DAVE" DELETE "/api/chat/blocks/user/$ALICEID")" "200"
as "$ALICE" POST "/api/chat/rooms/$TUE/invite" '{"username":"dave"}' >/dev/null
check "her invitations arrive again" \
    "$(as "$DAVE" GET /api/chat/rooms | room Tuesday | rf state)" "invited"

echo
echo "=== who may invite ==="
check "a member who did not create the room cannot invite" \
    "$(code "$CAROL" POST "/api/chat/rooms/$ROOM/invite" '{"username":"dave"}')" "403"
check "an admin can invite to a room she did not create" \
    "$(code "$ALICE" POST "/api/chat/rooms/$WED/invite" '{"username":"carol"}')" "200"

echo
echo "=== history outlives its author ==="
# The regression the schema exists to prevent. Every other foreign key to users in this
# database cascades; if these did, deleting bob would take the conversation with him.
BEFORE=$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages" | bodies | wc -l)
BOBID=$(sql "SELECT id FROM users WHERE username='bob';")
check "alice deletes bob's account" "$(code "$ALICE" DELETE "/api/admin/users/$BOBID")" "200"
check "the account is gone" "$(sql "SELECT COUNT(*) FROM users WHERE username='bob';")" "0"
check "his messages are not" \
    "$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages" | bodies | wc -l)" "$BEFORE"
check "and stay attributed to him" \
    "$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages" \
       | py "import sys,json;print(json.load(sys.stdin)['messages'][0]['author'])")" "bob"
check "marked as an account that has left" \
    "$(as "$ALICE" GET "/api/chat/rooms/$ROOM/messages" \
       | py "import sys,json;print(json.load(sys.stdin)['messages'][0].get('author_departed'))")" "True"
check "the room he created outlives him too" \
    "$(as "$ALICE" GET /api/chat/rooms | room Wednesday | rf created_by)" "bob"
check "but his membership is gone" \
    "$(as "$ALICE" GET /api/chat/rooms | room Saturday | rf member_count)" "2"

echo
echo "=== a deleted room takes its messages with it ==="
# Rooms are not deletable through the API; this asserts the cascade exists, so that if
# room deletion is ever added it does not leave orphaned rows behind. The pragma is not
# redundant: the sqlite3 CLI leaves foreign keys off by default, so without it the
# DELETE below succeeds and enforces nothing, and this test passes vacuously.
sql "INSERT INTO messages (room_id, user_id, author_name, body, created_at)
     VALUES ($MON, $ALICEID, 'alice', 'anyone there', strftime('%s','now'));"
check "the room has rows to lose" \
    "$(sql "SELECT COUNT(*) FROM messages WHERE room_id = $MON;")" "1"
sql "PRAGMA foreign_keys=ON; DELETE FROM rooms WHERE id = $MON;"
check "messages cascade from the room" \
    "$(sql "SELECT COUNT(*) FROM messages WHERE room_id = $MON;")" "0"
check "so do memberships" \
    "$(sql "SELECT COUNT(*) FROM room_members WHERE room_id = $MON;")" "0"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -30 "$WORK/server.log"; }
exit $((FAIL > 0))
