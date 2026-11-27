#!/usr/bin/env bash
# Install systemd services that start the dashboard and MQTT worker on boot.
# Run as the normal user from anywhere in the repo: deployment/systemd/install.sh
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SERVICE_USER="${SUDO_USER:-$USER}"
PYTHON="$REPO/backend/.venv/bin/python"

if [ ! -x "$PYTHON" ]; then
    echo "Missing $PYTHON - create backend/.venv and install requirements first." >&2
    exit 1
fi

for name in oximeter-dashboard oximeter-mqtt; do
    sed -e "s#@USER@#$SERVICE_USER#g" -e "s#@REPO@#$REPO#g" \
        "$REPO/deployment/systemd/$name.service.in" \
        | sudo tee "/etc/systemd/system/$name.service" >/dev/null
done

sudo systemctl daemon-reload
sudo systemctl enable oximeter-dashboard oximeter-mqtt
sudo systemctl restart oximeter-dashboard oximeter-mqtt

systemctl --no-pager --lines=5 status oximeter-dashboard oximeter-mqtt || true
