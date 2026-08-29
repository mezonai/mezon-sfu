#!/bin/sh
set -eu

: "${SFU_CONFIG_TEMPLATE:=/etc/mezon-sfu/config.ini.template}"
: "${SFU_CONFIG_FILE:=/run/mezon-sfu/config.ini}"

if [ -n "${SFU_JWT_SECRET_FILE:-}" ]; then
  [ -r "$SFU_JWT_SECRET_FILE" ] || { echo "SFU_JWT_SECRET_FILE is not readable" >&2; exit 1; }
  SFU_JWT_SECRET=$(tr -d '\r\n' < "$SFU_JWT_SECRET_FILE")
  export SFU_JWT_SECRET
fi

: "${SFU_JWT_SECRET:?Set SFU_JWT_SECRET or SFU_JWT_SECRET_FILE}"
case "$SFU_JWT_SECRET" in
  default|replace-with-*|changeme) echo "Refusing placeholder SFU_JWT_SECRET" >&2; exit 1 ;;
esac

set_default() {
  name=$1
  value=$2
  eval "current=\${$name-}"
  [ -n "$current" ] || eval "$name=\$value"
  export "$name"
}

set_default SFU_LOG_LEVEL 1
set_default SFU_MEDIA_PORT 7000
set_default SFU_SIGNALING_PORT 8000
set_default SFU_PUBLIC_HOST 127.0.0.1
set_default SFU_NATS_URL nats://nats:4222
set_default SFU_NATS_CLIENT_NAME sfu_nats_client
set_default SFU_NATS_HOOK_TOPIC mezon_sfu_hook_event
set_default SFU_PACKET_BUF_SIZE 2048
set_default SFU_PACKET_POOL_CAPACITY 262144
set_default SFU_PROVIDED_BUF_COUNT 8192
set_default SFU_PROVIDED_BUF_GROUP_ID 0
set_default SFU_WORKER_QUEUE_CAPACITY 16384
set_default SFU_FANOUT_RING_CAPACITY 4096
set_default SFU_FANOUT_JOB_POOL_CAPACITY 16384
set_default SFU_RELEASE_QUEUE_CAPACITY 8192
set_default SFU_VIDEO_POOL_PERCENT 85
set_default SFU_SOURCE_ADMISSION_BPS 240000
set_default SFU_SCREEN_PREFERRED_BPS 2000000
set_default SFU_SCREEN_MID_BPS 3000000
set_default SFU_SCREEN_CAP_BPS 3500000
set_default SFU_CAMERA_MID_BPS 720000
set_default SFU_CAMERA_CAP_BPS 1000000
set_default SFU_AF_XDP_INTERFACE eth0
set_default SFU_AF_XDP_QUEUES auto
set_default SFU_AF_XDP_FRAME_COUNT 16384
set_default SFU_AF_XDP_FRAME_SIZE 4096
set_default SFU_AF_XDP_MODE native

case "$SFU_MEDIA_PORT:$SFU_SIGNALING_PORT" in
  *[!0-9:]*|:*|*:) echo "Media and signaling ports must be numeric" >&2; exit 1 ;;
esac

mkdir -p "$(dirname "$SFU_CONFIG_FILE")"
cp "$SFU_CONFIG_TEMPLATE" "$SFU_CONFIG_FILE"
for name in \
  SFU_LOG_LEVEL SFU_MEDIA_PORT SFU_SIGNALING_PORT SFU_PUBLIC_HOST SFU_JWT_SECRET \
  SFU_NATS_URL SFU_NATS_CLIENT_NAME SFU_NATS_HOOK_TOPIC SFU_PACKET_BUF_SIZE \
  SFU_PACKET_POOL_CAPACITY SFU_PROVIDED_BUF_COUNT SFU_PROVIDED_BUF_GROUP_ID \
  SFU_WORKER_QUEUE_CAPACITY SFU_FANOUT_RING_CAPACITY SFU_FANOUT_JOB_POOL_CAPACITY \
  SFU_RELEASE_QUEUE_CAPACITY SFU_VIDEO_POOL_PERCENT SFU_SOURCE_ADMISSION_BPS \
  SFU_SCREEN_PREFERRED_BPS SFU_SCREEN_MID_BPS SFU_SCREEN_CAP_BPS \
  SFU_CAMERA_MID_BPS SFU_CAMERA_CAP_BPS SFU_AF_XDP_INTERFACE SFU_AF_XDP_QUEUES \
  SFU_AF_XDP_FRAME_COUNT SFU_AF_XDP_FRAME_SIZE SFU_AF_XDP_MODE
do
  eval "value=\${$name}"
  escaped=$(printf '%s' "$value" | sed 's/[\\&|]/\\&/g')
  sed -i "s|@$name@|$escaped|g" "$SFU_CONFIG_FILE"
done
chmod 0600 "$SFU_CONFIG_FILE"

if [ "$#" -eq 0 ] || [ "$1" = "mezon-sfu" ]; then
  [ "$#" -gt 0 ] && shift
  exec /usr/local/bin/mezon-sfu -c "$SFU_CONFIG_FILE" "$@"
fi
exec "$@"
