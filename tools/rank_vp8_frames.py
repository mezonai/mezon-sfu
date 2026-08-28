#!/usr/bin/env python3
"""Rank VP8 frame CSV rows by size or transmission span."""

import argparse
import csv


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_file", help="CSV produced by vp8_frame_sizes.py")
    parser.add_argument("--sort", choices=("size", "span"), default="size")
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--complete-only", action="store_true")
    parser.add_argument("--min-size", type=int, default=0)
    parser.add_argument("--min-span-ms", type=float, default=0)
    args = parser.parse_args()

    with open(args.csv_file, newline="") as source:
        rows = list(csv.DictReader(source))

    if args.complete_only:
        rows = [row for row in rows if row["complete"] == "1"]
    rows = [
        row
        for row in rows
        if int(row["vp8_bytes"]) >= args.min_size
        and float(row["span_ms"]) >= args.min_span_ms
    ]
    key = "vp8_bytes" if args.sort == "size" else "span_ms"
    conversion = int if key == "vp8_bytes" else float
    rows.sort(key=lambda row: conversion(row[key]), reverse=True)

    print("rtp_timestamp,vp8_bytes,rtp_bytes,packets,span_ms,first_sequence,last_sequence,complete")
    for row in rows[: args.limit]:
        print(
            ",".join(
                [
                    row["rtp_timestamp"],
                    row["vp8_bytes"],
                    row["rtp_bytes"],
                    row["packets"],
                    row["span_ms"],
                    row["first_sequence"],
                    row["last_sequence"],
                    row["complete"],
                ]
            )
        )


if __name__ == "__main__":
    main()
