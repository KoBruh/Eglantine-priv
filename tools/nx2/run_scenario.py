#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run eden-cli through a scripted scenario and report what the emulator did wrong.

    python tools/nx2/run_scenario.py <scenario> [--out DIR] [--env NAME=VALUE ...]

A scenario is a text file of steps; the game drives itself, so the result is repeatable:

    game: C:\\games\\Pokopia\\updates\\655360\\exefs\\main   (or pass --game)
    duration: 120          # seconds before the run is stopped
    capture_every: 60      # save a frame every N presents (EDEN_NX2_FRAME_CAPTURE_EVERY)
    env: EDEN_NX2_REJECT=0x2320ad7500   # any extra environment for this run
    at 15: press A         # tap a button (0.3 s) at 15 s
    at 70: hold LS_UP 1.5  # hold for 1.5 s
    at f3000: press PLUS   # at the 3000th present instead of a time

Buttons are the Switch names (A B X Y L R ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT LSTICK RSTICK)
and stick directions (LS_UP LS_DOWN LS_LEFT LS_RIGHT, RS_...), mapped to keys through the CLI's
sdl2-config.ini. The output directory gets log.txt, err.txt, frames/ (PNGs + sheet.png) and
report.md / report.json: crash state, rejected shaders, failed pipelines, skipped draws and
dispatches, unimplemented services, assertions and frame times.
"""
import argparse
import collections
import csv
import datetime
import json
import os
import pathlib
import re
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import frames as frame_tools  # noqa: E402
import thread_cpu  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_EXE = REPO / "build" / "bin" / ("eden-cli.exe" if os.name == "nt" else "eden-cli")
TAP_SECONDS = 0.3


def default_config():
    portable = DEFAULT_EXE.parent / "user" / "config" / "sdl2-config.ini"
    if portable.exists():
        return portable
    base = pathlib.Path(os.environ.get("APPDATA", pathlib.Path.home() / ".config"))
    return base / "eden" / "config" / "sdl2-config.ini"


def key_map(config_path):
    """Switch button name -> SDL scancode, from player 0 of sdl2-config.ini."""
    keys = {}
    text = config_path.read_text(encoding="utf-8", errors="replace")
    for name, code in re.findall(r'^player_0_button_(\w+)="engine:keyboard,code:(\d+)', text, re.M):
        if code != "0":
            keys[name.upper()] = int(code)
    for stick, prefix in (("lstick", "LS"), ("rstick", "RS")):
        match = re.search(rf'^player_0_{stick}="([^"]*)"', text, re.M)
        if match:
            for direction, code in re.findall(r"(up|down|left|right):engine\$0keyboard\$1code\$0(\d+)", match.group(1)):
                keys[f"{prefix}_{direction.upper()}"] = int(code)
    return keys


# --uncapped: no frame limiter and no vsync, so frame times show what a frame costs.
UNCAPPED = {"use_speed_limit": "false", "use_vsync": "0"}


def run_config(source, destination, overrides):
    """Copy the CLI config for this run (the CLI writes its config back) with settings changed."""
    text = source.read_text(encoding="utf-8", errors="replace")
    for name, value in overrides.items():
        # A setting with \default=true ignores its value line, so both lines are set.
        if not re.search(rf"^{re.escape(name)}=", text, re.M):
            # A setting newer than the config (the CLI writes it on its next exit): add it to
            # [Renderer], where the emulator's newer settings live.
            if not re.search(r"^\[Renderer\]$", text, re.M):
                sys.exit(f"{source} has no {name} setting and no [Renderer] section to add it to")
            print(f"note: {name} is not in {source.name}; adding it to [Renderer]")
            text = re.sub(r"^\[Renderer\]$", lambda _: f"[Renderer]\n{name}\\default=false\n{name}={value}",
                          text, count=1, flags=re.M)
            continue
        text = re.sub(rf"^{re.escape(name)}\\default=.*$", lambda _: f"{name}\\default=false", text, flags=re.M)
        text = re.sub(rf"^{re.escape(name)}=.*$", lambda _: f"{name}={value}", text, flags=re.M)
    destination.write_text(text, encoding="utf-8")


def parse_scenario(path, keys):
    settings = {"duration": 120.0, "capture_every": 120, "env": {}, "game": None, "until_frame": 0}
    events = []  # (sort key, "when:code")
    for number, raw in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        step = re.fullmatch(r"at\s+(f?)([\d.]+)\s*:\s*(press|hold|release)\s+(\w+)(?:\s+([\d.]+))?", line, re.I)
        if step:
            frame, when, action, button, length = step.groups()
            code = keys.get(button.upper())
            if code is None:
                sys.exit(f"{path}:{number}: no key is mapped to {button} (known: {', '.join(sorted(keys))})")
            start = float(when)
            prefix = "f" if frame else ""
            if action.lower() in ("press", "hold"):
                hold = float(length) if action.lower() == "hold" and length else TAP_SECONDS
                # A frame-timed release is 1 frame per ~1/60 s of hold.
                end = start + (round(hold * 60) if frame else hold)
                events.append((start, f"{prefix}{start:g}:{code}"))
                events.append((end, f"{prefix}{end:g}:-{code}"))
            else:
                events.append((start, f"{prefix}{start:g}:-{code}"))
            continue
        setting = re.fullmatch(r"(\w+)\s*:\s*(.+)", line)
        if not setting:
            sys.exit(f"{path}:{number}: cannot read '{raw}'")
        name, value = setting.group(1).lower(), setting.group(2).strip()
        if name == "env":
            env_name, _, env_value = value.partition("=")
            settings["env"][env_name.strip()] = env_value.strip()
        elif name in ("duration", "capture_every", "until_frame"):
            settings[name] = float(value) if name == "duration" else int(value)
        elif name == "game":
            settings["game"] = value
        else:
            sys.exit(f"{path}:{number}: unknown setting '{name}'")
    events.sort(key=lambda event: event[0])
    return settings, [text for _, text in events]


# Log lines worth reporting, grouped by a key with the variable parts (timestamps) removed.
PATTERNS = {
    "shader_rejected": re.compile(r"Native (graphics|compute) shader rejected: Native shader at (0x[0-9a-f]+), "
                                  r"instruction (0x[0-9a-f]+): ([^\[]+?)\s*\[op ([0-9a-f]+)"),
    "pipeline_error": re.compile(r"Render\.Vulkan <Error> \S+:(Create\w+Pipeline): (.+)"),
    "draw_skipped": re.compile(r"Draw skipped \(([^)]*)\): vs=(0x[0-9a-f]+) ps=(0x[0-9a-f]+)"),
    "dispatch_failed": re.compile(r"Native QMD failed: (.+)"),
    "unimplemented": re.compile(r"Unknown / unimplemented function '(\d+)\(([^)]*)\)': port='([^']+)'"),
    "stubbed": re.compile(r"<\w+> (\S+?):\d+:(\w+): \(STUBBED\)"),
    "assert": re.compile(r"<Critical> (\S+?):\d+:[^:]*: \S+: assert (.+)"),
    "gpu_error": re.compile(r"(HW\.GPU|Render\.\w+) <Error> \S+?:\d+:(\w+): (.+)"),
}
PRESENTS = re.compile(r"\[\s*([\d.]+)\].*last 600 presents: frame (\d+) us \(max (\d+), (\d+) over 100 ms\)")
CXX = re.compile(r"C\+\+ exception thrown on thread \d+: (.+)\n\s+what\(\): (.*)")


def analyze(log_text, err_text):
    found = {name: collections.Counter() for name in PATTERNS}
    for line in log_text.splitlines():
        for name, pattern in PATTERNS.items():
            match = pattern.search(line)
            if not match:
                continue
            groups = match.groups()
            if name == "shader_rejected":
                kind, address, offset, reason, op = groups
                found[name][f"{kind} {address} +{offset}: {reason.strip()} (op {op})"] += 1
            elif name == "draw_skipped":
                found[name][f"{groups[0]}: vs={groups[1]} ps={groups[2]}"] += 1
            elif name == "unimplemented":
                found[name][f"{groups[2]} cmd {groups[0]} {groups[1]}".strip()] += 1
            elif name == "gpu_error" and ("shader rejected" in line or "QMD failed" in line or "Pipeline" in groups[1]):
                continue  # already counted by a more specific pattern
            else:
                found[name][": ".join(group.strip() for group in groups)] += 1
    presents = [{"time": float(t), "avg_frame_us": int(a), "max_frame_us": int(m), "over_100ms": int(o)}
                for t, a, m, o in PRESENTS.findall(log_text)]
    exceptions = collections.Counter(f"{types.split()[0]}: {what}" for types, what in CXX.findall(err_text))
    return found, presents, exceptions


# The boot loading screen is dark; the first capture brighter than this ends it (Pokopia: title).
BRIGHT_LUMA = 60


def loading_milestone(out, lumas):
    """(frame, seconds) of the first bright capture after a dark start, or None. Seconds come
    from threads.csv: the first sample whose latest captured frame had reached it."""
    frames = sorted(lumas)
    dark_seen = False
    for frame in frames:
        if lumas[frame] < BRIGHT_LUMA / 2:
            dark_seen = True
        elif dark_seen and lumas[frame] >= BRIGHT_LUMA:
            break
    else:
        return None
    try:
        with open(out / "threads.csv", newline="", encoding="utf-8") as file:
            reached = [float(row["seconds"]) for row in csv.DictReader(file) if int(row["frame"]) >= frame]
    except OSError:
        reached = []
    return frame, (min(reached) if reached else None)


def write_report(out, scenario, settings, events, result, found, presents, exceptions, frame_list, lumas):
    sections = [
        ("shader_rejected", "Shaders rejected before translation (their draws/dispatches never run)"),
        ("pipeline_error", "Pipelines that failed to build"),
        ("draw_skipped", "Draws skipped"),
        ("dispatch_failed", "Compute dispatches that failed"),
        ("gpu_error", "Other GPU/renderer errors"),
        ("unimplemented", "Unimplemented service commands (auto-stubbed)"),
        ("stubbed", "Stubbed functions called"),
        ("assert", "Assertions that failed"),
    ]
    lines = [f"# Scenario report: {scenario.name}", "",
             f"- Run: {result['started']} for {result['seconds']:.0f} s, "
             f"{'**exited before the end** (exit code ' + str(result['exit_code']) + ')' if result['crashed'] else 'no crash (stopped by the harness)'}",
             f"- Game: `{settings['game']}`",
             f"- Extra environment: {', '.join(f'`{k}={v}`' for k, v in settings['env'].items()) or 'none'}",
             f"- Config changes: {', '.join(f'`{k}={v}`' for k, v in settings.get('config', {}).items()) or 'none'}",
             f"- Frames captured: {len(frame_list)} (every {settings['capture_every']} presents) -> `frames/sheet.png`", ""]
    milestone = loading_milestone(out, lumas)
    if milestone:
        frame, seconds = milestone
        when = f"{seconds:.0f} s into the run" if seconds is not None else "time unknown"
        lines += [f"- Boot loading screen ends (first bright capture): frame {frame}, {when}"]
    if presents:
        worst = max(presents, key=lambda p: p["max_frame_us"])
        lines += [f"- Frame time: last window avg {presents[-1]['avg_frame_us'] / 1000:.1f} ms; worst stall "
                  f"{worst['max_frame_us'] / 1000:.0f} ms at {worst['time']:.0f} s", ""]
    if presents:
        lines += ["## Frame time per 600 presents", "", "| seconds | avg ms | max ms | over 100 ms |",
                  "| ---: | ---: | ---: | ---: |"]
        lines += [f"| {p['time']:.0f} | {p['avg_frame_us'] / 1000:.1f} | {p['max_frame_us'] / 1000:.0f} | "
                  f"{p['over_100ms']} |" for p in presents] + [""]
    if (out / "threads.csv").exists():
        busiest = thread_cpu.summarize(out / "threads.csv")[:12]
        lines += ["## Busiest threads (% of one host core; `threads.csv` has every second)", "",
                  "| thread | count | avg % | peak % |", "| --- | ---: | ---: | ---: |"]
        lines += [f"| {name} | {count} | {average:.0f} | {peak:.0f} |" for name, count, average, peak in busiest]
        lines += [""]
    if exceptions:
        lines += ["## C++ exceptions thrown (first chance; caught ones included)", ""]
        lines += [f"- {count}x {text}" for text, count in exceptions.most_common()] + [""]
    for key, title in sections:
        counter = found[key]
        lines += [f"## {title}: {sum(counter.values())}", ""]
        lines += [f"- {count}x {text}" for text, count in counter.most_common(40)] or ["- none"]
        lines += [""]
    lines += ["## Input script", "", "```", ",".join(events), "```", ""]
    (out / "report.md").write_text("\n".join(lines), encoding="utf-8")
    (out / "report.json").write_text(json.dumps({
        "scenario": str(scenario), "result": result, "settings": settings,
        "issues": {key: dict(counter) for key, counter in found.items()},
        "exceptions": dict(exceptions), "presents": presents, "frames": frame_list,
        "brightness": {str(frame): luma for frame, luma in sorted(lumas.items())},
    }, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, help="output directory (default: runs/<scenario>-<time>)")
    parser.add_argument("--game", help="game path (overrides the scenario's game:)")
    parser.add_argument("--exe", type=pathlib.Path, default=DEFAULT_EXE)
    parser.add_argument("--config", type=pathlib.Path, default=None, help="sdl2-config.ini for key mappings")
    parser.add_argument("--env", action="append", default=[], help="extra NAME=VALUE (repeatable)")
    parser.add_argument("--duration", type=float, help="override the scenario's duration (seconds)")
    parser.add_argument("--sound", action="store_true", help="keep the game's audio (muted by default)")
    parser.add_argument("--uncapped", action="store_true",
                        help="turn off the speed limit and vsync (benchmarks: frame times are real costs)")
    parser.add_argument("--set", action="append", default=[], metavar="NAME=VALUE",
                        help="change a setting in this run's copy of the config (repeatable)")
    parser.add_argument("--foreground", action="store_true",
                        help="show the game window normally (by default it renders off-screen and "
                             "never takes focus)")
    args = parser.parse_args()

    config = args.config or default_config()
    keys = key_map(config)
    settings, events = parse_scenario(args.scenario, keys)
    settings["game"] = args.game or settings["game"]
    if not settings["game"]:
        sys.exit("no game: put 'game: <path>' in the scenario or pass --game")
    if args.duration:
        settings["duration"] = args.duration
    for item in args.env:
        name, _, value = item.partition("=")
        settings["env"][name] = value

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = args.out or REPO / "build" / "nx2-runs" / f"{args.scenario.stem}-{stamp}"
    frames_dir = out / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)
    (out / "keys.txt").write_text(",".join(events), encoding="ascii")
    overrides = {} if args.sound else {"audio_muted": "true"}
    if args.uncapped:
        overrides.update(UNCAPPED)
    for item in args.set:
        name, _, value = item.partition("=")
        overrides[name] = value
    run_config(config, out / "sdl2-config.ini", overrides)
    settings["config"] = overrides

    env = dict(os.environ)
    env.update({
        "EDEN_NX2_KEY_SCRIPT": f"@{out / 'keys.txt'}",
        "EDEN_NX2_FRAME_CAPTURE": str(frames_dir),
        "EDEN_NX2_FRAME_CAPTURE_EVERY": str(settings["capture_every"]),
        "EDEN_NX2_CRASH_DUMP": str(out),
    })
    if not args.foreground:
        env["EDEN_NX2_HIDE_WINDOW"] = "background"
    env.update(settings["env"])

    print(f"running {args.scenario.name} for {settings['duration']:.0f} s -> {out}")
    started = time.monotonic()
    with open(out / "log.txt", "wb") as log, open(out / "err.txt", "wb") as err:
        # No console window either, so a run never shows up over what the user is doing (the game
        # window itself is handled by EDEN_NX2_HIDE_WINDOW).
        flags = 0
        if os.name == "nt" and not args.foreground:
            flags = subprocess.CREATE_NO_WINDOW
        process = subprocess.Popen([str(args.exe), "-c", str(out / "sdl2-config.ini"), "-g", settings["game"]],
                                   env=env, stdout=log, stderr=err, creationflags=flags)
        exit_code, crashed = None, False
        sampler = thread_cpu.Sampler(process.pid, out / "threads.csv")
        # Stop at until_frame (the capture of that present exists) or at the duration, whichever
        # comes first: emulation speed varies between runs, frames do not.
        while time.monotonic() - started < settings["duration"]:
            try:
                exit_code = process.wait(timeout=1)
                crashed = True
                break
            except subprocess.TimeoutExpired:
                pass
            latest = max((int(path.stem.removeprefix("nx2-frame-")) for path in frames_dir.glob("nx2-frame-*.tga")),
                         default=0)
            sampler.sample(time.monotonic() - started, latest)
            if settings["until_frame"] and latest >= settings["until_frame"]:
                time.sleep(1)  # let the last capture finish writing
                break
        sampler.close()
        if not crashed:
            process.kill()
            process.wait()
    seconds = time.monotonic() - started
    result = {"started": stamp, "seconds": seconds, "crashed": crashed,
              "exit_code": f"{exit_code & 0xffffffff:#x}" if exit_code is not None else None}

    frame_list = sorted(path.stem.removeprefix("nx2-frame-") for path in frames_dir.glob("nx2-frame-*.tga"))
    lumas = {int(tga.stem.removeprefix("nx2-frame-")): round(frame_tools.mean_luma(tga), 1)
             for tga in frames_dir.glob("nx2-frame-*.tga")}
    if frame_list:
        rows = frame_tools.convert(frames_dir, columns=6)
        (frames_dir / "sheet.txt").write_text("\n".join("  ".join(row) for row in rows), encoding="ascii")
        for tga in frames_dir.glob("*.tga"):
            tga.unlink()  # the PNGs are kept; TGAs are 3.5 MiB each
    log_text = (out / "log.txt").read_text(encoding="utf-8", errors="replace")
    err_text = (out / "err.txt").read_text(encoding="utf-8", errors="replace")
    found, presents, exceptions = analyze(log_text, err_text)
    write_report(out, args.scenario, settings, events, result, found, presents, exceptions, frame_list, lumas)

    print(f"{'CRASHED' if crashed else 'ok'} after {seconds:.0f} s; report: {out / 'report.md'}")
    for key in ("shader_rejected", "pipeline_error", "draw_skipped", "dispatch_failed"):
        print(f"  {key}: {sum(found[key].values())}")
    return 1 if crashed else 0


if __name__ == "__main__":
    sys.exit(main())
