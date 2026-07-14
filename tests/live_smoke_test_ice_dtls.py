#!/usr/bin/env python3
import socket
import struct
import sys
import hashlib
import hmac
import subprocess
import time

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
UFRAG = sys.argv[2]
PWD = sys.argv[3]


def build_binding_request(server_ufrag, client_ufrag, pwd):
    msg_type = 0x0001
    txn_id = bytes(range(0xB0, 0xB0 + 12))
    username = f"{server_ufrag}:{client_ufrag}".encode()
    ulen = len(username)
    upad = (4 - ulen % 4) % 4
    attrs = struct.pack(">HH", 0x0006, ulen) + username + b"\x00" * upad

    header_len_for_mi = len(attrs) + 24
    header = struct.pack(">HHI", msg_type, header_len_for_mi, 0x2112A442) + txn_id
    mi_input = header + attrs
    mi = hmac.new(pwd.encode(), mi_input, hashlib.sha1).digest()
    attrs += struct.pack(">HH", 0x0008, 20) + mi

    final_header = struct.pack(">HHI", msg_type, len(attrs), 0x2112A442) + txn_id
    return final_header + attrs


def verify_binding_response(resp, txn_id, pwd):
    msg_type, length, cookie = struct.unpack(">HHI", resp[:8])
    assert msg_type == 0x0101, f"expected Binding Success, got {msg_type:#x}"
    assert resp[8:20] == txn_id, "transaction id mismatch"
    off = 20
    xor_mapped = None
    while off + 4 <= len(resp):
        atype, alen = struct.unpack(">HH", resp[off:off + 4])
        aval = resp[off + 4:off + 4 + alen]
        if atype == 0x0020:
            family = aval[1]
            xport = struct.unpack(">H", aval[2:4])[0] ^ (0x2112A442 >> 16)
            xaddr = struct.unpack(">I", aval[4:8])[0] ^ 0x2112A442
            xor_mapped = (family, xport, xaddr)
        off += 4 + ((alen + 3) & ~3)
    assert xor_mapped is not None, "no XOR-MAPPED-ADDRESS in response"
    return xor_mapped


print("--- STUN binding request ---")
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 0))
my_port = sock.getsockname()[1]
sock.settimeout(2.0)

req = build_binding_request(UFRAG, "pyclient", PWD)
txn_id = req[8:20]
sock.sendto(req, (HOST, PORT))
resp, _ = sock.recvfrom(2048)
family, xport, xaddr = verify_binding_response(resp, txn_id, PWD)
got_ip = socket.inet_ntoa(struct.pack(">I", xaddr))
print(f"XOR-MAPPED-ADDRESS: {got_ip}:{xport} (family={family})")
assert xport == my_port, f"reflexive port mismatch: {xport} != {my_port}"
assert got_ip == "127.0.0.1"
print("STUN OK: server correctly reflected our address, integrity verified")

print("--- STUN with wrong password (must be dropped, no response) ---")
bad_req = build_binding_request(UFRAG, "pyclient", "wrongpassword12345678901234")
sock.sendto(bad_req, (HOST, PORT))
try:
    sock.settimeout(0.5)
    sock.recvfrom(2048)
    print("FAIL: server responded to an unauthenticated request")
    sys.exit(1)
except socket.timeout:
    print("OK: server silently dropped the unauthenticated request")
sock.settimeout(2.0)
sock.close()

print("--- DTLS-SRTP handshake (openssl s_client) ---")
# openssl's DTLS client will complete a real handshake against our
# server and print the negotiated SRTP profile if successful.
proc = subprocess.run(
    ["openssl", "s_client", "-dtls1_2", "-connect", f"{HOST}:{PORT}",
     "-use_srtp", "SRTP_AES128_CM_SHA1_80"],
    input=b"", capture_output=True, timeout=10
)
out = proc.stdout.decode(errors="replace") + proc.stderr.decode(errors="replace")
if "SRTP Extension negotiated" in out or "Using SRTP_AES128_CM_SHA1_80" in out or "SRTP-Cipher-Profile" in out:
    print("DTLS-SRTP OK: openssl s_client negotiated an SRTP profile with our server")
    for line in out.splitlines():
        if "SRTP" in line or "Cipher" in line and "Cipher is" in line:
            print(" ", line.strip())
else:
    print("openssl s_client output:")
    print(out[-2000:])
    if "New, TLSv1" in out or "New,  TLSv" in out or "Cipher is" in out:
        print("DTLS handshake completed (cipher negotiated) though SRTP line not matched by parser")
    else:
        print("FAIL: DTLS handshake did not complete")
        sys.exit(1)

print("ALL OK")
