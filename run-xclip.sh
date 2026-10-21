#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${XDG_SESSION_TYPE:-}" != wayland || -z "${WAYLAND_DISPLAY:-}" || -z "${DISPLAY:-}" ]]; then
  echo "Launch this build from a GNOME Wayland session with Xwayland available." >&2
  exit 1
fi
export DESKFLOW_CLIPBOARD=xclip
export XDG_CONFIG_HOME="$project_dir/run/config"
export QT_QPA_PLATFORM=wayland
if [[ ! -f "$XDG_CONFIG_HOME/Deskflow/Deskflow.conf" ]]; then
  echo "Run ./prepare-xclip-config.py first to copy your existing configuration." >&2
  exit 1
fi
exec "$project_dir/build-wayland/bin/deskflow" "$@"
