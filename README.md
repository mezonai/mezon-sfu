# mezon-sfu
A high-performance optimized for HD meetings and large-scale deployment.

The SFU core is built around a lock-free room execution model. Each room is processed by a dedicated isolated thread, ensuring deterministic packet routing without shared-state contention. Media packets are forwarded through a zero-copy pipeline powered by io_uring, enabling efficient fan-out to thousands of subscribers with minimal CPU overhead.

## features

* **Decoupled Architecture:** Run both signaling and media processes together or distribute them independently.


* **High-Performance Routing:** Low-latency packet pool design paired with multi-threaded worker pipelines.


* **Native Security:** Integrated DTLS handshake and secure SRTP packet protection.


* **WebRTC Test Client:** Includes a diagnostic HTML WebRTC client to verify connectivity.

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

## build liburing
```
git clone https://github.com/axboe/liburing.git
cd liburing

./configure
make -j$(nproc)
sudo make install
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

## build mezon sfu
```
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/usr/local -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
```

The compiled binary will be generated at `./build/mezon-sfu`.

## running the server

`mezon-sfu` can be configured using environment variables and runtime flags.

### crucial environment variable

* **`SFU_PUBLIC_HOST`**: This controls the IP address the SFU advertises to WebRTC clients.


* Set this to `127.0.0.1` for local testing.
* Set this to your server's **external public IP** (e.g., `27.72.29.150`) when deploying to a remote host.

#### running both signaling & media

Best for local development or single-server environments.

```bash
SFU_PUBLIC_HOST=127.0.0.1 ./build/mezon-sfu

```

## running the WebRTC test client

Modern WebRTC engines restrict local network discovery (mDNS protection) when running files directly off the hard drive. **You must host the test client over HTTP/HTTPS to test it successfully.**

1. Navigate to your client folder.
2. Spin up a local static server:
```bash
# Using Node.js
npx serve .

# OR using Python
python3 -m http.server 3000

```


3. Open your browser and navigate to `http://localhost:3000/webrtc_test_client.html`.
4. Input your WebSocket signaling URL (e.g., `ws://127.0.0.1:8080`).


5. Click **Connect** to start streaming!


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
    "args": ["--mode", "both"],
    "request": "launch",
    "env": {
      "SFU_PUBLIC_HOST": "127.0.0.1"
    }
  }
]

