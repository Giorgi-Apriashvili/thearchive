#!/usr/bin/env bash
#
# Installs a systemd user timer that runs deploy/backup.sh daily.
#
#   deploy/install-backup-timer.sh
#
# Run once, as the user who owns the deployment — not root. Safe to run again: it
# rewrites the units, which is how to pick up a moved repository.
#
# A user timer only runs while that user has a systemd instance, so lingering must be on
# (loginctl enable-linger <user>), or the timer stops at logout. Persistent=true catches
# up on a run missed while the machine was off.
#
# The units are written here rather than kept as files in the repository because they
# need this checkout's absolute path, which differs per host.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
UNITS="$HOME/.config/systemd/user"
mkdir -p "$UNITS"

if [ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" != "yes" ]; then
    echo "warning: lingering is off for $USER, so the timer will stop when you log out." >&2
    echo "         enable it with: loginctl enable-linger $USER" >&2
fi

cat > "$UNITS/thearchive-backup.service" <<EOF
[Unit]
Description=Back up the WeekendArchive database and prune old backups

[Service]
Type=oneshot
ExecStart=$HERE/backup.sh
EOF

cat > "$UNITS/thearchive-backup.timer" <<EOF
[Unit]
Description=Daily WeekendArchive database backup

[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true

[Install]
WantedBy=timers.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now thearchive-backup.timer
systemctl --user list-timers thearchive-backup.timer --no-pager
