If your long-term goal is **a production-grade SFU comparable to Google Cloud Conferencing (GCC) quality**, then yes—the first major milestone is **SVC**.

I'd structure the roadmap like this:

## Phase 1: Transport Layer (Current)

* ✅ ICE
* ✅ DTLS
* ✅ SRTP
* ✅ RTP/RTCP
* ✅ Packet forwarding
* ✅ SDP negotiation
* ✅ Payload type translation
* ✅ Per-peer transceiver model

At the end of this phase, you have a working SFU.

---

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

---

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

---

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

---

## Phase 5: NACK / RTX

Support:

* RTX cache
* Packet history
* NACK parser
* Retransmission scheduler

---

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

---

## Phase 8: Advanced GCC

Now optimize:

* pacing
* packet prioritization
* fairness
* probing
* bitrate allocation

---

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

---

## Recommendation

I would prioritize the work in this order:

1. **SVC support** (layer parsing and forwarding)
2. **TWCC feedback generation**
3. **Google Congestion Control (GCC) bandwidth estimator**
4. **Layer scheduler driven by GCC**
5. **RTX/NACK**
6. **PLI/FIR**
7. **Pacing and advanced bandwidth probing**

This order mirrors how modern SFUs achieve high-quality video: first make multiple scalable layers available (SVC), then use congestion control (GCC) to select the appropriate layer dynamically based on each subscriber's network conditions.
