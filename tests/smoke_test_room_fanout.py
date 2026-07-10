#!/usr/bin/env python3
"""
Smoke test: 3 simulated peers each send one tagged UDP datagram to
mezon-sfu. Each peer should receive the *other two* peers' datagrams
back, and never its own -- validating the room registry's exclude-sender
logic and the actual send_zc forwarding path end to end over real UDP.
"""
import socket
import sys
import time

SFU_HOST = "127.0.0.1"
SFU_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 17000
NUM_PEERS = 3

socks = []
for i in range(NUM_PEERS):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(2.0)
    socks.append(s)

# First datagram from each peer also registers it in the room (touch_peer
# happens on recv), so subsequent sends are the ones that actually get
# forwarded to already-known peers. Send a throwaway "join" packet first,
# then the tagged payloads, with a short pause so all three are registered
# before any of them expects to receive fanout.
for i, s in enumerate(socks):
    s.sendto(f"join-{i}".encode(), (SFU_HOST, SFU_PORT))

time.sleep(0.3)
# drain any echoes/joins that arrived so far (none expected yet since join
# packets predate other peers being registered, but be safe)
for s in socks:
    try:
        while True:
            s.recvfrom(2048)
    except socket.timeout:
        pass

tags = [f"payload-from-peer-{i}".encode() for i in range(NUM_PEERS)]
for i, s in enumerate(socks):
    s.sendto(tags[i], (SFU_HOST, SFU_PORT))
    time.sleep(0.05)

time.sleep(0.3)

received = [set() for _ in range(NUM_PEERS)]
for i, s in enumerate(socks):
    try:
        while True:
            data, _ = s.recvfrom(2048)
            received[i].add(data)
    except socket.timeout:
        pass

ok = True
for i in range(NUM_PEERS):
    expected = {tags[j] for j in range(NUM_PEERS) if j != i}
    got = received[i]
    if tags[i] in got:
        print(f"FAIL: peer {i} received its own packet back")
        ok = False
    missing = expected - got
    if missing:
        print(f"FAIL: peer {i} did not receive: {missing}")
        ok = False
    extra = got - expected
    if extra:
        print(f"FAIL: peer {i} received unexpected: {extra}")
        ok = False
    print(f"peer {i}: received {[d.decode() for d in got]}")

for s in socks:
    s.close()

if ok:
    print("smoke_test_room_fanout: OK")
    sys.exit(0)
else:
    print("smoke_test_room_fanout: FAILED")
    sys.exit(1)
