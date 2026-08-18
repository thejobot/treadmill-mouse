#!/bin/zsh
# Wait for the DualSense to pair, then prove the dongle drives the cursor.
#
#   ./tools/pair_and_verify.sh [wait_seconds]
#
# Put the controller in pairing mode: hold CREATE + PS for ~5s until the
# light bar double-blinks. Then move the RIGHT STICK when prompted and keep
# your hands off the real mouse.

HERE="${0:A:h}/.."
PY="$HERE/.venv/bin/python"
[[ -x "$PY" ]] || PY="$(command -v python3)"
WAIT="${1:-180}"

echo "Waiting up to ${WAIT}s for the controller to connect..."
echo "  Hold CREATE + PS for ~5 seconds now."
echo

connected=0
elapsed=0
while (( elapsed < WAIT )); do
  c=$("$PY" "$HERE/tools/jp.py" BT.STATUS 2>/dev/null | grep -o '"connections": [0-9]*' | grep -o '[0-9]*$')
  if [[ -n "$c" && "$c" -gt 0 ]]; then
    connected=1
    break
  fi
  sleep 5
  elapsed=$((elapsed + 5))
done

if (( connected == 0 )); then
  echo "Controller never connected after ${WAIT}s."
  exit 1
fi

echo "Connected."
"$PY" "$HERE/tools/jp.py" PLAYERS.LIST 2>/dev/null | grep -E '"name"|"transport"'
echo
echo "Now MOVE THE RIGHT STICK in circles for 20 seconds. Do not touch the mouse."
echo "Starting in 5..."
sleep 5

"$PY" "$HERE/tools/verify_mouse.py" 20
