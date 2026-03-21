#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MODULE_ID="plaits"
DEVICE="${MOVE_HOST:-move.local}"
REMOTE_MODULES_DIR="/data/UserData/schwung/modules/sound_generators"

# Build first if no dist exists
if [ ! -f "$REPO_ROOT/dist/${MODULE_ID}-module.tar.gz" ]; then
    echo "No build found, building first..."
    "$SCRIPT_DIR/build.sh"
fi

echo "==> Installing $MODULE_ID to $DEVICE..."

# Create remote directory and extract
ssh ableton@$DEVICE "mkdir -p $REMOTE_MODULES_DIR"
scp "$REPO_ROOT/dist/${MODULE_ID}-module.tar.gz" "ableton@$DEVICE:~/"
ssh ableton@$DEVICE "cd $REMOTE_MODULES_DIR && tar -xzf ~/${MODULE_ID}-module.tar.gz"

echo "Done! Restart Schwung to load the module."
