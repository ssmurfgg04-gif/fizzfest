#!/usr/bin/env bash
set -e
PORT="${PORT:-80}"
exec ./server "$PORT"
