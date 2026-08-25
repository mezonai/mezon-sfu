# mezon-sfu
A high-performance optimized for HD meetings and large-scale deployment.

The SFU core is built around a lock-free room execution model. Each room is processed by a dedicated isolated thread, ensuring deterministic packet routing without shared-state contention. Media packets are forwarded through a zero-copy pipeline powered AF_XDP (io_uring fallback), enabling efficient fan-out to thousands of subscribers with minimal CPU overhead.

## features

* **WebRTC Compliance:** Compatible with both standard WebRTC clients and [libmezia](https://github.com/mezonai/libmezia) - lightweight and ultra low latentcy audio/video for native platform
* **High-Performance Routing:** Low-latency packet pool design paired with multi-threaded worker pipelines.
* **Native Security:** Integrated DTLS handshake and secure SRTP packet protection.
* **WebRTC Test Client:** Includes a diagnostic HTML WebRTC client to verify connectivity.
* **Simple, Standalone Setup:** No external dependencies required to get running.
* Lock-free fanout with hazard pointers
* Full SVC temporal/spatial layer support
* Modern zero-copy network stack with AF_XDP backend and io_uring fallback
* Standards-compliant GCC congestion control
* **Push To Talk** PTT native support

## AF_XDP runtime configuration

Configure the interface and hardware queue in `config.ini`:

```ini
[af_xdp]
interface = eth0
queues = auto
frame_count = 16384
frame_size = 4096
mode = native  # native, skb, or auto
```

The AF_XDP binary requires permission to load BPF programs and administer the selected interface (normally root or appropriate `CAP_BPF`/`CAP_NET_ADMIN` capabilities). It supports IPv4 UDP and does not reassemble fragments. The configured frame pool is split evenly into power-of-two RX and TX rings. Configure RSS/flow steering so the media UDP port reaches the selected queue; matching media packets on another queue are dropped rather than passed to an undrained UDP socket. On a cold neighbour entry, TX temporarily falls back to the bound nonblocking UDP socket so the kernel can resolve ARP, then direct AF_XDP transmission resumes. The loader refuses to replace an existing XDP program and cleanup detaches only the program attached by this process.

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

These are smoke-run results from one development machine, not guaranteed performance targets. Use a non-quick run with CPU affinity and frequency scaling controlled for comparative measurements.

## build prerequisites

Before building, ensure you have the following installed on your system:

* **CMake** (3.15 or higher)
* **C Compiler** (GCC or Clang)
* **BoringSSL & libsrtp2** development libraries

To compile the C backend binary, run the following commands from the root directory:

## install libuv
`sudo apt install libuv1-dev`

## build mimalloc
```
git clone https://github.com/microsoft/mimalloc.git
cd mimalloc
mkdir -p build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

## default AF_XDP build

The default build uses AF_XDP (`USE_AF_XDP=ON`). Install clang, libxdp, libbpf, and matching Linux headers:

```sh
sudo apt install clang libxdp-dev libbpf-dev linux-headers-$(uname -r)
cmake -S . -B build
cmake --build build -j$(nproc)
```

## io_uring fallback

To build with io_uring instead, install liburing and disable AF_XDP explicitly:

```sh
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make -j$(nproc)
sudo make install
cd -
cmake -S . -B build-uring -DUSE_AF_XDP=OFF
cmake --build build-uring -j$(nproc)
```

## build boringSSL
```
git clone https://boringssl.googlesource.com/boringssl
cd boringssl
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
sudo ldconfig
```
```
cd boringssl
sudo mkdir /usr/local/include/boringssl
sudo cp -rf include/* /usr/local/include/boringssl/
sudo cp -rf build/bssl /usr/local/bin/
sudo mkdir /usr/local/lib/boringssl
sudo cp -rf build/lib* /usr/local/lib/boringssl/
```

## build  libsrtp
```
git clone https://github.com/cisco/libsrtp.git
git checkout 24b3bf8

cd libsrtp
./configure --enable-openssl \
  crypto_CFLAGS="-I/usr/local/include/boringssl/" \
  crypto_LIBS="-L/usr/local/lib/boringssl/ -lcrypto -lstdc++"
make
```

## build nats client
```
git clone https://github.com/nats-io/nats.c.git
cd nats.c
mkdir build && cd build
cmake .. -DNATS_BUILD_STREAMING=OFF -DNATS_BUILD_EXAMPLES=OFF
make -j$(nproc)
sudo make install
sudo ldconfig
```

## build libxdp-dev
```
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

## build mezon sfu
```
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/usr/local -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSFU_DIAG_LOG=ON
make -j$(nproc)
ctest --output-on-failure
```

The compiled binary will be generated at `./build/mezon-sfu`.

## running the server

`mezon-sfu` can be configured using environment variables and runtime flags.

### crucial environment variable

* **`config.ini`**: This is default config to run in local.


* Set publish_host to `127.0.0.1` for local testing.
* Set this to your server's **external public IP** (e.g., `203.0.113.88`) when deploying to a remote host.

#### running sfu

Best for local development or single-server environments.

```bash
./build/mezon-sfu -c ../config.ini

```

## running the WebRTC test client

Modern WebRTC engines restrict local network discovery (mDNS protection) when running files directly off the hard drive. **You must host the test client over HTTP/HTTPS to test it successfully.**

1. Open your browser and navigate to `http://localhost:3000/webrtc_test_client.html`.
2. Input your WebSocket signaling URL (e.g., `ws://127.0.0.1:8080`).
3. Input jwt_secret (in config.ini). This is for testing purpose
4. Click **Connect** to start streaming!


## editor & debugger integration (Zed / VS Code)

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

### benchmark

Run from a clean release build with `./build/benchmark/bench_sfu_core` (1,000,000 iterations, 1,200-byte synthetic RTP packets, 4 workers, fan-out 3). Machines i5-10400, 12 logical cores.

| Benchmark | What it measures | Wall time | Throughput |
| --- | --- | --- | --- |
| `rtp_parse` | RTP parsing | ~27 ns/packet | **~37.1 M packets/s** |
| `packet_pool` | Packet-pool alloc/retain/release cycle (media-path prerequisite) | ~273 ns/op | **~3.7 M ops/s** |
| `fanout_mesh` | SPSC worker-mesh enqueue + drain (job = 1 packet × 3 targets) | ~122 ns/job | **~8.2 M jobs/s** |
| `media_fanout` | End-to-end fanout | ~853 ns/target | **~1.17 M targets/s** |
| `srtp_decrypt` | SRTP unprotect (AES-128-GCM) on a 1200-byte packet | ~440 ns/packet | **~2.3 M packets/s** |
| `srtp_encrypt` | SRTP protect (AES-128-GCM) on a 1200-byte packet | ~420 ns/packet | **~2.4 M packets/s** |

SRTP is measured on a single stream with monotonically increasing transport sequence (so replay/ROC state advances like real media). The remaining pipeline stages (UDP ingress, STUN, DTLS, SVC parse, congestion control, layer scheduler) still have no dedicated harness.

The dispatcher owns UDP receive completions, packet-to-worker hashing, and worker-inbox delivery. STUN, DTLS, SRTP, RTP/SVC parsing, congestion control, scheduling, routing, protection, and send-ring processing execute on workers. The current media path is `sfu_dispatch_packet()` → `sfu_ingress_process()` → `sfu_router_forward()` → `sfu_egress_process()`. Fanout crosses the worker-to-worker SPSC mesh only when a subscriber is owned by another worker. Publisher-uplink TWCC is generated by the SFU from received publisher RTP, while subscriber-downlink TWCC is parsed and fed into GCC for pacing and layer selection. Ordinary forwarded RTP currently remaps the payload type and transport-wide sequence extension; it does not normalize the RTP SSRC, sequence number, or timestamp.
