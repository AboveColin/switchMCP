#!/usr/bin/env bash
# One-command dev loop for switchMCP: build the agent and hot-deploy it to the
# console over the network (no FTP, no SD pull). See https://github.com/AboveColin/switchMCP/wiki/Development.
#
#   ./dev.sh              build + push + reboot + smoke-test
#   ./dev.sh --no-reboot  build + push only (loads on next boot)
#   ./dev.sh build        build the agent only
#   ./dev.sh smoke        run the live smoke test against the console
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
# Deliberately no default IP. A placeholder here would silently point the deploy
# at whatever machine happens to own that address on someone else's network, and
# the failure would look like "the console is broken" rather than "you did not
# set this". Put your own in .env (gitignored) or the environment.
[ -f "$ROOT/.env" ] && . "$ROOT/.env"
export SWITCH_TOKEN="${SWITCH_TOKEN:-$(cat "$ROOT/.device-token" 2>/dev/null || true)}"

if [ -z "${SWITCH_HOST:-}" ]; then
  echo "SWITCH_HOST is not set (your console's IP)." >&2
  echo "  export SWITCH_HOST=10.0.0.5    # or put it in ./.env" >&2
  echo "  Don't know it? Settings > Internet on the console, or:" >&2
  echo "  SWITCH_TOKEN=\$(cat .device-token) server/.venv/bin/python -c \\" >&2
  echo "    'import asyncio,os,sys; sys.path.insert(0,\"server/src\");" \
       "from switch_mcp.discovery import scan;" \
       "print(asyncio.run(scan(token=os.environ[\"SWITCH_TOKEN\"])))'" >&2
  exit 2
fi
export SWITCH_HOST

PY="$ROOT/server/.venv/bin/python"

build() { echo "== building agent =="; make -C "$ROOT/agent"; }

case "${1:-deploy}" in
  build) build ;;
  smoke) ( cd "$ROOT/server" && "$PY" tests/live_smoke.py ) ;;
  deploy|"") build; ( cd "$ROOT/server" && "$PY" tests/dev_deploy.py ) ;;
  --no-reboot) build; ( cd "$ROOT/server" && "$PY" tests/dev_deploy.py --no-reboot ) ;;
  *) echo "usage: dev.sh [deploy|build|smoke|--no-reboot]"; exit 2 ;;
esac
