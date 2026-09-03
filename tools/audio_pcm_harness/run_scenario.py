#!/usr/bin/env python3
"""Run one PCM-capture scenario against G-Diffuser.exe and collect its SHA-256.

Part of the C-R2.3 bit-identical PCM gate. A scenario is a small JSON file
(scenarios/*.json) declaring the determinism-pin environment, an optional
tick-scripted autoinput file, and a capture-frame window. This launcher:

  1. Sets GDX_PCM_CAPTURE=<out-dir>/<id>  plus GDX_PCM_CAPTURE_FRAMES and the
     scenario's env pins (GDX_RAND_SEED1/2, GDX_AUDIO_THREAD=0, ...).
  2. If the scenario names an autoinput file, drops it next to the exe as
     gdx-autoinput.txt (backing up any pre-existing file, restored afterwards).
  3. Launches the exe with cwd = the exe directory (so it finds its assets AND
     the gdx-autoinput.txt), and waits for it to exit. The exe auto-exits once
     the capture window fills (main.cpp polls gdx_pcm_capture_finished()), so a
     bounded GDX_PCM_CAPTURE_FRAMES makes this fully headless. A timeout guards a
     hung/never-finishing run.
  4. Collects <out-dir>/<id>.pcm.sha256 and prints the digest.

Scenario JSON schema:
  id               str    output prefix / gdx-autoinput name stem (required)
  title            str    human label
  status           str    ready | needs-recording | blocked (informational)
  autoinput        str?   filename (relative to scenarios/) copied to the exe dir, or null
  capture_frames   int    GDX_PCM_CAPTURE_FRAMES (bounded window; required for headless auto-exit)
  timeout_seconds  int    hard wall-clock kill deadline for the child
  env              obj    extra environment variables (string->string)
  notes            list   free text

Usage:
  run_scenario.py --exe path/to/G-Diffuser.exe --scenario scenarios/01_title_bgm.json
                  [--out-dir OUT] [--label LABEL] [--extra-env K=V ...] [--dry-run]

Exit 0 if the run finished and a .pcm.sha256 was produced; 1 otherwise.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time


def load_scenario(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def read_sidecar_digest(path):
    """First whitespace-delimited token of a .pcm.sha256 sidecar (sha256sum style)."""
    with open(path, "r", encoding="utf-8") as f:
        line = f.readline().strip()
    return line.split()[0] if line else ""


def run(exe, scenario_path, out_dir, label, extra_env, dry_run):
    scenario = load_scenario(scenario_path)
    sid = scenario["id"]
    exe = os.path.abspath(exe)
    exe_dir = os.path.dirname(exe)
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    prefix = os.path.join(out_dir, label or sid)
    pcm_path = prefix + ".pcm"
    sha_path = prefix + ".pcm.sha256"
    # Clear stale artifacts so a failed launch cannot be mistaken for success.
    for p in (pcm_path, sha_path):
        if os.path.exists(p):
            os.remove(p)

    env = dict(os.environ)
    env.update({str(k): str(v) for k, v in scenario.get("env", {}).items()})
    env["GDX_PCM_CAPTURE"] = prefix
    frames = scenario.get("capture_frames")
    if frames:
        env["GDX_PCM_CAPTURE_FRAMES"] = str(int(frames))
    for kv in extra_env or []:
        if "=" not in kv:
            raise ValueError("--extra-env must be K=V, got %r" % kv)
        k, v = kv.split("=", 1)
        env[k] = v

    # Stage the autoinput script next to the exe (input_bridge.c reads gdx-autoinput.txt from the
    # process CWD, and we launch with cwd = exe_dir). Back up any pre-existing file and restore it.
    autoinput = scenario.get("autoinput")
    live_autoinput = os.path.join(exe_dir, "gdx-autoinput.txt")
    backup = None
    staged = False
    if autoinput:
        src = os.path.join(os.path.dirname(os.path.abspath(scenario_path)), autoinput)
        if not os.path.isfile(src):
            print("ERROR: scenario autoinput file not found: %s" % src, file=sys.stderr)
            return 1
        if os.path.exists(live_autoinput):
            backup = live_autoinput + ".harness-bak"
            shutil.move(live_autoinput, backup)
        shutil.copyfile(src, live_autoinput)
        staged = True

    cmd = [exe]
    timeout = int(scenario.get("timeout_seconds", 300))
    print("[run_scenario] scenario=%s (%s) status=%s" %
          (sid, scenario.get("title", ""), scenario.get("status", "?")))
    print("[run_scenario] out prefix : %s" % prefix)
    print("[run_scenario] capture    : GDX_PCM_CAPTURE_FRAMES=%s" % env.get("GDX_PCM_CAPTURE_FRAMES", "<unbounded>"))
    print("[run_scenario] pins       : %s" %
          ", ".join("%s=%s" % (k, env[k]) for k in sorted(scenario.get("env", {}))))
    print("[run_scenario] autoinput  : %s" % (autoinput or "<none>"))
    print("[run_scenario] cmd        : %s (cwd=%s, timeout=%ds)" % (" ".join(cmd), exe_dir, timeout))

    if dry_run:
        print("[run_scenario] --dry-run: not launching.")
        return 0

    if not os.path.isfile(exe):
        print("ERROR: exe not found: %s" % exe, file=sys.stderr)
        return 1

    try:
        started = time.time()
        try:
            proc = subprocess.run(cmd, cwd=exe_dir, env=env, timeout=timeout)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            print("ERROR: scenario timed out after %ds (capture never finalized). "
                  "Increase capture window / timeout, or the scenario never reached audio."
                  % timeout, file=sys.stderr)
            return 1
        elapsed = time.time() - started
        print("[run_scenario] exit code %d after %.1fs" % (rc, elapsed))
    finally:
        if staged:
            os.remove(live_autoinput)
            if backup:
                shutil.move(backup, live_autoinput)

    if not os.path.isfile(sha_path):
        print("ERROR: no sidecar produced at %s. The capture never finalized "
              "(did the run reach audio? is GDX_PCM_CAPTURE_FRAMES set?)." % sha_path,
              file=sys.stderr)
        return 1

    digest = read_sidecar_digest(sha_path)
    pcm_size = os.path.getsize(pcm_path) if os.path.isfile(pcm_path) else 0
    print("")
    print("[run_scenario] PCM     : %s (%d bytes)" % (pcm_path, pcm_size))
    print("[run_scenario] SHA-256 : %s" % digest)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="Run one PCM-capture scenario and collect its SHA-256.")
    ap.add_argument("--exe", required=True, help="path to G-Diffuser.exe")
    ap.add_argument("--scenario", required=True, help="path to a scenarios/*.json file")
    ap.add_argument("--out-dir", default="captures", help="directory for <id>.pcm + .sha256")
    ap.add_argument("--label", default=None, help="override output prefix stem (default: scenario id)")
    ap.add_argument("--extra-env", action="append", default=[], metavar="K=V",
                    help="extra environment variable (repeatable)")
    ap.add_argument("--dry-run", action="store_true", help="print the plan without launching")
    args = ap.parse_args(argv)
    try:
        return run(args.exe, args.scenario, args.out_dir, args.label, args.extra_env, args.dry_run)
    except (OSError, ValueError, KeyError) as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
