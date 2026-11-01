#!/usr/bin/env bash
# Baut das Plugin mit dem Builder aus sd88mes mpc-vst-plugins (MPC_VST zeigt auf dessen Ordner; GitHub setzt das selbst).
# Zuerst erzeugt gen_ui.py den Bildschirm (Layout, Parameter, Bilder), dann baut der Builder das Plugin.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ -x "${MPC_VST:-/nonexistent}/tools/build_port.sh" ] || { echo "MPC_VST muss auf einen Ordner mit mpc-vst-plugins zeigen" >&2; exit 1; }
python3 "$here/gen_ui.py"
exec "$MPC_VST/tools/build_port.sh" "$here/vst.json"
