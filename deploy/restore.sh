#!/usr/bin/env bash
#
# Puts a database backup back in place.
#
#   deploy/restore.sh                 lists the backups, newest first; changes nothing
#   deploy/restore.sh <backup.db>     restores that one, after asking
#   deploy/restore.sh --yes <backup.db>   …without asking
#
# <backup.db> is one of the files the listing shows, or any SQLite backup — such as an
# off-site copy decrypted elsewhere with `age -d` and copied here (see the README).
#
# Nothing is deleted. The database being replaced is moved, with its -wal and -shm files,
# to a restore-aside directory beside the backups, and the script ends by printing the
# commands that would put it back. backup.sh prunes that directory on the same schedule
# as the backups, since what it holds is one. The backup is checked before the app is stopped, so a
# damaged file is refused while the site is still up.
#
# Everything after the backup was taken is lost: messages, accounts and links made since.
# Uploaded files are not in backups and are not touched.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ENV_FILE="$HERE/.env"
[ -f "$ENV_FILE" ] || { echo "no $ENV_FILE" >&2; exit 1; }
setting() { grep -E "^$1=" "$ENV_FILE" | tail -1 | cut -d= -f2- || true; }

DATA_DIR=$(setting ARCHIVE_DATA_DIR)
[ -n "$DATA_DIR" ] || { echo "ARCHIVE_DATA_DIR is not set in $ENV_FILE" >&2; exit 1; }
BACKUP_DIR=$(setting ARCHIVE_BACKUP_DIR)
BACKUP_DIR=${BACKUP_DIR:-$(dirname "$DATA_DIR")/backups}
DB="$DATA_DIR/db/archive.db"
PUBLIC_URL=$(setting ARCHIVE_PUBLIC_URL)
COMPOSE=(docker compose -f "$HERE/docker-compose.yml")

schema() { sqlite3 "file:$1?mode=ro" 'PRAGMA user_version;' 2>/dev/null || echo '?'; }
accounts() { sqlite3 "file:$1?mode=ro" 'SELECT COUNT(*) FROM users;' 2>/dev/null || echo '?'; }

ASSUME_YES=0
if [ "${1:-}" = "--yes" ]; then ASSUME_YES=1; shift; fi

# ---- listing -------------------------------------------------------------------------
if [ $# -eq 0 ]; then
    echo "Current database: schema v$(schema "$DB"), $(accounts "$DB") account(s)"
    echo
    echo "Backups in $BACKUP_DIR, newest first:"
    # By the time in each name, not mtime: a copied file's mtime would not say when it was
    # taken. The time is the last part of the name, so a hand-named one such as
    # archive-pre-v12-20260923-095700.db sorts by its time too.
    found=0
    while IFS= read -r f; do
        found=1
        printf '  %-44s %6s  schema v%-3s %s account(s)\n' "$(basename "$f")" \
            "$(du -h "$f" | cut -f1)" "$(schema "$f")" "$(accounts "$f")"
    done < <(find "$BACKUP_DIR" -maxdepth 1 -type f -name 'archive-*.db' |
             sed -E 's/^(.*([0-9]{8}-[0-9]{6})\.db)$/\2 \1/' | sort -r | cut -d' ' -f2-)
    [ "$found" -eq 1 ] || echo "  (none)"
    echo
    echo "To restore one: $0 $BACKUP_DIR/<name>"
    exit 0
fi

# ---- checks, with the site still up --------------------------------------------------
SOURCE="$1"
[ -f "$SOURCE" ] || SOURCE="$BACKUP_DIR/$1"
[ -f "$SOURCE" ] || { echo "no such backup: $1" >&2; exit 1; }
[[ "$SOURCE" != *.age ]] || { echo "$SOURCE is encrypted; decrypt it first with age -d (see the README)" >&2; exit 1; }
if [ "$(sqlite3 "file:$SOURCE?mode=ro" 'PRAGMA integrity_check;' 2>&1)" != "ok" ]; then
    echo "$SOURCE failed its integrity check; refusing to restore it" >&2
    exit 1
fi

FROM=$(schema "$DB")
TO=$(schema "$SOURCE")
echo "Restore:  $SOURCE"
echo "          schema v$TO, $(accounts "$SOURCE") account(s)"
echo "Replaces: $DB"
echo "          schema v$FROM, $(accounts "$DB") account(s)"
echo
echo "Everything since that backup was taken will be lost. The site is down for a few seconds."
if [[ "$TO" =~ ^[0-9]+$ && "$FROM" =~ ^[0-9]+$ && "$TO" -lt "$FROM" ]]; then
    echo
    echo "Note: this backup predates a database upgrade (v$TO → v$FROM). The app upgrades it"
    echo "again when it starts. If you are undoing a bad update, roll the code back first"
    echo "(git checkout <commit>, then rebuild), or the same upgrade runs again."
fi
if [ "$ASSUME_YES" -ne 1 ]; then
    echo
    read -r -p "Type yes to restore: " answer
    [ "$answer" = "yes" ] || { echo "Nothing changed."; exit 1; }
fi

# ---- the swap ------------------------------------------------------------------------
ASIDE="$(dirname "$BACKUP_DIR")/restore-aside/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ASIDE"

"${COMPOSE[@]}" stop app
# Moved together: the -wal and -shm files belong to the database being replaced, and left
# beside the restored one SQLite would apply them on top of it.
for f in "$DB" "$DB-wal" "$DB-shm"; do
    [ -e "$f" ] && mv "$f" "$ASIDE/"
done
cp "$SOURCE" "$DB"
"${COMPOSE[@]}" start app

echo
echo "Restored. The replaced database is in $ASIDE"
echo "(kept as long as backups are, then deleted by backup.sh)"

# ---- did it come back ----------------------------------------------------------------
if [ -n "$PUBLIC_URL" ]; then
    up=0
    for _ in $(seq 1 30); do
        if curl -sf -o /dev/null "$PUBLIC_URL/healthz"; then up=1; break; fi
        sleep 1
    done
    if [ "$up" -eq 1 ]; then
        echo "The site is answering again at $PUBLIC_URL"
    else
        echo "The site has not answered within 30 seconds. Check: ${COMPOSE[*]} logs app" >&2
    fi
fi

echo
echo "To undo this restore:"
echo "  ${COMPOSE[*]} stop app"
echo "  rm -f $DB $DB-wal $DB-shm && mv $ASIDE/archive.db* $DATA_DIR/db/"
echo "  ${COMPOSE[*]} start app"
