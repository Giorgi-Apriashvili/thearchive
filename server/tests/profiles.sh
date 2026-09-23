#!/usr/bin/env bash
#
# Member profiles: display name, bio and picture, who can see them, and what a picture
# keeps of the image it was made from.
#
#   ./server/tests/profiles.sh [path-to-binary]
#
# alice is the bootstrap admin; bob and carol join by invite. Images are built by img.py
# with the standard library, so nothing here needs the vips command-line tool.
set -u
BIN="${1:-$HOME/build-thearchive/thearchive}"
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
BASE=http://127.0.0.1:$PORT
DATA=$(mktemp -d)
WORK=$(mktemp -d)
ALICE=$(mktemp); BOB=$(mktemp); CAROL=$(mktemp)
DB="$DATA/db/archive.db"
PASS=0; FAIL=0

cleanup() {
    kill "${PID:-}" 2>/dev/null; wait "${PID:-}" 2>/dev/null
    rm -rf "$DATA" "$WORK" "$ALICE" "$BOB" "$CAROL"
}
trap cleanup EXIT

ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1  ($2)"; FAIL=$((FAIL+1)); }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1" "expected '$3', got '$2'"; }
sql()  { sqlite3 "$DB" "$1"; }
img()  { python3 "$HERE/img.py" "$@"; }
# A field of a JSON object on stdin, or '-' when absent.
field() { python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('$1','-'))"; }
code()  { curl -s -o /dev/null -w '%{http_code}' "$@"; }
patch_profile() { curl -s -o /dev/null -w '%{http_code}' -b "$1" -X PATCH "$BASE/api/me/profile" \
                  -H 'Content-Type: application/json' --data-binary "$2"; }
put_avatar() { curl -s -b "$1" -X PUT "$BASE/api/me/avatar" --data-binary "@$2"; }
put_avatar_code() { curl -s -o /dev/null -w '%{http_code}' -b "$1" -X PUT "$BASE/api/me/avatar" --data-binary "@$2"; }

# A 2s sweep, so the removal of stale pictures is observable.
ARCHIVE_SECURE_COOKIES=0 ARCHIVE_DATA_DIR="$DATA" ARCHIVE_PORT=$PORT ARCHIVE_GC_INTERVAL_SECONDS=2 \
    "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "$BASE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

curl -s -o /dev/null -c "$ALICE" -X POST "$BASE/api/auth/bootstrap" \
    -H 'Content-Type: application/json' -d '{"username":"alice","password":"correct-horse-battery"}'
