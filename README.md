# mezon-sfu

A high-performance SFU (Selective Forwarding Unit) optimized for HD meetings and large-scale deployment.

The SFU core is built around a lock-free room execution model. Each room is processed by a dedicated isolated thread, ensuring deterministic packet routing without shared-state contention. Media packets are forwarded through a zero-copy pipeline powered by AF_XDP (io_uring fallback), enabling efficient fan-out to thousands of subscribers with minimal CPU overhead.

AF_XDP is the default network backend; io_uring is available as a fallback for environments where AF_XDP isn't supported.

## Why mezon-sfu?

Existing SFUs options are capable, but heavy — large dependency trees, runtime overhead, and general-purpose designs that aren't tuned for any one use case. mezon-sfu exists because we needed something different:

* **Standalone, dependency-light:** No external media-server runtime to operate — build it, run the binary.
* **Deeply optimized for meetings:** Built from scratch around the meeting workload specifically (HD video, screen share, large rooms) rather than adapted from a general-purpose media server.
* **Lightweight and easy to integrate:** Small enough to embed into the existing Mezon ecosystem without dragging in a heavyweight stack.

## Features

* **WebRTC Compliance:** Compatible with both standard WebRTC clients and [libmezia](https://github.com/mezonai/libmezia) — a lightweight, ultra-low-latency audio/video library for native platforms
* **High-Performance Routing:** Low-latency packet pool design paired with multi-threaded worker pipelines
* **Native Security:** Integrated DTLS handshake and secure SRTP packet protection
* **WebRTC Test Client:** Includes a diagnostic HTML WebRTC client to verify connectivity
* **Simple, Standalone Setup:** No external dependencies required to get running
* Lock-free fanout with hazard pointers
* Full SVC temporal/spatial layer support
* Modern zero-copy network stack with AF_XDP backend and io_uring fallback
* Standards-compliant GCC congestion control
* **Push To Talk (PTT):** Native support

## Build prerequisites

Before building, ensure you have the following installed:

* **CMake** (3.15 or higher)
* **C Compiler** (GCC or Clang)
* **BoringSSL & libsrtp2** development libraries

## Building from source

Run these steps in order from the repository root.

### 1. Install system packages

```sh
sudo apt install libuv1-dev
```

### 2. Build mimalloc

```sh
git clone https://github.com/microsoft/mimalloc.git
cd mimalloc
mkdir -p build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

### 3. Build BoringSSL

```sh
git clone https://boringssl.googlesource.com/boringssl
cd boringssl
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

```sh
cd boringssl
sudo mkdir /usr/local/include/boringssl
sudo cp -rf include/* /usr/local/include/boringssl/
sudo cp -rf build/bssl /usr/local/bin/
sudo mkdir /usr/local/lib/boringssl
sudo cp -rf build/lib* /usr/local/lib/boringssl/
```

### 4. Build libsrtp

```sh
git clone https://github.com/cisco/libsrtp.git
git checkout 24b3bf8

cd libsrtp
./configure --enable-openssl \
  crypto_CFLAGS="-I/usr/local/include/boringssl/" \
  crypto_LIBS="-L/usr/local/lib/boringssl/ -lcrypto -lstdc++"
make
```

### 5. Build the NATS client

```sh
git clone https://github.com/nats-io/nats.c.git
cd nats.c
mkdir build && cd build
cmake .. -DNATS_BUILD_STREAMING=OFF -DNATS_BUILD_EXAMPLES=OFF
make -j$(nproc)
sudo make install
sudo ldconfig
```

### 6. Choose a network backend

Pick **one** of the two options below.

#### Option A — AF_XDP (default)

Install clang, libxdp, libbpf, and matching Linux headers:

```sh
sudo apt install clang libxdp-dev libbpf-dev linux-headers-$(uname -r)
```

To build libxdp-dev from source instead:

```sh
git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git
cd xdp-tools

# Fetch all tags and update submodules
git fetch --tags
git submodule update --init --recursive

# Find the latest release tag
git tag -l "v*" | tail -n 5

# Checkout the latest stable release (e.g., v1.4.2)
git checkout v1.4.2

# Ensure submodules match the selected release tag
git submodule update --init --recursive

# Clean previous build artifacts
make clean

# Run configure script to generate build configuration
./configure

# Build and install
make
sudo make install

# libbpf first (libxdp depends on it)
sudo make -C lib/libbpf/src install PREFIX=/usr/local LIBDIR=/usr/local/lib

# then libxdp
sudo make -C lib/libxdp install PREFIX=/usr/local LIBDIR=/usr/local/lib
```

Then configure and build mezon-sfu:

```sh
cmake -S . -B build
cmake --build build -j$(nproc)
```

#### Option B — io_uring fallback

Install liburing and disable AF_XDP explicitly:

```sh
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make -j$(nproc)
sudo make install
cd -
cmake -S . -B build-uring -DSFU_NET_BACKEND="io_uring"
cmake --build build-uring -j$(nproc)
```

### 7. Build mezon-sfu

```sh
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/usr/local -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSFU_DIAG_LOG=ON
make -j$(nproc)
ctest --output-on-failure
```

The compiled binary will be generated at `./build/mezon-sfu`.

## Docker deployment (`io_uring`)

The production container uses the `io_uring` backend. It does not require host networking, eBPF access, or a privileged container. The host must provide a modern Linux kernel and adequate `nofile` and `memlock` limits.

Create the runtime environment file and replace both placeholders:

```sh
cp .env.example .env
# SFU_PUBLIC_HOST must be the public IP or DNS name clients can reach.
# SFU_JWT_SECRET must be a long random secret and must match the token issuer.
```

Build and start NATS and the SFU:

```sh
docker compose build --pull
docker compose up -d
docker compose ps
docker compose logs -f sfu
```

The container exposes UDP `7000` for WebRTC media and TCP `8000` for signaling by default. Open both ports in the host and cloud firewalls. Docker/Kubernetes secrets can be mounted and selected with `SFU_JWT_SECRET_FILE`; this takes precedence over `SFU_JWT_SECRET`.

The health check performs a WebSocket upgrade against the signaling listener. Stop gracefully with:

```sh
docker compose down --timeout 30
```

AF_XDP remains available for native-host deployments. It is intentionally not used by this container because it requires host networking, the physical NIC and queue configuration, BPF access, and elevated capabilities.

## WebRTC load test

`tools/loadtest` is a separate Go/Pion client that exercises JWT authentication, WebSocket signaling, ICE, DTLS-SRTP, renegotiation, synthetic Opus/VP8 publishing, and subscriber RTP reception.

### Run against an existing SFU

Requirements: Go 1.24+, an accessible signaling URL, open UDP media routing, and the same JWT secret configured on the SFU.

```sh
cd tools/loadtest
go test ./...
go run . \
  -url ws://127.0.0.1:8000/ws \
  -jwt-secret 'replace-with-the-sfu-jwt-secret' \
  -rooms 1 \
  -peers 3 \
  -speakers 1 \
  -duration 30s \
  -ramp-duration 5s \
  -bitrate 240000 \
  -json-file ../../loadtest-results/smoke-report.json
```

For the production topology, run:

```sh
cd tools/loadtest
mkdir -p ../../loadtest-results
go run . \
  -url ws://SFU_HOST:8000/ws \
  -jwt-secret 'replace-with-the-sfu-jwt-secret' \
  -rooms 30 \
  -peers 10 \
  -speakers 2 \
  -duration 60m \
  -ramp-duration 30s \
  -bitrate 240000 \
  -min-success-rate 100 \
  -max-packet-loss 1 \
  -min-rx-packets 1 \
  -json-file ../../loadtest-results/capacity-report.json
```

The process exits nonzero when an enabled threshold fails. Use `go run . -help` for all topology, media, threshold, and reporting options.

### Run with Docker Compose

Copy the environment example, set a strong JWT secret, and create the report directory:

```sh
cp .env.example .env
# Edit .env: set SFU_PUBLIC_HOST and SFU_JWT_SECRET.
mkdir -p loadtest-results
```

Run a small functional smoke test locally:

```sh
mkdir -p loadtest-results
LOADTEST_ROOMS=1 \
LOADTEST_PEERS_PER_ROOM=3 \
LOADTEST_SPEAKERS_PER_ROOM=1 \
LOADTEST_DURATION=30s \
docker compose -f compose.yaml -f compose.loadtest.yaml up \
  --build --abort-on-container-exit --exit-code-from loadtest
```

Run the target topology of 300 participants across 30 rooms, with two speakers per room:

```sh
mkdir -p loadtest-results
docker compose -f compose.yaml -f compose.loadtest.yaml up \
  --build --abort-on-container-exit --exit-code-from loadtest
```

The report is written to `loadtest-results/report.json`. Defaults require all peers to connect, subscriber media reception, and no more than 1% measured packet loss. Override `LOADTEST_DURATION`, `LOADTEST_RAMP_DURATION`, `LOADTEST_VIDEO_BITRATE`, `LOADTEST_MIN_SUCCESS_RATE`, `LOADTEST_MAX_PACKET_LOSS`, and `LOADTEST_MIN_RX_PACKETS` when needed.

Stop and remove the test stack after completion:

```sh
docker compose -f compose.yaml -f compose.loadtest.yaml down --timeout 30
```

A same-host Compose run is a functional regression test, not authoritative capacity evidence. For production qualification, run the load generator from separate machines for at least 60 minutes and record SFU CPU, RSS, NIC throughput/drops, container restarts, and packet-pool or queue exhaustion logs. Approve 300-user capacity only with at least 30% CPU and network headroom.

## Running the server

`mezon-sfu` can be configured using environment variables and runtime flags.

**Config file:** `config.ini` is the default configuration used for local runs.

Edit `publish_host` in `config.ini`:
* Set it to `127.0.0.1` for local testing.
* Set it to your server's **external public IP** (e.g., `203.0.113.88`) when deploying to a remote host.

### Starting the SFU

Best for local development or single-server environments.

```bash
./build/mezon-sfu -c ../config.ini
```

## Running the WebRTC test client

Modern WebRTC engines restrict local network discovery (mDNS protection) when running files directly off the hard drive. **You must host the test client over HTTP/HTTPS to test it successfully.**

1. Open your browser and navigate to `http://localhost:3000/webrtc_test_client.html`.
2. Input your WebSocket signaling URL (e.g., `ws://127.0.0.1:8080`).
3. Input `jwt_secret` (from `config.ini`). This is for testing purposes only.
4. Click **Connect** to start streaming!

See [`examples/webrtc_test_client.html`](examples/webrtc_test_client.html) and its [README](examples/README.md) for the full client — camera + screen-share publishing, speaker/audience join modes, and a field-by-field walkthrough of the join form.

## Agent integration

[mezon-call-translation](https://github.com/mezonai/mezon-call-translation) is a companion service that adds real-time speech-to-text and translation to calls running on the SFU, via a general-purpose Voice AI Agent (not tied to any specific WebRTC provider).

**What it does:**
* Joins a call as a Voice AI Agent and pulls audio through voice activity detection (VAD) to filter out silence before transcription.
* Transcribes speech with the [Vosk](https://alphacephei.com/vosk/) offline STT engine and can synthesize translated audio back with Kokoro TTS.
* Runs behind a FastAPI server that fans work out to multiple STT workers, so one deployment can serve many simultaneous calls.
* Scales horizontally — an Nginx load balancer sits in front of multiple server instances, each with its own worker pool, so agent capacity can grow independently of the SFU.

**How it connects:**
* The agent is dispatched into a call through the server's `POST /agent/join` REST endpoint.
* Audio and results flow over a WebSocket API (`ws://<host>:8000/ws/vosk/`), which accepts per-client parameters (`client_id`, `session_id`, `language`, and whether transcript and/or translation output is wanted) and returns JSON transcript/translation events.
* Health is exposed via `/health` and `/health/simple`, which the load balancer polls to route around unhealthy instances.

**Setup:** see the [mezon-call-translation Quick Start](https://github.com/mezonai/mezon-call-translation#-quick-start) and [Setup Guide](https://github.com/mezonai/mezon-call-translation/blob/main/docs/setup/SETUP-GUIDE.md) for environment configuration and the Vosk/Kokoro models downloaded via the provided scripts.

> Note: some of mezon-call-translation's published docs still describe the agent as LiveKit-specific — treat that framing as outdated; the agent itself is provider-agnostic.

## Editor & debugger integration (Zed / VS Code)

If you use the **Zed** editor, you can run and debug your builds directly with CodeLLDB by configuring your workspace tasks:

```json
[
  {
    "label": "Debug mezon-sfu (CMake Both)",
    "adapter": "CodeLLDB",
    "build": {
      "command": "cmake",
      "args": ["--build", "build"],
      "cwd": "$ZED_WORKTREE_ROOT"
    },
    "program": "$ZED_WORKTREE_ROOT/build/mezon-sfu",
    "args": ["-c", "config.ini"],
    "request": "launch"
  }
]
```

## Architecture

The dispatcher owns UDP receive completions, packet-to-worker hashing, and worker-inbox delivery. STUN, DTLS, SRTP, RTP/SVC parsing, congestion control, scheduling, routing, protection, and send-ring processing execute on workers.

The current media path:

```
sfu_dispatch_packet() → sfu_ingress_process() → sfu_router_forward() → sfu_egress_process()
```

Fanout crosses the worker-to-worker SPSC mesh only when a subscriber is owned by another worker. Publisher-uplink TWCC is generated by the SFU from received publisher RTP, while subscriber-downlink TWCC is parsed and fed into GCC for pacing and layer selection. Ordinary forwarded RTP currently remaps the payload type and transport-wide sequence extension; it does not normalize the RTP SSRC, sequence number, or timestamp.

## AF_XDP reference

### Runtime configuration

Configure the interface and hardware queue in `config.ini`:

```ini
[af_xdp]
interface = eth0
queues = auto
frame_count = 16384
frame_size = 4096
mode = native  # native, skb, or auto
```

The AF_XDP binary requires permission to load BPF programs and administer the selected interface (normally root or appropriate `CAP_BPF`/`CAP_NET_ADMIN` capabilities). It supports IPv4 UDP and does not reassemble fragments. The configured frame pool is split evenly into power-of-two RX and TX rings. Configure RSS/flow steering so the media UDP port reaches the selected queue; matching media packets on another queue are dropped rather than passed to an undrained UDP socket. On a cold neighbour entry, TX temporarily falls back to the bound nonblocking UDP socket so the kernel can resolve ARP, then direct AF_XDP transmission resumes. The loader refuses to replace an existing XDP program, and cleanup detaches only the program attached by this process.

### Tests

The AF_XDP frame and software-ring unit tests are CPU-only and do not require root or a network interface:

```sh
ctest --test-dir build --output-on-failure -R 'af_xdp_(frame|ring)'
```

A privileged veth/network-namespace smoke test is available but is not registered by default:

```sh
cmake -S . -B build-af-xdp-integration -DSFU_ENABLE_PRIVILEGED_TESTS=ON
cmake --build build-af-xdp-integration -j$(nproc)
sudo ctest --test-dir build-af-xdp-integration --output-on-failure -R af_xdp_veth_integration
```

The script uses a temporary single-queue veth pair in `skb` mode and cleans up its namespace, links, process, and XDP attachment on exit.

## Benchmarks

> Results below are smoke-run figures from a single development machine, not guaranteed performance targets. For comparative measurements, use a non-quick run with CPU affinity and frequency scaling controlled.

### Network layer (AF_XDP frame parsing)

Run the focused frame parser/builder benchmark with:

```sh
build-af-xdp/benchmark/bench_af_xdp_frame all --quick
build-af-xdp/benchmark/bench_af_xdp_frame parse_ipv4_udp --packet-size 1200
build-af-xdp/benchmark/bench_af_xdp_frame build_ipv4_udp --packet-size 1200
```

Example results for a 1200-byte payload (`--quick`, 1,000 measured iterations):

| Benchmark | Time per operation | Operations per second |
|---|---:|---:|
| IPv4/UDP frame parsing | 39.39 ns | 25.39 M |
| VLAN IPv4/UDP frame parsing | 39.12 ns | 25.56 M |
| IPv4/UDP frame construction | 179.17 ns | 5.58 M |
| IPv4 header checksum | 34.90 ns | 28.65 M |

### Media pipeline (`bench_sfu_core`)

Run from a clean release build with `./build/benchmark/bench_sfu_core` (1,000,000 iterations, 1,200-byte synthetic RTP packets, 4 workers, fan-out 3). Machine: i5-10400, 12 logical cores.

| Benchmark | What it measures | Wall time | Throughput |
| --- | --- | --- | --- |
| `rtp_parse` | RTP parsing | ~27 ns/packet | **~37.1 M packets/s** |
| `packet_pool` | Packet-pool alloc/retain/release cycle (media-path prerequisite) | ~273 ns/op | **~3.7 M ops/s** |
| `fanout_mesh` | SPSC worker-mesh enqueue + drain (job = 1 packet × 3 targets) | ~122 ns/job | **~8.2 M jobs/s** |
| `media_fanout` | End-to-end fanout | ~853 ns/target | **~1.17 M targets/s** |
| `srtp_decrypt` | SRTP unprotect (AES-128-GCM) on a 1200-byte packet | ~440 ns/packet | **~2.3 M packets/s** |
| `srtp_encrypt` | SRTP protect (AES-128-GCM) on a 1200-byte packet | ~420 ns/packet | **~2.4 M packets/s** |

SRTP is measured on a single stream with monotonically increasing transport sequence (so replay/ROC state advances like real media). The remaining pipeline stages (UDP ingress, STUN, DTLS, SVC parse, congestion control, layer scheduler) still have no dedicated harness.
