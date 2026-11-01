#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-thread CPU time of a running emulator, by thread name (Windows; standard library only).

run_scenario.py samples the game once a second and writes threads.csv: for every interval, each
thread's CPU time as a percentage of one host core. A guest CPU core thread near 100% means the
game is computing (emulated CPU bound); low everywhere while the game waits means it is waiting
on something (a timer, I/O, the GPU or a service).

    python tools/nx2/thread_cpu.py <threads.csv> [--from SECONDS] [--to SECONDS]

prints the average and peak per thread name over that part of a run.
"""
import argparse
import collections
import csv
import ctypes
import os
import pathlib
import sys

if os.name == "nt":
    from ctypes import wintypes

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class _ThreadEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ThreadID", wintypes.DWORD), ("th32OwnerProcessID", wintypes.DWORD),
                    ("tpBasePri", wintypes.LONG), ("tpDeltaPri", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD)]

    _kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _kernel32.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ThreadEntry)]
    _kernel32.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ThreadEntry)]
    _kernel32.OpenThread.restype = wintypes.HANDLE
    _kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.GetThreadTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    _kernel32.GetThreadDescription.restype = ctypes.c_long
    _kernel32.GetThreadDescription.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.LPWSTR)]
    _kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

_SNAPTHREAD = 0x4
_QUERY_LIMITED_INFORMATION = 0x0800
_INVALID_HANDLE = ctypes.c_void_p(-1).value


def _filetime(value):
    return value.dwHighDateTime << 32 | value.dwLowDateTime


def thread_times(pid):
    """{thread id: (name, user + kernel CPU time in seconds)} for every thread of pid."""
    if os.name != "nt":
        return {}
    snapshot = _kernel32.CreateToolhelp32Snapshot(_SNAPTHREAD, 0)
    if snapshot in (None, _INVALID_HANDLE):
        return {}
    result = {}
    try:
        entry = _ThreadEntry()
        entry.dwSize = ctypes.sizeof(entry)
        more = _kernel32.Thread32First(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32OwnerProcessID == pid:
                handle = _kernel32.OpenThread(_QUERY_LIMITED_INFORMATION, False, entry.th32ThreadID)
                if handle:
                    try:
                        times = [wintypes.FILETIME() for _ in range(4)]
                        name = ""
                        description = wintypes.LPWSTR()
                        if _kernel32.GetThreadDescription(handle, ctypes.byref(description)) >= 0:
                            name = description.value or ""
                            _kernel32.LocalFree(ctypes.cast(description, ctypes.c_void_p))
                        if _kernel32.GetThreadTimes(handle, *(ctypes.byref(t) for t in times)):
                            cpu = (_filetime(times[2]) + _filetime(times[3])) / 1e7
                            result[entry.th32ThreadID] = (name or f"thread {entry.th32ThreadID}", cpu)
                    finally:
                        _kernel32.CloseHandle(handle)
            more = _kernel32.Thread32Next(snapshot, ctypes.byref(entry))
    finally:
        _kernel32.CloseHandle(snapshot)
    return result


class Sampler:
    """Writes one CSV row per thread per sample: time, frame, thread id, name, CPU % of a core."""

    def __init__(self, pid, path):
        self.pid = pid
        self.file = open(path, "w", newline="", encoding="utf-8")
        self.writer = csv.writer(self.file)
        self.writer.writerow(["seconds", "frame", "tid", "name", "cpu_percent"])
        self.last = {}
        self.last_time = None

    def sample(self, seconds, frame):
        now = thread_times(self.pid)
        if self.last_time is not None and seconds > self.last_time:
            interval = seconds - self.last_time
            for tid, (name, cpu) in now.items():
                previous = self.last.get(tid, (name, 0.0))[1]
                percent = max(0.0, cpu - previous) * 100 / interval
                if percent >= 0.5:
                    self.writer.writerow([f"{seconds:.1f}", frame, tid, name, f"{percent:.1f}"])
        self.last, self.last_time = now, seconds

    def close(self):
        self.file.close()


def summarize(path, start=0.0, end=float("inf")):
    """[(name, threads, average %, peak %)] over [start, end), busiest first. Threads sharing a
    name are added up per sample (average and peak of the sum)."""
    per_sample = collections.defaultdict(lambda: collections.defaultdict(float))
    tids = collections.defaultdict(set)
    samples = set()
    with open(path, newline="", encoding="utf-8") as file:
        for row in csv.DictReader(file):
            seconds = float(row["seconds"])
            if not start <= seconds < end:
                continue
            samples.add(seconds)
            per_sample[row["name"]][seconds] += float(row["cpu_percent"])
            tids[row["name"]].add(row["tid"])
    count = max(1, len(samples))
    rows = [(name, len(tids[name]), sum(values.values()) / count, max(values.values()))
            for name, values in per_sample.items()]
    return sorted(rows, key=lambda row: -row[2])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", type=pathlib.Path)
    parser.add_argument("--from", dest="start", type=float, default=0.0, help="seconds into the run")
    parser.add_argument("--to", dest="end", type=float, default=float("inf"))
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    if not args.csv.exists():
        sys.exit(f"{args.csv}: not found")
    print(f"{'thread':32} {'n':>3} {'avg %':>7} {'peak %':>7}")
    for name, count, average, peak in summarize(args.csv, args.start, args.end)[:args.top]:
        print(f"{name[:32]:32} {count:>3} {average:7.1f} {peak:7.1f}")


if __name__ == "__main__":
    main()
