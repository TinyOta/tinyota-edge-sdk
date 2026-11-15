#!/bin/sh
# Called by the TinyOTA agent after SHA-256 and signature checks pass:
#   install-app.sh <verified image> <new version>
# Example: the upload is a single executable that replaces /opt/myapp/myapp.
# Exit non-zero to report the update as failed.
set -eu
IMAGE="$1"
VERSION="$2"
APP=/opt/myapp/myapp

install -m 0755 "$IMAGE" "$APP.new"
cp -p "$APP" "$APP.prev" 2>/dev/null || true
mv "$APP.new" "$APP"                      # atomic on the same filesystem

if ! systemctl restart myapp.service; then
    mv "$APP.prev" "$APP" && systemctl restart myapp.service || true
    exit 1
fi
echo "installed myapp $VERSION"