join() {
    local jar="$1" name="$2" invite
    invite=$(curl -s -b "$ALICE" -X POST "$BASE/api/invites" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')
    curl -s -o /dev/null -c "$jar" -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
        -d "{\"invite\":\"$invite\",\"username\":\"$name\",\"password\":\"correct-horse-battery\"}"
}
join "$BOB" bob
join "$CAROL" carol

echo "=== who can see a profile ==="
check "not someone with no account" "$(code "$BASE/api/users/alice")" "401"
P=$(curl -s -b "$BOB" "$BASE/api/users/alice")
check "a member can" "$(printf '%s' "$P" | field username)" "alice"
check "with the role" "$(printf '%s' "$P" | field role)" "admin"
check "and when they joined" "$(printf '%s' "$P" | python3 -c 'import sys,json;print(json.load(sys.stdin)["joined"] > 0)')" "True"
check "no display name, bio or picture until set" \
    "$(printf '%s' "$P" | field display_name),$(printf '%s' "$P" | field bio),$(printf '%s' "$P" | field avatar)" "-,-,-"
check "someone else's profile is not marked as yours" "$(printf '%s' "$P" | field is_me)" "False"
check "your own is" "$(curl -s -b "$BOB" "$BASE/api/users/bob" | field is_me)" "True"
check "and shows who invited you" "$(curl -s -b "$BOB" "$BASE/api/users/bob" | field invited_by)" "alice"
check "usernames match regardless of case" "$(code -b "$BOB" "$BASE/api/users/ALICE")" "200"
check "an unknown member is 404" "$(code -b "$BOB" "$BASE/api/users/nobody")" "404"

echo
echo "=== editing your own profile ==="
check "needs a session" "$(code -X PATCH "$BASE/api/me/profile" -H 'Content-Type: application/json' -d '{"bio":"x"}')" "401"
check "set a display name" "$(patch_profile "$BOB" '{"display_name":"  Bob the Builder  "}')" "200"
check "stored trimmed" "$(curl -s -b "$CAROL" "$BASE/api/users/bob" | field display_name)" "Bob the Builder"
check "set a bio with a line break" "$(patch_profile "$BOB" '{"bio":"Line one\r\nLine two"}')" "200"
check "line breaks kept, carriage returns dropped" \
    "$(curl -s -b "$CAROL" "$BASE/api/users/bob" | python3 -c 'import sys,json;print(repr(json.load(sys.stdin)["bio"]))')" \
    "'Line one\\nLine two'"
check "saving one field leaves the other alone" \
    "$(curl -s -b "$CAROL" "$BASE/api/users/bob" | field display_name)" "Bob the Builder"
check "and it reaches /api/me" "$(curl -s -b "$BOB" "$BASE/api/me" | field display_name)" "Bob the Builder"

# Characters, not bytes: 40 Georgian letters are 120 bytes and must fit.
check "40 characters in a non-Latin script fit" \
    "$(patch_profile "$CAROL" "{\"display_name\":\"$(python3 -c "print('ა'*40)")\"}")" "200"
check "41 do not" "$(patch_profile "$CAROL" "{\"display_name\":\"$(python3 -c "print('a'*41)")\"}")" "400"
check "a bio over 500 characters is refused" \
    "$(patch_profile "$CAROL" "{\"bio\":\"$(python3 -c "print('b'*501)")\"}")" "400"
check "a display name cannot contain a line break" "$(patch_profile "$CAROL" '{"display_name":"two\nlines"}')" "400"
check "nor other control characters" "$(patch_profile "$CAROL" '{"bio":"bell\u0007"}')" "400"
# A right-to-left override would visually rewrite the @username beside the name.
check "nor a text-direction override" "$(patch_profile "$CAROL" '{"display_name":"evil\u202Eeci"}')" "400"
check "nor a bidi isolate, even in a bio" "$(patch_profile "$CAROL" '{"bio":"a\u2066b"}')" "400"
check "a name must be text" "$(patch_profile "$CAROL" '{"display_name":42}')" "400"
patch_profile "$CAROL" '{"display_name":""}' >/dev/null
check "an empty value clears the field" "$(curl -s -b "$BOB" "$BASE/api/users/carol" | field display_name)" "-"

echo
echo "=== pictures ==="
img png 400 200 SECRET-LOCATION > "$WORK/wide.png"
img png 300 300 > "$WORK/square.png"
head -c 5000 /dev/urandom > "$WORK/noise.bin"
printf '<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>' > "$WORK/pic.svg"
img bomb > "$WORK/bomb.png"
# Valid images either side of the 50-megapixel limit. The one over it decodes perfectly
# well, so only the pixel check can refuse it.
img png1bit 7100 7100 > "$WORK/over.png"
img png1bit 7000 7000 > "$WORK/under.png"
head -c $((11 * 1024 * 1024)) /dev/zero > "$WORK/huge.bin"
: > "$WORK/empty.bin"

check "uploading needs a session" \
    "$(code -X PUT "$BASE/api/me/avatar" --data-binary "@$WORK/square.png")" "401"
URL1=$(put_avatar "$BOB" "$WORK/wide.png" | field avatar)
case "$URL1" in /api/avatars/*/*) ok "a picture is accepted and given a URL" ;; *) bad "a picture is accepted and given a URL" "$URL1" ;; esac
BOB_ID=$(sql "SELECT id FROM users WHERE username='bob';")
V1=${URL1##*/}

check "pictures are for members only" "$(code "$BASE$URL1")" "401"
curl -s -D "$WORK/h" -o "$WORK/got.webp" -b "$CAROL" "$BASE$URL1"
grep -qi '^content-type: image/webp' "$WORK/h" && ok "served as WebP" || bad "served as WebP" "$(grep -i content-type "$WORK/h")"
grep -qi '^cache-control: private, max-age=31536000, immutable' "$WORK/h" \
    && ok "cached for good, privately — the URL changes with the picture" \
    || bad "cached for good, privately" "$(grep -i cache-control "$WORK/h")"
