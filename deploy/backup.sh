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
# copied: they expire with their links, and copying them would keep files the notice
# says are deleted. With ARCHIVE_OFFSITE_TARGET set, each backup is also copied off this
# machine, encrypted, by offsite.sh.
#
# With ARCHIVE_HEALTHCHECK_URL set, each run reports to a dead-man's switch (see the
# README's Monitoring section): started, then success or failure with a short summary.
# A failed off-site copy, or the data disk past ARCHIVE_DISK_WARN_PERCENT (default 90),
# reports as a failure while still exiting 0, so neither blocks an update.
#
# Needs sqlite3 on the host. The backup API gives a consistent copy while the app runs.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ENV_FILE="$HERE/.env"
[ -f "$ENV_FILE" ] || { echo "no $ENV_FILE" >&2; exit 1; }

# Single keys rather than sourcing the file: .env is written for compose, which allows
# unquoted spaces (ARCHIVE_OPERATOR_NAME=Jane Doe) that a shell would try to run.
setting() { grep -E "^$1=" "$ENV_FILE" | tail -1 | cut -d= -f2- || true; }

# Optional dead-man's switch (healthchecks.io or anything that speaks its ping protocol).
# Each run reports in; a run that fails says so, and a run that never happens — timer
# off, server down — is noticed by its silence. A ping that cannot be delivered is only
# noted here: its absence is what raises the alarm.
HEALTHCHECK_URL=$(setting ARCHIVE_HEALTHCHECK_URL)
ping_check() {  # <suffix: "" | /start | /fail> <message>
    [ -n "$HEALTHCHECK_URL" ] || return 0
    curl -fsS -m 10 --retry 3 -o /dev/null --data-raw "$2" "$HEALTHCHECK_URL$1" \
        || echo "could not reach the health check" >&2
}
REPORTED=0
STEP="reading the settings"
# Any early exit — a failed integrity check, a missing database, a bad setting — reports
# as a failure, naming the step it happened in.
trap 'rc=$?; if [ "$rc" -ne 0 ] && [ "$REPORTED" -eq 0 ]; then
          ping_check /fail "backup.sh failed (exit $rc) while $STEP"; fi' EXIT
ping_check /start "started"

DATA_DIR=$(setting ARCHIVE_DATA_DIR)
[ -n "$DATA_DIR" ] || { echo "ARCHIVE_DATA_DIR is not set in $ENV_FILE" >&2; exit 1; }
BACKUP_DIR=$(setting ARCHIVE_BACKUP_DIR)
BACKUP_DIR=${BACKUP_DIR:-$(dirname "$DATA_DIR")/backups}
DAYS=$(setting ARCHIVE_BACKUP_DAYS)
DAYS=${DAYS:-14}
[[ "$DAYS" =~ ^[1-9][0-9]*$ ]] || { echo "ARCHIVE_BACKUP_DAYS must be a positive whole number" >&2; exit 1; }

WARN_PERCENT=$(setting ARCHIVE_DISK_WARN_PERCENT)
WARN_PERCENT=${WARN_PERCENT:-90}

STEP="looking for the database"
DB="$DATA_DIR/db/archive.db"
[ -f "$DB" ] || { echo "no database at $DB" >&2; exit 1; }
mkdir -p "$BACKUP_DIR"

STEP="backing up the database"
OUT="$BACKUP_DIR/archive-$(date +%Y%m%d-%H%M%S).db"
sqlite3 "$DB" ".backup '$OUT'"
# An unverified backup is a hope, not a backup.
if [ "$(sqlite3 "$OUT" 'PRAGMA integrity_check;')" != "ok" ]; then
    rm -f "$OUT"
    echo "backup failed its integrity check and was removed" >&2
    exit 1
fi
echo "backed up to $OUT"

STEP="pruning old backups"
# By minutes rather than find's -mtime, which rounds ages down to whole days and would
# keep a backup up to a day longer than the period stated.
find "$BACKUP_DIR" -maxdepth 1 -type f -name 'archive-*.db' -mmin "+$((DAYS * 1440))" \
    -print -delete | sed 's/^/pruned /'
# Databases restore.sh set aside are backups too, as far as the notice is concerned.
ASIDE_DIR="$(dirname "$BACKUP_DIR")/restore-aside"
if [ -d "$ASIDE_DIR" ]; then
    find "$ASIDE_DIR" -mindepth 1 -maxdepth 1 -type d -mmin "+$((DAYS * 1440))" \
        -print -exec rm -rf {} + | sed 's/^/pruned /'
fi

# Off-site copies, when configured. A failure there is reported but does not fail this
# script: the local backup above is what the update command waits on, and an unreachable
# storage box should not block a deploy. The next run sends whatever this one missed.
STEP="copying off-site"
PROBLEMS=()
if [ -n "$(setting ARCHIVE_OFFSITE_TARGET)" ]; then
    if offsite=$("$HERE/offsite.sh" 2>&1); then
        echo "$offsite"
        last=${offsite##*$'\n'}
        OFFSITE="off-site: ${last#off-site copy }"
    else
        echo "$offsite" >&2
        echo "OFFSITE COPY FAILED: the local backup is fine, but it was not copied off this machine" >&2
        # The first line is the cause (ssh, age); rsync's own closing line is generic.
        OFFSITE="off-site: FAILED — ${offsite%%$'\n'*}"
        PROBLEMS+=("the off-site copy failed")
    fi
else
    OFFSITE="off-site: not configured"
fi

# The disk filling up is the other slow failure worth hearing about before it bites:
# uploads stop, and so would the next backup.
STEP="checking free space"
DISK_USED=$(df -P "$DATA_DIR" | awk 'NR == 2 { sub("%", "", $5); print $5 }')
if [ "$DISK_USED" -ge "$WARN_PERCENT" ]; then
    PROBLEMS+=("the data disk is ${DISK_USED}% full")
fi

SUMMARY="backup: $(basename "$OUT") ($(du -h "$OUT" | cut -f1))
$OFFSITE
disk: ${DISK_USED}% used (warns at ${WARN_PERCENT}%)"
REPORTED=1
if [ ${#PROBLEMS[@]} -gt 0 ]; then
    # Reported as a failure, but the exit status stays 0: the local backup is good, and
    # the update command that runs this first should not be blocked by either problem.
    ping_check /fail "PROBLEM: $(printf '%s; ' "${PROBLEMS[@]}" | sed 's/; $//')
$SUMMARY"
else
    ping_check "" "$SUMMARY"
fi
