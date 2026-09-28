#!/usr/bin/env bash
# Deploy gx-rack/ (dashboard + REST + MCP server) to a Docker host over ssh
# and rebuild it.
#
#   GX_RACK_HOST=user@server agent/deploy-gx-rack.sh
#   GX_RACK_HOST=user@server GX_RACK_DIR=/opt/gx-rack agent/deploy-gx-rack.sh
#
# The server needs Docker with the compose plugin, and a .env in GX_RACK_DIR
# (template: gx-rack/.env.example). That .env is never copied, overwritten or
# printed by this script. Running Docker on this machine instead? Just
# `cd gx-rack && docker compose up -d --build`.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${GX_RACK_HOST:?set GX_RACK_HOST=user@server}"
DIR="${GX_RACK_DIR:-gx-rack}"   # relative paths are under the remote home

tar -C "$REPO/gx-rack" --exclude='.env' --exclude='__pycache__' -czf - . \
  | ssh -o BatchMode=yes "$HOST" "mkdir -p '$DIR' && cd '$DIR' && tar xzf - &&
      { test -f .env || { echo 'no .env on the server -- see gx-rack/README.md' >&2; exit 1; }; } &&
      docker compose up -d --build 2>&1 | tail -2 && sleep 6 &&
      docker ps --filter name=gx-rack --format '{{.Names}} {{.Status}}'"
