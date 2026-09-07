#!/bin/bash
# Installs the PaceKeeper Dial calendar relay as a launchd user agent on macOS.
# Idempotent: safe to re-run after pulling a new calendar_feed.py.
set -euo pipefail

DIR=$(cd "$(dirname "$0")" && pwd)
LABEL=com.pacekeeper.calendar-feed
SUPPORT="$HOME/Library/Application Support/pacekeeper-calendar"
VENV="$SUPPORT/venv"
ENV_DIR="$HOME/.config/pacekeeper"
ENV_FILE="$ENV_DIR/calendar.env"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/pacekeeper-calendar.log"
UID_NUM=$(id -u)

echo "==> venv at $VENV"
mkdir -p "$SUPPORT" "$ENV_DIR" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv "$VENV"
fi
"$VENV/bin/python" -m pip install --quiet --upgrade pip
"$VENV/bin/python" -m pip install --quiet -r "$DIR/requirements.txt"

FRESH_ENV=0
if [ ! -f "$ENV_FILE" ]; then
    cp "$DIR/calendar.env.example" "$ENV_FILE"
    FRESH_ENV=1
fi
chmod 600 "$ENV_FILE"

echo "==> launch agent $PLIST"
sed -e "s|__PYTHON__|$VENV/bin/python|g" \
    -e "s|__SCRIPT__|$DIR/calendar_feed.py|g" \
    -e "s|__HOME__|$HOME|g" \
    "$DIR/$LABEL.plist.template" > "$PLIST"

if [ "$FRESH_ENV" = "1" ]; then
    cat <<EOF

  $ENV_FILE was created from the example and still has placeholders.
  Edit it (ICS_URL, MY_EMAIL) and then run:

      launchctl kickstart -k gui/$UID_NUM/$LABEL

  The agent is loaded but will log "ICS_URL is not set" until you do.

EOF
fi

launchctl bootout "gui/$UID_NUM" "$PLIST" 2>/dev/null || true
launchctl bootstrap "gui/$UID_NUM" "$PLIST"
launchctl enable "gui/$UID_NUM/$LABEL" 2>/dev/null || true

PORT=$(awk -F= '/^[[:space:]]*PORT[[:space:]]*=/ {gsub(/[[:space:]"]/, "", $2); print $2}' "$ENV_FILE" | tail -1)
PORT=${PORT:-8765}

echo "==> waiting for http://localhost:$PORT/health"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if curl -fsS "http://localhost:$PORT/health" 2>/dev/null; then
        echo
        break
    fi
    sleep 1
done

echo
echo "Log:      $LOG"
echo "Config:   $ENV_FILE"
echo "Restart:  launchctl kickstart -k gui/$UID_NUM/$LABEL"
echo "Test:     $VENV/bin/python $DIR/calendar_feed.py --once"
