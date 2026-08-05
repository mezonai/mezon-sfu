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

*Here is how those 11 steps map directly to your codebase's concrete functions and execution units:*

| Step | Architecture Layer | Code Base Mapping / Function |
| --- | --- | --- |
| **1** | UDP Ingress | **`io_uring`** (or socket `recvfrom`/`recvmmsg` ring) |
| **2** | ICE Layer | **`handle_stun()`** |
| **3** | DTLS Layer | **`handle_dtls()`** |
| **4** | SRTP Decryption | **`sfu_srtp_unprotect()`** *(Note: `sfu_srtp_ctx_init_from_dtls` extracts keying material once during handshake; `unprotect` decrypts every packet)* |
| **5** | RTP Parser | **`sfu_room_forward_packet()`** $\rightarrow$ `sfu_rtp_packet_parse()` |
| **6** | SVC Parser | **`sfu_room_forward_packet()`** $\rightarrow$ `sfu_parse_vp9_descriptor()` |
| **7** | Congestion Control | **`sfu_room_forward_packet()`** $\rightarrow$ `sfu_twcc_parser_next()` / `gcc_bwe_process_twcc_packet()` |
| **8** | Layer Scheduler | **`sfu_room_forward_packet()`** $\rightarrow$ Layer gating / `needs_keyframe` checks |
| **9** | Packet Router | **Fanout job** / ring buffer cross-thread enqueue to subscriber queues |
| **10** | Outbound SRTP | Header rewriting (SSRC, sequence/timestamp normalization) $\rightarrow$ **`sfu_srtp_protect()`** |
| **11** | UDP Egress | **`io_uring`** (or `sendmmsg`/`sendto` write ring) |

Steps **5, 6, 7, and 8** are indeed encapsulated inside **`sfu_room_forward_packet()`** executing on the worker thread, while Step 9 hands off the processed packet to the subscriber egress pipeline where Step 10 (`protect`) and Step 11 (`io_uring` write) take over.


## diagram
<img width="1440" height="840" alt="image" src="https://github.com/user-attachments/assets/f3772f59-b4b9-4086-9a6e-a80346da1bef" />

<img width="1440" height="720" alt="image" src="https://github.com/user-attachments/assets/3c4832a0-ff90-4357-a3b9-5b329fd6ee0d" />

<img width="2720" height="1320" alt="mezon_sfu_core_architecture" src="https://github.com/user-attachments/assets/67892729-529a-4bcf-9490-1598d6526a5a" />