```

## topology this wires up
 
    [NIC] -> [dispatcher core: multishot recvmsg, SSRC/4-tuple hash]
                   |  SPSC ring per worker
                   v
    [worker core 0..N: pop inbox, forward via send_zc, reap completions]

 Core 0 is reserved for the dispatcher; cores 1..N-1 are workers. This is a placeholder policy -- production topology should account for NUMA (Non-Uniform Memory Access) nodes and leave a core free for the kernel's network softirq handling, but the mapping itself is what matters for now: one dispatcher, N workers, no shared mutable state between them beyond the SPSC rings.

*Here is how those 11 steps map directly to the current codebase's functions and execution units:*

| Step | Architecture Layer | Execution Unit | Codebase Mapping |
| --- | --- | --- | --- |
| **1** | UDP Ingress | Dispatcher thread | `scheduler_thread_main()` reaps the multishot receive ring; `handle_recv_cqe()` creates the packet; `on_recv()` hashes the remote address and pushes it to a worker inbox (`src/runtime/scheduler.c`, `src/net/io_uring.c`). |
| **2** | ICE / STUN | Selected worker | `worker_thread_main()` calls `sfu_dispatch_packet()`, which detects STUN and invokes `handle_stun()` → `sfu_stun_extract_client_ufrag()` / `sfu_stun_handle_binding_request()` (`src/runtime/worker.c`, `src/pipeline/dispatch.c`, `src/transport/stun/stun.c`). |
| **3** | DTLS | Session-owner worker | `sfu_dispatch_packet()` invokes `handle_dtls()` → `sfu_dtls_conn_feed()` / `sfu_dtls_conn_drain_output()`; once established, `sfu_srtp_ctx_init_from_dtls()` initializes SRTP (`src/pipeline/dispatch.c`, `src/transport/dtls/dtls.c`). |
| **4** | SRTP Decryption | Worker | `sfu_ingress_process()` classifies RTP/RTCP and calls `sfu_srtp_unprotect_rtp()` or `sfu_srtp_unprotect_rtcp()` (`src/pipeline/ingress.c`, `src/transport/srtp/srtp.c`). |
| **5** | RTP Parsing | Publisher worker | `sfu_ingress_process()` calls `sfu_rtp_packet_parse()` to decode the RTP header, extensions, payload, and padding (`src/pipeline/ingress.c`, `src/rtp/rtp_packet.c`). |
| **6** | SVC Parsing | Publisher worker | `extract_svc_metadata()` calls `sfu_svc_parse_descriptor()`, which dispatches VP9 packets to `sfu_parse_vp9_descriptor()` (`src/pipeline/ingress.c`, `src/media/svc/svc_descriptor.c`, `src/media/svc/svc_parser.c`). |
| **7** | Congestion Control / TWCC | Publisher and subscriber workers | **Publisher uplink:** ingress records transport sequence arrivals with `sfu_twcc_recv_tracker_record()`; the worker periodically runs `sfu_session_maybe_send_twcc_feedback()` → `sfu_twcc_feedback_build()`. **Subscriber downlink:** egress records sent TWCC sequences; returned RTCP is parsed by `handle_twcc_member()` / `sfu_twcc_parser_next()` and fed to `gcc_bwe_process_twcc_packet()` (`src/pipeline/ingress.c`, `src/pipeline/egress.c`, `src/peer/session.c`, `src/congestion/`). |
| **8** | Layer Scheduler / Selector | Subscriber worker sets targets; publisher worker gates frames | `sfu_svc_update_layers()` updates `sfu_subscriber_scheduler_set_bitrate()` from GCC estimates. During routing, `sfu_scheduler_prepare_packet()` selects and classifies each packet; `sfu_scheduler_commit_packet()` or `sfu_scheduler_reject_packet()` updates dependency and layer-transition state after egress admission (`src/runtime/scheduler.c`, `src/pipeline/router.c`). |
| **9** | Routing / Fanout | Publisher worker, then subscriber-owner worker when remote | `sfu_router_forward()` walks the receiver snapshot and calls `sfu_egress_process()`. Local subscribers continue directly; remote subscribers cross the worker mesh through `sfu_fanout_mesh_enqueue_forward()` and resume in `sfu_worker_handle_fanout_job()` (`src/pipeline/router.c`, `src/pipeline/egress.c`, `src/runtime/fanout.c`, `src/runtime/fanout_job.c`). |
| **10** | Packet Rewrite / Outbound SRTP | Subscriber-owner worker | `sfu_egress_process_local()` remaps payload type, applies pacing, caches plaintext for RTX, writes the subscriber TWCC extension, records send history, and calls `sfu_srtp_protect_rtp()` (`src/pipeline/egress.c`, `src/rtp/rtp_ext.c`, `src/transport/srtp/srtp.c`). |
| **11** | UDP Egress | Per-worker send ring | `sfu_egress_process_local()` queues the protected packet with `sfu_ring_queue_send_zc()`; the worker submits and reaps its `io_uring` send ring through `sfu_ring_submit()` / `sfu_ring_reap()` (`src/pipeline/egress.c`, `src/net/io_uring.c`, `src/runtime/worker.c`). |

The dispatcher owns UDP receive completions, packet-to-worker hashing, and worker-inbox delivery. STUN, DTLS, SRTP, RTP/SVC parsing, congestion control, scheduling, routing, protection, and send-ring processing execute on workers. The current media path is `sfu_dispatch_packet()` → `sfu_ingress_process()` → `sfu_router_forward()` → `sfu_egress_process()`. Fanout crosses the worker-to-worker SPSC mesh only when a subscriber is owned by another worker. Publisher-uplink TWCC is generated by the SFU from received publisher RTP, while subscriber-downlink TWCC is parsed and fed into GCC for pacing and layer selection. Ordinary forwarded RTP currently remaps the payload type and transport-wide sequence extension; it does not normalize the RTP SSRC, sequence number, or timestamp.


