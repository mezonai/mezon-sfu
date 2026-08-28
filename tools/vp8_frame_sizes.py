#!/usr/bin/env python3
"""Export per-frame VP8 RTP sizes and timing from a classic PCAP capture."""

import argparse
import csv
import math
import struct
import sys
from collections import OrderedDict


def percentile(values, percentile_value):
    if not values:
        return 0
    values = sorted(values)
    index = max(0, min(len(values) - 1, math.ceil(len(values) * percentile_value) - 1))
    return values[index]


def read_pcap(path):
    with open(path, "rb") as capture:
        global_header = capture.read(24)
        if len(global_header) != 24:
            raise RuntimeError("invalid pcap header")

        magic = global_header[:4]
        formats = {
            b"\xd4\xc3\xb2\xa1": ("<", False),
            b"\xa1\xb2\xc3\xd4": (">", False),
            b"\x4d\x3c\xb2\xa1": ("<", True),
            b"\xa1\xb2\x3c\x4d": (">", True),
        }
        if magic not in formats:
            raise RuntimeError("unsupported capture format; convert pcapng to pcap first")

        endian, nanosecond = formats[magic]
        link_type = struct.unpack(endian + "I", global_header[20:24])[0]

        while True:
            packet_header = capture.read(16)
            if len(packet_header) != 16:
                break
            seconds, fraction, captured_len, _ = struct.unpack(endian + "IIII", packet_header)
            packet = capture.read(captured_len)
            divisor = 1_000_000_000 if nanosecond else 1_000_000
            yield seconds + fraction / divisor, link_type, packet


def ipv4_offset(packet, link_type):
    if link_type == 101:
        return 0
    if link_type != 1 or len(packet) < 14:
        return None

    ether_type = struct.unpack("!H", packet[12:14])[0]
    offset = 14
    if ether_type == 0x8100 and len(packet) >= 18:
        ether_type = struct.unpack("!H", packet[16:18])[0]
        offset = 18
    return offset if ether_type == 0x0800 else None


def vp8_descriptor_length(payload):
    if not payload:
        return 0

    offset = 1
    if not payload[0] & 0x80:
        return offset
    if len(payload) <= offset:
        return len(payload)

    extension = payload[offset]
    offset += 1
    if extension & 0x80 and offset < len(payload):
        picture_id = payload[offset]
        offset += 1
        if picture_id & 0x80 and offset < len(payload):
            offset += 1
    if extension & 0x40 and offset < len(payload):
        offset += 1
    if extension & 0x30 and offset < len(payload):
        offset += 1
    return min(offset, len(payload))


def parse_rtp(packet, link_type):
    ip_offset = ipv4_offset(packet, link_type)
    if ip_offset is None or len(packet) < ip_offset + 20 or packet[ip_offset] >> 4 != 4:
        return None

    ip_header_len = (packet[ip_offset] & 0x0F) * 4
    if packet[ip_offset + 9] != 17:
        return None

    udp_offset = ip_offset + ip_header_len
    if len(packet) < udp_offset + 8:
        return None

    rtp = packet[udp_offset + 8 :]
    if len(rtp) < 12 or rtp[0] >> 6 != 2:
        return None

    header_len = 12 + (rtp[0] & 0x0F) * 4
    if rtp[0] & 0x10:
        if len(rtp) < header_len + 4:
            return None
        extension_words = struct.unpack("!H", rtp[header_len + 2 : header_len + 4])[0]
        header_len += 4 + extension_words * 4
    if len(rtp) < header_len:
        return None

    padding_len = rtp[-1] if rtp[0] & 0x20 else 0
    payload_end = len(rtp) - padding_len
    if payload_end < header_len:
        return None

    return {
        "marker": bool(rtp[1] & 0x80),
        "payload_type": rtp[1] & 0x7F,
        "sequence": struct.unpack("!H", rtp[2:4])[0],
        "timestamp": struct.unpack("!I", rtp[4:8])[0],
        "ssrc": struct.unpack("!I", rtp[8:12])[0],
        "rtp_bytes": len(rtp),
        "payload": rtp[header_len:payload_end],
    }


def analyze(capture_path, screen_ssrc, payload_type):
    frames = OrderedDict()
    for capture_time, link_type, packet in read_pcap(capture_path):
        rtp = parse_rtp(packet, link_type)
        if not rtp or rtp["ssrc"] != screen_ssrc or rtp["payload_type"] != payload_type:
            continue

        frame = frames.setdefault(
            rtp["timestamp"],
            {
                "first_time": capture_time,
                "last_time": capture_time,
                "first_sequence": rtp["sequence"],
                "last_sequence": rtp["sequence"],
                "packets": 0,
                "rtp_bytes": 0,
                "vp8_bytes": 0,
                "complete": False,
            },
        )
        descriptor_len = vp8_descriptor_length(rtp["payload"])
        frame["last_time"] = capture_time
        frame["last_sequence"] = rtp["sequence"]
        frame["packets"] += 1
        frame["rtp_bytes"] += rtp["rtp_bytes"]
        frame["vp8_bytes"] += max(0, len(rtp["payload"]) - descriptor_len)
        frame["complete"] |= rtp["marker"]

    return [
        {
            "rtp_timestamp": timestamp,
            "first_capture_time": frame["first_time"],
            "span_ms": (frame["last_time"] - frame["first_time"]) * 1000,
            "first_sequence": frame["first_sequence"],
            "last_sequence": frame["last_sequence"],
            "packets": frame["packets"],
            "rtp_bytes": frame["rtp_bytes"],
            "vp8_bytes": frame["vp8_bytes"],
            "complete": int(frame["complete"]),
        }
        for timestamp, frame in frames.items()
    ]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", help="classic PCAP file")
    parser.add_argument("screen_ssrc", type=lambda value: int(value, 0))
    parser.add_argument("--payload-type", type=int, default=96)
    parser.add_argument("--output", default="-", help="CSV path, or - for stdout")
    args = parser.parse_args()

    rows = analyze(args.capture, args.screen_ssrc, args.payload_type)
    fields = [
        "rtp_timestamp",
        "first_capture_time",
        "span_ms",
        "first_sequence",
        "last_sequence",
        "packets",
        "rtp_bytes",
        "vp8_bytes",
        "complete",
    ]
    output = sys.stdout if args.output == "-" else open(args.output, "w", newline="")
    try:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    finally:
        if output is not sys.stdout:
            output.close()

    complete = [row for row in rows if row["complete"]]
    sizes = [row["vp8_bytes"] for row in complete]
    spans = [row["span_ms"] for row in complete]
    if sizes:
        print(
            " ".join(
                [
                    f"frames={len(sizes)}",
                    f"size_p50={percentile(sizes, 0.50)}",
                    f"size_p90={percentile(sizes, 0.90)}",
                    f"size_p95={percentile(sizes, 0.95)}",
                    f"size_p99={percentile(sizes, 0.99)}",
                    f"size_max={max(sizes)}",
                    f"span_p99_ms={percentile(spans, 0.99):.2f}",
                    f"span_max_ms={max(spans):.2f}",
                ]
            ),
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
