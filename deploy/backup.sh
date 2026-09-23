#!/usr/bin/env bash
#
# Backs up the database, then deletes backups older than the retention period.
#
#   deploy/backup.sh
#
# Runs daily from a systemd timer (install it with deploy/install-backup-timer.sh) and
# before every update (the README's update command calls it first). Daily matters for
# more than safety: pruning only happens when this runs, so without a schedule "backups
# are kept 14 days" would be false through any stretch with no deploys.
#
# Retention is ARCHIVE_BACKUP_DAYS in deploy/.env, default 14 — the same value the app
# reads for the privacy notice, so the period the notice states is the one applied here.
# A backup is deleted once it is strictly older than that, at the next run after, so at
# most a day late.
#
# A backup is the SQLite database only: accounts, shares, chat. Uploaded files are not
# copied. They expire with their links, and copying them would keep files the notice
# says are deleted.
#
# Needs sqlite3 on the host. The backup API gives a consistent copy while the app runs.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ENV_FILE="$HERE/.env"
[ -f "$ENV_FILE" ] || { echo "no $ENV_FILE" >&2; exit 1; }

# Single keys rather than sourcing the file: .env is written for compose, which allows
# unquoted spaces (ARCHIVE_OPERATOR_NAME=Jane Doe) that a shell would try to run.
setting() { grep -E "^$1=" "$ENV_FILE" | tail -1 | cut -d= -f2- || true; }

DATA_DIR=$(setting ARCHIVE_DATA_DIR)
[ -n "$DATA_DIR" ] || { echo "ARCHIVE_DATA_DIR is not set in $ENV_FILE" >&2; exit 1; }
BACKUP_DIR=$(setting ARCHIVE_BACKUP_DIR)
BACKUP_DIR=${BACKUP_DIR:-$(dirname "$DATA_DIR")/backups}
DAYS=$(setting ARCHIVE_BACKUP_DAYS)
DAYS=${DAYS:-14}
[[ "$DAYS" =~ ^[1-9][0-9]*$ ]] || { echo "ARCHIVE_BACKUP_DAYS must be a positive whole number" >&2; exit 1; }

DB="$DATA_DIR/db/archive.db"
[ -f "$DB" ] || { echo "no database at $DB" >&2; exit 1; }
mkdir -p "$BACKUP_DIR"

OUT="$BACKUP_DIR/archive-$(date +%Y%m%d-%H%M%S).db"
sqlite3 "$DB" ".backup '$OUT'"
# An unverified backup is a hope, not a backup.
if [ "$(sqlite3 "$OUT" 'PRAGMA integrity_check;')" != "ok" ]; then
    rm -f "$OUT"
    echo "backup failed its integrity check and was removed" >&2
    exit 1
fi
echo "backed up to $OUT"

# By minutes rather than find's -mtime, which rounds ages down to whole days and would
# keep a backup up to a day longer than the period stated.
find "$BACKUP_DIR" -maxdepth 1 -type f -name 'archive-*.db' -mmin "+$((DAYS * 1440))" \
    -print -delete | sed 's/^/pruned /'
