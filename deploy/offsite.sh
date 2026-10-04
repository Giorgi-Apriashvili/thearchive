#!/usr/bin/env bash
#
# Copies the database backups off this machine, encrypted. Called by backup.sh after each
# backup when ARCHIVE_OFFSITE_TARGET is set in deploy/.env; can also be run on its own.
#
#   deploy/offsite.sh
#
# The local backups sit on the same disk as the database, so a failed disk or a lost
# server would take both. This keeps a copy of each somewhere else.
#
# Each backup is encrypted with age to ARCHIVE_OFFSITE_RECIPIENT, a public key. Its private
# key is kept off this machine, so neither this server nor the storage it ships to can
# read the copies; only whoever holds that key, for a restore.
#
# The encrypted copies live in an outbox beside the backups, which is mirrored to the
# target with rsync --delete. The remote therefore holds exactly what is held here: the
# retention backup.sh applies — and the privacy notice states — applies off-site too
# without a second pruning rule, and a run that was missed is caught up by the next.
# The flip side: whoever controls this server can delete the off-site copies. They guard
# against losing the machine, not against someone who has taken it over.
#
# ARCHIVE_OFFSITE_TARGET is an rsync destination. For a Hetzner Storage Box sub-account:
#   ARCHIVE_OFFSITE_TARGET=u123456-sub1@u123456-sub1.your-storagebox.de:backups
# connected to on port 23 with the key ~/.ssh/thearchive-offsite (see the README). A plain
# directory path also works, which is how this is tested.
#
# Needs age and rsync on the host.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ENV_FILE="$HERE/.env"
[ -f "$ENV_FILE" ] || { echo "no $ENV_FILE" >&2; exit 1; }
setting() { grep -E "^$1=" "$ENV_FILE" | tail -1 | cut -d= -f2- || true; }

TARGET=$(setting ARCHIVE_OFFSITE_TARGET)
RECIPIENT=$(setting ARCHIVE_OFFSITE_RECIPIENT)
[ -n "$TARGET" ] || { echo "ARCHIVE_OFFSITE_TARGET is not set in $ENV_FILE" >&2; exit 1; }
[[ "$RECIPIENT" == age1* ]] || { echo "ARCHIVE_OFFSITE_RECIPIENT must be an age public key (age1…)" >&2; exit 1; }
command -v age >/dev/null || { echo "age is not installed" >&2; exit 1; }
command -v rsync >/dev/null || { echo "rsync is not installed" >&2; exit 1; }

DATA_DIR=$(setting ARCHIVE_DATA_DIR)
BACKUP_DIR=$(setting ARCHIVE_BACKUP_DIR)
BACKUP_DIR=${BACKUP_DIR:-$(dirname "$DATA_DIR")/backups}
OUTBOX="$BACKUP_DIR/offsite"
mkdir -p "$OUTBOX"

# Encrypt what is new. Written under a temporary name and renamed when complete, so an
# interrupted run never leaves a truncated file to be shipped as if it were whole.
for db in "$BACKUP_DIR"/archive-*.db; do
    [ -e "$db" ] || continue
    out="$OUTBOX/$(basename "$db").age"
    # Newer than its copy only if two backups landed in the same second and the second
    # replaced the first under the same name; the copy must follow it.
    [ "$out" -nt "$db" ] && continue
    age -r "$RECIPIENT" -o "$out.partial" "$db"
    mv "$out.partial" "$out"
done
rm -f "$OUTBOX"/*.partial

# Drop what backup.sh has pruned, so the mirror below prunes it remotely too.
for enc in "$OUTBOX"/archive-*.db.age; do
    [ -e "$enc" ] || continue
    [ -e "$BACKUP_DIR/$(basename "$enc" .age)" ] || rm -f "$enc"
done

# Storage Box SSH listens on port 23. BatchMode: a missing key fails at once rather than
# waiting at a password prompt nobody will answer from a timer.
SSH="ssh -p 23 -i $HOME/.ssh/thearchive-offsite -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=30"
rsync -a --delete --include='archive-*.db.age' --exclude='*' -e "$SSH" "$OUTBOX/" "$TARGET/"

touch "$OUTBOX/.last-ok"
echo "off-site copy up to date: $(find "$OUTBOX" -maxdepth 1 -name 'archive-*.db.age' | wc -l) encrypted backup(s)"