check "cropped square and scaled to 256" "$(img info < "$WORK/got.webp" | cut -d' ' -f1-2)" "256 256"
# The point of stripping: a phone photo's EXIF holds where it was taken.
check "no EXIF block survives" "$(img info < "$WORK/got.webp" | cut -d' ' -f3)" "no-exif"
python3 -c "import sys;sys.exit(b'SECRET-LOCATION' in open('$WORK/got.webp','rb').read())" \
    && ok "and nothing of the original's metadata is in the bytes" \
    || bad "and nothing of the original's metadata is in the bytes" "marker found"
check "the upload itself is not kept — one file, the rendered picture" \
    "$(find "$DATA/avatars" -type f | wc -l)" "1"
check "shown on the profile" "$(curl -s -b "$CAROL" "$BASE/api/users/bob" | field avatar)" "$URL1"
check "and in /api/me" "$(curl -s -b "$BOB" "$BASE/api/me" | field avatar)" "$URL1"

check "random bytes are refused" "$(put_avatar_code "$BOB" "$WORK/noise.bin")" "415"
check "so is SVG — libvips is never handed it" "$(put_avatar_code "$BOB" "$WORK/pic.svg")" "415"
check "an empty body is refused" "$(put_avatar_code "$BOB" "$WORK/empty.bin")" "400"
check "over 10 MB is refused" "$(put_avatar_code "$BOB" "$WORK/huge.bin")" "413"
check "a small file claiming 100,000 x 100,000 pixels is refused before decoding" \
    "$(put_avatar_code "$BOB" "$WORK/bomb.png")" "422"
check "a valid image just over 50 megapixels is refused ($(stat -c%s "$WORK/over.png") bytes of file)" \
    "$(put_avatar_code "$BOB" "$WORK/over.png")" "422"
