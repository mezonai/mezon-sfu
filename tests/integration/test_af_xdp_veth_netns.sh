#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "SKIP: AF_XDP integration test requires root" >&2
  exit 77
fi

SFU_BIN=${1:-}
if [[ -z ${SFU_BIN} || ! -x ${SFU_BIN} ]]; then
  echo "usage: $0 /path/to/mezon-sfu" >&2
  exit 2
fi

NS=sfu-xdp-test
HOST_DEV=sfu-xdp-host
PEER_DEV=sfu-xdp-peer
TMP_DIR=$(mktemp -d)
SFU_PID=

cleanup() {
  if [[ -n ${SFU_PID} ]] && kill -0 "${SFU_PID}" 2>/dev/null; then
    kill -TERM "${SFU_PID}" 2>/dev/null || true
    for _ in $(seq 1 50); do
      kill -0 "${SFU_PID}" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL "${SFU_PID}" 2>/dev/null || true
    wait "${SFU_PID}" 2>/dev/null || true
  fi
  ip netns del "${NS}" 2>/dev/null || true
  ip link del "${HOST_DEV}" 2>/dev/null || true
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT INT TERM

ip netns del "${NS}" 2>/dev/null || true
ip link del "${HOST_DEV}" 2>/dev/null || true
ip netns add "${NS}"
ip link add "${HOST_DEV}" type veth peer name "${PEER_DEV}"
ip link set "${PEER_DEV}" netns "${NS}"
ip addr add 192.0.2.1/24 dev "${HOST_DEV}"
ip link set "${HOST_DEV}" up
ip netns exec "${NS}" ip addr add 192.0.2.2/24 dev "${PEER_DEV}"
ip netns exec "${NS}" ip link set lo up
ip netns exec "${NS}" ip link set "${PEER_DEV}" up

cat >"${TMP_DIR}/config.ini" <<'CONFIG'
[server]
log_level = 1
media_port = 7000
signaling_port = 18000
public_host = 192.0.2.1
jwt_secret = integration-test

[nats]
nats_url = nats://127.0.0.1:4222
nats_client_name = sfu_xdp_integration
nats_hook_topic = sfu_xdp_integration

[buffers]
packet_buf_size = 2048
packet_pool_capacity = 16384
provided_buf_count = 1024
provided_buf_group_id = 0

[queues]
worker_queue_capacity = 1024
fanout_ring_capacity = 1024
fanout_job_pool_capacity = 1024
release_queue_capacity = 1024

[af_xdp]
interface = sfu-xdp-host
queue_id = 0
frame_count = 1024
frame_size = 4096
mode = skb
CONFIG

"${SFU_BIN}" -c "${TMP_DIR}/config.ini" >"${TMP_DIR}/sfu.log" 2>&1 &
SFU_PID=$!
sleep 2
if ! kill -0 "${SFU_PID}" 2>/dev/null; then
  echo "AF_XDP SFU failed to start:" >&2
  tr '\n' ' ' <"${TMP_DIR}/sfu.log" >&2
  echo >&2
  exit 1
fi

if ! ip -details link show dev "${HOST_DEV}" | grep -q 'prog/xdp'; then
  echo "XDP program is not attached to ${HOST_DEV}" >&2
  exit 1
fi

ip netns exec "${NS}" python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"af-xdp-integration", ("192.0.2.1", 7000))
PY

kill -TERM "${SFU_PID}"
for _ in $(seq 1 100); do
  kill -0 "${SFU_PID}" 2>/dev/null || break
  sleep 0.1
done
if kill -0 "${SFU_PID}" 2>/dev/null; then
  echo "SFU did not shut down cleanly" >&2
  exit 1
fi
wait "${SFU_PID}" || true
SFU_PID=

if ip -details link show dev "${HOST_DEV}" | grep -q 'prog/xdp'; then
  echo "owned XDP program remained attached after shutdown" >&2
  exit 1
fi

echo "AF_XDP veth integration smoke test: OK"
