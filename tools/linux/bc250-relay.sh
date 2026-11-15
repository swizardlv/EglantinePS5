#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run on the BC250, outside the capture's timeout, to serialize whole test turns.
set -euo pipefail
[[ $# -gt 0 ]] || { echo "usage: bc250-relay.sh COMMAND [ARGS...]" >&2; exit 2; }
if [[ ${BC250_RELAY_HELD:-0} == 1 ]]; then
    exec "$@"
fi
lease=${BC250_RELAY_LOCK:-$HOME/.cache/eglantineps5-bc250/board.lock}
mkdir -p -- "$(dirname -- "$lease")"
echo "BC250 relay: queued; waiting for the current test to finish" >&2
# The flock parent keeps ownership; the child closes the descriptor so an AppImage
# mount helper cannot retain it after the capture exits. Nested launchers inherit
# the marker and use the enclosing capture's lease instead of locking twice.
exec env BC250_RELAY_HELD=1 flock --close --exclusive "$lease" "$@"