check "one just under is accepted" "$(put_avatar_code "$BOB" "$WORK/under.png")" "200"
URL1=$(put_avatar "$BOB" "$WORK/wide.png" | field avatar); V1=${URL1##*/}
check "a refused upload leaves the current picture in place" \
    "$(put_avatar_code "$BOB" "$WORK/noise.bin"; curl -s -b "$CAROL" "$BASE/api/users/bob" | field avatar)" "415$URL1"

URL2=$(put_avatar "$BOB" "$WORK/square.png" | field avatar)
[ "$URL2" != "$URL1" ] && ok "a new picture gets a new URL" || bad "a new picture gets a new URL" "$URL2"
check "the old URL stops working" "$(code -b "$CAROL" "$BASE$URL1")" "404"
[ -f "$DATA/avatars/$BOB_ID-$V1.webp" ] && bad "and the old file is deleted at once" "still there" \
    || ok "and the old file is deleted at once"
check "a crafted version cannot reach outside the folder" \
    "$(code -b "$CAROL" "$BASE/api/avatars/$BOB_ID/..%2F..%2Fdb%2Farchive.db")" "404"

check "removing it" "$(code -b "$BOB" -X DELETE "$BASE/api/me/avatar")" "200"
check "leaves no picture on the profile" "$(curl -s -b "$CAROL" "$BASE/api/users/bob" | field avatar)" "-"
check "and no file" "$(find "$DATA/avatars" -type f | wc -l)" "0"

echo
echo "=== rooms on a profile are rooms you share ==="
# Who is in a room is visible only to its members. A profile listing every room its
# owner is in would reveal that to anyone who opened it.
room() { curl -s -b "$1" -X POST "$BASE/api/chat/rooms" -H 'Content-Type: application/json' \
         -d "{\"name\":\"$2\"}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["id"])'; }
BOTH=$(room "$ALICE" "Both")
curl -s -o /dev/null -b "$ALICE" -X POST "$BASE/api/chat/rooms/$BOTH/invite" -H 'Content-Type: application/json' -d '{"username":"bob"}'
curl -s -o /dev/null -b "$BOB" -X POST "$BASE/api/chat/rooms/$BOTH/respond" -H 'Content-Type: application/json' -d '{"action":"accept"}'
room "$ALICE" "Alice alone" >/dev/null
room "$CAROL" "Carol alone" >/dev/null
rooms_seen() { curl -s -b "$1" "$BASE/api/users/$2" \
    | python3 -c 'import sys,json;print(",".join(r["name"] for r in json.load(sys.stdin)["shared_rooms"]))'; }
check "bob sees only the room he and alice share" "$(rooms_seen "$BOB" alice)" "Both"
check "carol, who shares none, sees none" "$(rooms_seen "$CAROL" alice)" ""
check "alice sees all of her own" "$(rooms_seen "$ALICE" alice)" "Alice alone,Both"

echo
echo "=== profiles in chat ==="
put_avatar "$BOB" "$WORK/square.png" >/dev/null
BOB_PIC=$(curl -s -b "$BOB" "$BASE/api/me" | field avatar)
curl -s -o /dev/null -b "$BOB" -X POST "$BASE/api/chat/rooms/$BOTH/messages" -H 'Content-Type: application/json' -d '{"body":"hello"}'
MSG=$(curl -s -b "$ALICE" "$BASE/api/chat/rooms/$BOTH/messages" | python3 -c 'import sys,json;print(json.dumps(json.load(sys.stdin)["messages"][-1]))')
# The username attributes the message; a display name is only what its owner typed.
check "a message is still attributed to the username" "$(printf '%s' "$MSG" | field author)" "bob"
check "with the display name beside it" "$(printf '%s' "$MSG" | field author_display_name)" "Bob the Builder"
check "and the picture" "$(printf '%s' "$MSG" | field author_avatar)" "$BOB_PIC"
MEM=$(curl -s -b "$ALICE" "$BASE/api/chat/rooms/$BOTH/members" \
      | python3 -c 'import sys,json;print(json.dumps([m for m in json.load(sys.stdin)["members"] if m["username"]=="bob"][0]))')
check "the member list carries both too" \
    "$(printf '%s' "$MEM" | field display_name)|$(printf '%s' "$MEM" | field avatar)" "Bob the Builder|$BOB_PIC"

echo
echo "=== a download page knows whether its viewer is a member ==="
printf 'x' > "$WORK/f.bin"
LOC=$(curl -s -D - -o /dev/null -b "$ALICE" -X POST "$BASE/files" -H "Upload-Length: 1" \
      -H "Upload-Metadata: filename $(printf f.bin | base64 -w0)" | tr -d '\r' | awk 'tolower($1)=="location:"{print $2}')
curl -s -o /dev/null -b "$ALICE" -X PATCH "$BASE$LOC" -H 'Content-Type: application/offset+octet-stream' \
    -H 'Upload-Offset: 0' --data-binary "@$WORK/f.bin"
TOKEN=$(curl -s -b "$ALICE" -X POST "$BASE/api/shares" -H 'Content-Type: application/json' \
        -d "{\"uploads\":[\"${LOC##*/}\"],\"public\":true}" | field token)
check "someone holding only the link is not" "$(curl -s "$BASE/api/shares/$TOKEN" | field viewer_is_member)" "False"
check "a signed-in member is" "$(curl -s -b "$BOB" "$BASE/api/shares/$TOKEN" | field viewer_is_member)" "True"

echo
echo "=== a deleted account's picture is swept ==="
put_avatar "$CAROL" "$WORK/square.png" >/dev/null
CAROL_ID=$(sql "SELECT id FROM users WHERE username='carol';")
CAROL_FILE=$(find "$DATA/avatars" -name "$CAROL_ID-*.webp")
check "carol has a picture on disk" "$([ -n "$CAROL_FILE" ] && echo yes)" "yes"
check "alice deletes carol's account" "$(code -b "$ALICE" -X DELETE "$BASE/api/admin/users/$CAROL_ID")" "200"
# The sweep leaves pictures younger than ten minutes alone, so it can never delete one
# between its file being written and the database pointing at it. Age this one past that.
touch -d '20 minutes ago' "$CAROL_FILE"
sleep 3
[ -f "$CAROL_FILE" ] && bad "the sweep deletes it" "still there" || ok "the sweep deletes it"
[ -f "$DATA/avatars/$BOB_ID-${BOB_PIC##*/}.webp" ] && ok "and leaves a current picture alone" \
    || bad "and leaves a current picture alone" "bob's is gone"
# The grace, from the other side: a young file nobody points at yet survives a sweep.
cp "$DATA/avatars/$BOB_ID-${BOB_PIC##*/}.webp" "$DATA/avatars/$BOB_ID-notyetcommitted.webp"
sleep 3
[ -f "$DATA/avatars/$BOB_ID-notyetcommitted.webp" ] && ok "a picture still being saved is not swept" \
    || bad "a picture still being saved is not swept" "it was"

echo
echo "=============================="
echo " $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ] || { echo; echo "--- server log ---"; tail -25 "$WORK/server.log"; }
exit $((FAIL > 0))
