#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sample Linux process CPU time and resident memory for benchmark trials."""
import argparse
import csv
import os
import pathlib
import time


def read_process_sample(pid):
    stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
    closing = stat.rfind(")")
    if closing < 0:
        raise ValueError("malformed /proc process stat record")
    fields = stat[closing + 1:].split()
    if len(fields) < 22:
        raise ValueError("truncated /proc process stat record")
    ticks = int(fields[11]) + int(fields[12])  # utime + stime
    rss_pages = max(0, int(fields[21]))
    ticks_per_second = os.sysconf("SC_CLK_TCK")
    page_size = os.sysconf("SC_PAGE_SIZE")
    return time.monotonic(), ticks / ticks_per_second, rss_pages * page_size / (1024.0 * 1024.0)


def sample_process(pid, output, interval):
    path = pathlib.Path(output)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["monotonic_seconds", "cpu_seconds_total", "rss_mib"])
        previous = None
        while True:
            try:
                current = read_process_sample(pid)
            except (FileNotFoundError, ProcessLookupError):
                break
            writer.writerow([f"{current[0]:.9f}", f"{current[1]:.9f}", f"{current[2]:.3f}"])
            handle.flush()
            previous = current
            time.sleep(interval)
        # A single sample still yields useful RSS, but no interval CPU rate.
        del previous


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--interval", type=float, default=0.25)
    args = parser.parse_args()
    if args.pid <= 0 or not 0.05 <= args.interval <= 10:
        parser.error("pid must be positive and interval must be in 0.05..10 seconds")
    sample_process(args.pid, args.output, args.interval)


if __name__ == "__main__":
    main()
