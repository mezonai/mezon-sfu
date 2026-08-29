#!/bin/sh
set -eu

host="${SFU_HEALTH_HOST:-127.0.0.1}"
port="${SFU_SIGNALING_PORT:-8000}"
request='GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n'

response=$(printf '%b' "$request" | nc -w 2 "$host" "$port" 2>/dev/null || true)
printf '%s' "$response" | grep -q ' 101 '
