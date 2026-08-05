## Phase 1: Transport Layer (Current)

* ✅ ICE
* ✅ DTLS
* ✅ SRTP
* ✅ RTP/RTCP
* ✅ Packet forwarding
* ✅ SDP negotiation
* ✅ Payload type translation
* ✅ Per-peer transceiver model


## Phase 2: SVC Support (Highest Priority)

Without SVC, the SFU can only:

* forward a single stream
* or simulcast (3 separate encodes)

GCC's efficiency comes largely from SVC.

### Receive

Parse:

* VP9 SVC
* AV1 SVC
* VP8 (optional)

Understand:

* Spatial layer
* Temporal layer
* Dependency chain
* Frame type
* Decode target

Maintain per publisher:

```c
publisher
    layer0
    layer1
    layer2
```

### Forward

Each subscriber chooses

```
Publisher A
    720p T2

Subscriber 1
    180p T1

Subscriber 2
    360p T2

Subscriber 3
    720p T2
```

No transcoding.

Just packet filtering.

This is the biggest feature after basic forwarding.

## Phase 3: Congestion Control (TWCC)

Need:

* TWCC parser
* TWCC feedback generator
* RTT estimation
* Loss estimation
* Bandwidth estimation

Eventually implement:

```
Google GCC
```

Components:

* Arrival-time filter
* Trendline estimator
* Overuse detector
* AIMD controller

This decides

```
Subscriber bandwidth

↓

desired layer

↓

switch SVC layer
```

## Phase 4: Layer Scheduler

This becomes the SFU brain.

Instead of

```
forward packet
```

it becomes

```
publisher packet

↓

identify frame

↓

layer scheduler

↓

which subscribers want it?

↓

forward
```

The scheduler understands:

* Keyframe
* Delta frame
* Layer dependency
* Active speaker
* Pinning
* Bandwidth

## Phase 5: NACK / RTX

Support:

* RTX cache
* Packet history
* NACK parser
* Retransmission scheduler

## Phase 6: PLI / FIR

Generate:

* PLI
* FIR

when:

* layer switching
* new subscriber
* packet loss

---

## Phase 7: REMB Removal

Modern WebRTC prefers:

* TWCC
* GCC

REMB is legacy.

## Phase 8: Advanced GCC

Now optimize:

* pacing
* packet prioritization
* fairness
* probing
* bitrate allocation

# Architecture

At that point the SFU pipeline becomes:

```
UDP

↓

ICE

↓

DTLS

↓

SRTP

↓

RTP Parser

↓

SVC Parser

↓

Congestion Controller (TWCC)

↓

Layer Scheduler

↓

Packet Router

↓

SRTP

↓

UDP
```

## Priority item

1. **SVC support** (layer parsing and forwarding)
2. **TWCC feedback generation**
3. **Google Congestion Control (GCC) bandwidth estimator**
4. **Layer scheduler driven by GCC**
5. **RTX/NACK**
6. **PLI/FIR**
7. **Pacing and advanced bandwidth probing**

Here is how those 11 steps map directly to your codebase's concrete functions and execution units:

| Step | Architecture Layer | Code Base Mapping / Function |
| --- | --- | --- |
| **1** | UDP Ingress | **`io_uring`** (or socket `recvfrom`/`recvmmsg` ring) |
| **2** | ICE Layer | **`handle_stun()`** |
| **3** | DTLS Layer | **`handle_dtls()`** |
| **4** | SRTP Decryption | **`sfu_srtp_unprotect()`** *(Note: `sfu_srtp_ctx_init_from_dtls` extracts keying material once during handshake; `unprotect` decrypts every packet)* |
| **5** | RTP Parser | **`sfu_room_forward_packet()`** $\rightarrow$ `sfu_rtp_parse()` |
| **6** | SVC Parser | **`sfu_room_forward_packet()`** $\rightarrow$ `sfu_svc_parse()` |
| **7** | Congestion Control | **`sfu_room_forward_packet()`** $\rightarrow$ `sfu_twcc_parser_next()` / `gcc_bwe_process_twcc_packet()` |
| **8** | Layer Scheduler | **`sfu_room_forward_packet()`** $\rightarrow$ Layer gating / `needs_keyframe` checks |
| **9** | Packet Router | **Fanout job** / ring buffer cross-thread enqueue to subscriber queues |
| **10** | Outbound SRTP | Header rewriting (SSRC, sequence/timestamp normalization) $\rightarrow$ **`sfu_srtp_protect()`** |
| **11** | UDP Egress | **`io_uring`** (or `sendmmsg`/`sendto` write ring) |

Steps **5, 6, 7, and 8** are indeed encapsulated inside **`sfu_room_forward_packet()`** executing on the worker thread, while Step 9 hands off the processed packet to the subscriber egress pipeline where Step 10 (`protect`) and Step 11 (`io_uring` write) take over.
