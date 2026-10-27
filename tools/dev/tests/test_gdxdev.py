#!/usr/bin/env python3
"""Unit tests for gdxdev. Stdlib unittest only:  python3 tools/dev/tests/test_gdxdev.py

These are the golden-file checks the plan asks for (05-devcli-plan.md §5 steps 1-2):
the manifest round-trips through the schema check, the ini merge provably preserves a
foreign agent's key, and the log parser reproduces hand-computed numbers.
"""

import argparse
import binascii
import io
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import importlib.util
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from gdxdev import hwrun, inifile, prof, replay, runs, util  # noqa: E402
import hw  # noqa: E402
from gdxdev.util import GdxError  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


class ManifestTest(unittest.TestCase):
    def _full(self):
        return runs.new_manifest(
            run_id="20260907-120000-boot", track="devcli", lane="azahar",
            started="2026-09-07T12:00:00", ended="2026-09-07T12:01:00", wall_s=60.0,
            build={"id": "native/devcli@abcdef12", "dirty": False, "artifact": "x.3dsx",
                   "sha256": "0" * 64, "generator": "Ninja", "configure_s": 1.0,
                   "build_s": 2.0},
            tree={"branch": "native/devcli", "head": "a" * 40, "base": "native/main",
                  "base_head": "b" * 40, "branch_point": "b" * 40, "behind": 0,
                  "ahead": 3, "dirty": False, "current": True},
            ini={"debug.filelog": "1"}, replay=None, gputrace=False, seconds=60,
            platform={"host": "darwin-arm64", "azahar": "2126.0", "retroarch": None,
                      "model": "emu-new3ds"},
            result={"verdict": "PASS", "fatals": 0, "max_frame": 100,
                    "distinct_frames": 10, "errors": {}},
            artifacts={"log": "log.txt", "emu_log": "emu.log", "shots": []})

    def test_valid_manifest_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            rd = runs.RunDir(d)
            m = self._full()
            rd.write_manifest(m)
            self.assertEqual(rd.read_manifest(), m)
            self.assertEqual(runs.validate(rd.read_manifest()), [])

    def test_missing_key_is_rejected(self):
        m = self._full()
        del m["gputrace"]
        self.assertTrue(any("gputrace" in p for p in runs.validate(m)))

    def test_gputrace_must_be_bool(self):
        m = self._full()
        m["gputrace"] = "yes"
        self.assertTrue(any("gputrace" in p for p in runs.validate(m)))

    def test_a_manifest_without_a_tree_is_refused(self):
        """NEXT.md B46 / PROTOCOL.md: a parity cell is a claim about a tree. A manifest
        that does not name its branch point cannot support a parity claim, so the schema
        refuses it rather than letting the number be quoted bare."""
        m = self._full()
        m["tree"] = None
        problems = runs.validate(m)
        self.assertTrue(any("tree" in p for p in problems), problems)

    def test_a_tree_without_a_branch_point_is_refused(self):
        m = self._full()
        m["tree"] = dict(m["tree"], branch_point=None)
        self.assertTrue(any("branch_point" in p for p in runs.validate(m)))

    def test_a_half_filled_tree_is_refused(self):
        m = self._full()
        m["tree"] = {"branch": "native/x", "branch_point": "c" * 40}
        self.assertTrue(any("behind" in p for p in runs.validate(m)))

    def test_timeout_is_a_legal_verdict(self):
        """NEXT.md B45: a run cut short by --seconds is a different FACT from a crash,
        so it gets its own word rather than borrowing FAIL's."""
        m = self._full()
        m["result"] = dict(m["result"], verdict="TIMEOUT")
        self.assertEqual(runs.validate(m), [])

    def test_write_refuses_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            m = self._full()
            m["schema"] = 99
            with self.assertRaises(GdxError):
                runs.RunDir(d).write_manifest(m)


class IniMergeTest(unittest.TestCase):
    SAMPLE = (
        "; gdiffuser.ini\n"
        "\n"
        "[debug]\n"
        "console = 1\n"
        "filelog = 1\n"
        "gputrace = 0\n"
        "someone_elses_lever = 1\n"
        "\n"
        "[stereo]\n"
        "enabled = 1\n"
    )

    def _tmp(self):
        f = tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False)
        f.write(self.SAMPLE)
        f.close()
        return f.name

    def test_merge_preserves_foreign_keys(self):
        p = self._tmp()
        inifile.merge(p, {("debug", "gputrace"): "1"})
        got = inifile.read(p)
        # PROTOCOL.md line 17: another agent's lever must survive our write.
        self.assertEqual(got["debug"]["someone_elses_lever"], "1")
        self.assertEqual(got["debug"]["console"], "1")
        self.assertEqual(got["debug"]["gputrace"], "1")
        self.assertEqual(got["stereo"]["enabled"], "1")
        self.assertIn("; gdiffuser.ini", util.read_text(p))  # comments survive too
        os.unlink(p)

    def test_new_key_lands_in_its_section(self):
        p = self._tmp()
        inifile.merge(p, {("debug", "atlas3d"): "1"})
        got = inifile.read(p)
        self.assertEqual(got["debug"]["atlas3d"], "1")
        self.assertEqual(got["stereo"]["enabled"], "1")
        os.unlink(p)

    def test_new_section_is_appended(self):
        p = self._tmp()
        inifile.merge(p, {("native", "tier1"): "1"})
        self.assertEqual(inifile.read(p)["native"]["tier1"], "1")
        os.unlink(p)

    def test_dry_run_changes_nothing(self):
        p = self._tmp()
        inifile.merge(p, {("debug", "gputrace"): "1"}, dry_run=True)
        self.assertEqual(util.read_text(p), self.SAMPLE)
        os.unlink(p)

    def test_spec_parsing(self):
        self.assertEqual(inifile.parse_spec("gputrace=1"), ("debug", "gputrace", "1"))
        self.assertEqual(inifile.parse_spec("native.tier1=2"), ("native", "tier1", "2"))
        with self.assertRaises(GdxError):
            inifile.parse_spec("nope")


class AutoinputTest(unittest.TestCase):
    def _write(self, text):
        f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
        f.write(text)
        f.close()
        return f.name

    def test_valid(self):
        p = self._write("ticks\n# comment\n60 START\n120 A 30\n300 SHOT drive1\n")
        self.assertEqual(replay.validate_autoinput(p), 3)
        os.unlink(p)

    def test_seconds_mode_rejected(self):
        p = self._write("2 START\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_descending_ticks_rejected(self):
        p = self._write("ticks\n120 A\n60 B\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_unknown_token_rejected(self):
        p = self._write("ticks\n60 TURBO\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_too_many_events_rejected(self):
        p = self._write("ticks\n" + "".join("%d A\n" % (i * 10) for i in range(70)))
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_shot_needs_a_label(self):
        p = self._write("ticks\n60 SHOT\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)


class ProfParseTest(unittest.TestCase):
    """Golden numbers computed by hand from tools/dev/tests/fixtures/prof-sample.log."""

    @classmethod
    def setUpClass(cls):
        cls.p = prof.parse_file(os.path.join(FIXTURES, "prof-sample.log"))

    def test_counts(self):
        self.assertEqual(len(self.p.prof), 3)
        self.assertEqual(len(self.p.prof_storm), 1)
        self.assertEqual(len(self.p.profop), 1)
        self.assertEqual(len(self.p.gpu), 1)
        self.assertEqual(len(self.p.fill), 1)
        self.assertEqual(len(self.p.rt), 1)
        self.assertEqual(len(self.p.watchdog), 4)

    def test_prof_fields(self):
        w = self.p.prof[0]
        self.assertEqual(w["frame"], 64)
        self.assertEqual(w["n"], 64)
        self.assertAlmostEqual(w["br"], 3.20)
        self.assertAlmostEqual(w["dsp"], 6.50)
        self.assertEqual(w["nD"], 110)
        self.assertAlmostEqual(w["total"], 3.20 + 6.50 + 0.70 + 3.40 + 1.20 + 2.30 + 0.02 + 0.10)

    def test_section_means(self):
        s = prof.section_summary(self.p.prof)
        self.assertEqual(s["windows"], 3)
        self.assertEqual(s["frames"], 192)
        self.assertAlmostEqual(s["br"]["mean"], (3.20 + 3.50 + 3.80) / 3, places=6)
        self.assertAlmostEqual(s["dsp"]["mean"], (6.50 + 6.90 + 7.30) / 3, places=6)

    def test_nd_bucketing(self):
        buckets = dict((n, len(w)) for n, w in prof.bucket_by_nd(self.p.prof))
        self.assertEqual(buckets, {"crowd (nD 100-inf)": 3})

    def test_fps_percentiles(self):
        # beats: +0 (dropped), +130, +260, +150 -> 26.0, 52.0, 30.0 fps
        f = prof.fps_summary(self.p)
        self.assertEqual(f["beats"], 3)
        self.assertAlmostEqual(f["p50"], 30.0)
        self.assertAlmostEqual(f["min"], 26.0)
        self.assertAlmostEqual(f["max"], 52.0)
        self.assertEqual(f["max_frame"], 540)

    def test_profop(self):
        rows = prof.op_summary(self.p)
        self.assertEqual(rows[0][0], "E4")
        self.assertAlmostEqual(rows[0][1], 3.90)

    def test_errors_counted_once_each(self):
        self.assertEqual(self.p.errors["[fatal]"], 1)
        self.assertEqual(self.p.errors["[gfxfail]"], 1)
        self.assertEqual(self.p.errors["[fatal-trace]"], 0)
        self.assertEqual(self.p.fatals, 1)

    def test_gputrace_detected(self):
        self.assertTrue(self.p.gputrace)

    def test_percentile_is_nearest_rank(self):
        v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        self.assertEqual(prof.percentile(v, 10), 1)
        self.assertEqual(prof.percentile(v, 50), 5)
        self.assertEqual(prof.percentile(v, 90), 9)


class ShotBeatTest(unittest.TestCase):
    """A10: a [watchdog] beat that carried a replay SHOT is measuring the harness.

    The screenshot is a framebuffer readback plus a 230-288 KB BMP write, all on the main
    thread -- about 2 s per image on an SD card. On the 2026-09-20 hardware logs EVERY beat
    that dipped into the teens carried one, in every lever configuration, which is what made
    the fps beat sequence look identical run to run and lever to lever.
    """

    def test_beats_are_tagged_by_log_position(self):
        text = ("[watchdog] beat=1 frame=100(+100) stage=2\n"
                "[autotest] SHOT captured -> autotest/race00_L.bmp bytes=288054 frame=150\n"
                "[watchdog] beat=2 frame=160(+60) stage=2\n"
                "[watchdog] beat=3 frame=460(+300) stage=2\n")
        p = prof.parse_text(text)
        self.assertEqual([b["shots"] for b in p.watchdog], [0, 1, 0])

    def test_excluding_shot_beats_changes_the_summary(self):
        p = prof.parse_file(os.path.join(FIXTURES, "hw-fps-all1-interp1.log"))
        every = prof.fps_summary(p)
        clean = prof.fps_summary(p, exclude_shots=True)
        self.assertGreater(every["shot_beats"], 0)
        self.assertEqual(clean["beats"], every["beats"] - every["shot_beats"])
        # The dips into the teens are entirely SHOT beats: without them the floor is a
        # real frame rate, not a screenshot.
        self.assertLess(every["min"], 20.0)
        self.assertGreater(clean["min"], 30.0)
        self.assertGreater(clean["mean"], every["mean"])

    def test_the_legacy_dump_receipts_count_too(self):
        # Hardware takes the legacy path ("SHOT dumped" + "SHOT scanout dumped"), the
        # emulator the oracle one ("SHOT captured"). Both must tag the beat.
        text = ("[watchdog] beat=1 frame=100(+100)\n"
                "[autotest] SHOT dumped -> autotest/race00.bmp\n"
                "[autotest] SHOT scanout dumped -> autotest/race00_scan.bmp\n"
                "[watchdog] beat=2 frame=160(+60)\n")
        p = prof.parse_text(text)
        self.assertEqual(p.watchdog[1]["shots"], 2)


class VerdictTest(unittest.TestCase):
    def test_fatal_outside_the_slice_is_invisible(self):
        from gdxdev import emu
        out = util.Out(quiet=True)
        mine = ("OutputDebugString: frame 1\nOutputDebugString: frame 2\n"
                "OutputDebugString: frame 3\n")
        r = emu.judge(mine, "", out)
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["fatals"], 0)
        # The same text WITH someone else's crash appended would fail — which is exactly
        # what ci-3ds.sh does by grepping the whole shared log.
        r2 = emu.judge(mine + "[fatal] someone else's crash\n", "", out)
        self.assertEqual(r2["verdict"], "FAIL")

    def test_no_heartbeat_fails(self):
        from gdxdev import emu
        r = emu.judge("OutputDebugString: frame 1\n", "", util.Out(quiet=True))
        self.assertEqual(r["verdict"], "FAIL")
        self.assertIn("heartbeat", r["detail"])


class SdGuardTest(unittest.TestCase):
    def test_writes_are_confined_to_3ds(self):
        from gdxdev import sd
        with tempfile.TemporaryDirectory() as vol:
            self.assertTrue(sd._safe_dest(vol, "3ds/G-Diffuser-3DS.3dsx")
                            .startswith(os.path.join(vol, "3ds")))
            for escape in ("../evil", "3ds/../../evil", "boot.firm", "/etc/hosts"):
                with self.assertRaises(GdxError, msg=escape):
                    sd._safe_dest(vol, escape)


class ReplayResolutionTest(unittest.TestCase):
    def test_committed_replays_all_validate(self):
        d = replay.replays_dir()
        found = [f for f in sorted(os.listdir(d)) if f.endswith(".txt")]
        self.assertTrue(found, "no committed replays under %s" % d)
        for f in found:
            replay.validate_autoinput(os.path.join(d, f))

    def test_resolve_by_id(self):
        path, kind = replay.resolve("storm2")
        self.assertEqual(kind, "autoinput")
        self.assertTrue(path.endswith("storm2.txt"))

    def test_unknown_id_exits_5(self):
        with self.assertRaises(GdxError) as cm:
            replay.resolve("no-such-replay")
        self.assertEqual(cm.exception.code, util.EXIT_STAGING)


class ReplayStagingTest(unittest.TestCase):
    """Staging must be lane-agnostic: `retro run --replay` regressed because retro.py
    declared the flag but never called stage(), and stage() was hardcoded to the shared
    Azahar SD. Reported by the refbuild track, 2026-09-12."""

    def test_autoinput_lands_at_the_given_sd_root(self):
        with tempfile.TemporaryDirectory() as sd:
            block, ini = replay.stage("boot", sdmc_root=sd,
                                      gdx_dir=os.path.join(sd, "3ds", "gdiffuser"))
            dest = os.path.join(sd, "gdx-autoinput.txt")
            self.assertTrue(os.path.isfile(dest), "not staged to the private SD root")
            self.assertEqual(block["kind"], "autoinput")
            self.assertEqual(block["events"], 2)
            # Must actively DISARM a stale .gdxrec rather than leaving it to win.
            self.assertEqual(ini, {("debug", "input_replay"): ""})
            src = os.path.join(replay.replays_dir(), "boot.txt")
            self.assertEqual(util.read_text(dest), util.read_text(src))

    def test_gdxrec_lands_under_gdiffuser_rec(self):
        """The guest opens the ABSOLUTE path <gdx3ds_fs_base_path()>rec/<name>.gdxrec, i.e.
        sdmc:/3ds/gdiffuser/rec/, so staging is cwd-independent. (Earlier the guest used a
        cwd-relative path and staging chased it to the SD root; both sides are now pinned.)
        Reported by integration-verify, 2026-09-13. The previous version of this very test
        asserted the wrong path, which is how the bug survived."""
        with tempfile.TemporaryDirectory() as sd:
            gdx = os.path.join(sd, "3ds", "gdiffuser")
            os.makedirs(gdx)
            rec_src = os.path.join(replay.replays_dir(), "unittest-tmp.gdxrec")
            with open(rec_src, "w") as f:
                f.write("gdxrec 1\n0 0000 0 0\n1 0001 0 0\n")
            try:
                block, ini = replay.stage("unittest-tmp", sdmc_root=sd, gdx_dir=gdx)
            finally:
                os.unlink(rec_src)
            self.assertTrue(os.path.isfile(os.path.join(gdx, "rec", "unittest-tmp.gdxrec")),
                            "must stage at <sd>/3ds/gdiffuser/rec/")
            self.assertFalse(os.path.exists(os.path.join(sd, "rec")),
                             "must NOT stage at the SD root")
            self.assertEqual(block["kind"], "gdxrec")
            # 03-oracle-plan.md:128 — input_replay wins over autoinput.
            self.assertEqual(ini, {("debug", "input_replay"): "unittest-tmp"})

    def test_a_gdxrec_with_shots_arms_the_capture_path(self):
        """SHOT capture is gated on [debug] fh; with fh=0 the guest logs "capture request
        ignored" and the completion check would fail a good run."""
        with tempfile.TemporaryDirectory() as sd:
            src = os.path.join(replay.replays_dir(), "unittest-shots.gdxrec")
            with open(src, "w") as f:
                f.write("gdxrec 1\n0 0000 0 0\n10 0000 0 0 SHOT title\n")
            try:
                block, ini = replay.stage("unittest-shots", sdmc_root=sd)
            finally:
                os.unlink(src)
            self.assertEqual(block["shots"], ["title"])
            self.assertEqual(ini[("debug", "fh")], "1")
            self.assertFalse(os.path.exists(os.path.join(sd, "gdx-autoinput.txt")))

    def test_rec_path_matches_the_guest_source(self):
        """Pin the staging path to the guest that consumes it, so the two cannot drift."""
        src = os.path.join(util.repo_root(), "port", "3ds", "main_3ds.cpp")
        if not os.path.exists(src):
            self.skipTest("port source not present")
        text = util.read_text(src)
        self.assertIn('"%srec/%s.gdxrec"', text,
                      "the guest no longer builds <base>/rec/<name>.gdxrec — re-check rec_dir()")
        self.assertIn('gdx3ds_fs_base_path()', text)
        with tempfile.TemporaryDirectory() as sd:
            self.assertEqual(replay.rec_dir(sd), os.path.join(sd, "3ds", "gdiffuser", "rec"))

    def test_default_root_is_the_azahar_sd(self):
        self.assertEqual(replay.autoinput_path(), util.AUTOINPUT_SD)
        self.assertEqual(replay.rec_dir(), os.path.join(util.AZAHAR_SDMC, "3ds", "gdiffuser", "rec"))


class ReplayReceiptTest(unittest.TestCase):
    """A staged replay that never loads must FAIL the run. Without this, a mis-staged
    replay boots fine, never replays, and reports a confident PASS."""

    def test_open_failure_is_detected(self):
        ok, why = replay.verify_receipts(
            "gdxrec", "[rec] replay open failed: rec/storm2.gdxrec\n")
        self.assertFalse(ok)
        self.assertIn("rejected", why)

    def test_missing_receipt_is_detected(self):
        ok, why = replay.verify_receipts("gdxrec", "[watchdog] beat=1 frame=10(+10)\n")
        self.assertFalse(ok)
        self.assertIn("never loaded", why)

    def test_armed_receipt_passes(self):
        ok, why = replay.verify_receipts(
            "gdxrec", "[rec] replay armed rec/storm2.gdxrec frames=600\n")
        self.assertTrue(ok, why)

    def test_autoinput_receipt(self):
        self.assertTrue(replay.verify_receipts(
            "autoinput", "[input] autoinput script loaded: 2 events (tick timebase)\n")[0])
        ok, why = replay.verify_receipts("autoinput", "nothing here\n")
        self.assertFalse(ok)

    def test_seconds_timebase_fallback_fails(self):
        ok, why = replay.verify_receipts(
            "autoinput", "[input] autoinput script loaded: 1 events (seconds timebase)\n")
        self.assertFalse(ok)
        self.assertIn("62", why)

    def test_no_replay_is_always_ok(self):
        self.assertTrue(replay.verify_receipts(None, "")[0])

    def test_judge_fails_a_run_whose_replay_never_loaded(self):
        from gdxdev import emu
        beats = "".join("OutputDebugString: frame %d\n" % i for i in range(1, 5))
        r = emu.judge(beats, "", util.Out(quiet=True), replay_kind="gdxrec")
        self.assertEqual(r["verdict"], "FAIL")
        r2 = emu.judge(beats, "[rec] replay armed rec/x.gdxrec frames=10\n",
                       util.Out(quiet=True), replay_kind="gdxrec")
        self.assertEqual(r2["verdict"], "PASS")


class BenignReceiptTest(unittest.TestCase):
    def test_atexit_trace_is_not_a_fatal(self):
        """port/3ds/main_3ds.cpp:815 emits this on the NORMAL exit() path, so counting it
        made every clean run FAIL on "1 [fatal] line(s)" (integration-verify, 2026-09-13)."""
        text = ("OutputDebugString: frame 1\nOutputDebugString: frame 2\n"
                "OutputDebugString: frame 3\n"
                "[fatal-trace] atexit: process exiting via exit()\n")
        p = prof.parse_text(text)
        self.assertEqual(p.errors["[fatal-trace]"], 0)
        self.assertEqual(p.benign, 1)
        self.assertEqual(p.fatals, 0)
        from gdxdev import emu
        self.assertEqual(emu.judge(text, "", util.Out(quiet=True))["verdict"], "PASS")

    def test_a_real_fatal_trace_still_counts(self):
        p = prof.parse_text("[fatal-trace] pc=0x001 lr=0x002 in Machine_Draw\n")
        self.assertEqual(p.errors["[fatal-trace]"], 1)
        self.assertEqual(p.fatals, 1)


class RetroGfxTest(unittest.TestCase):
    def test_gfx_is_case_insensitive(self):
        from gdxdev import retro
        self.assertEqual(retro.normalise_gfx("vulkan"), "Vulkan")
        self.assertEqual(retro.normalise_gfx("OpenGL"), "OpenGL")
        self.assertEqual(retro.normalise_gfx("SOFTWARE"), "Software")
        self.assertIsNone(retro.normalise_gfx(None))

    def test_unknown_gfx_is_a_usage_error(self):
        from gdxdev import retro
        with self.assertRaises(GdxError) as cm:
            retro.normalise_gfx("metal")
        self.assertEqual(cm.exception.code, util.EXIT_USAGE)

    def test_crash_signature_names_the_vulkan_worker(self):
        from gdxdev import retro
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("thread VulkanWorker\nReceived signal SIGBUS\n")
            path = f.name
        try:
            sig = retro._crash_signature(path)
        finally:
            os.unlink(path)
        self.assertIn("SIGBUS", sig)
        self.assertIn("VulkanWorker", sig)

    def test_no_guest_log_is_retried_not_blamed_on_the_renderer(self):
        """An empty guest log means the boot died before filelog init — transient under
        load, so it must be retried rather than falling straight through to the next
        renderer (measured: one Vulkan run gave up after a single attempt)."""
        import inspect
        from gdxdev import retro
        src = inspect.getsource(retro._attempt)
        i = src.index("if not sd_log.strip():")
        self.assertIn("return False, True", src[i:i + 600],
                      "an empty guest log must be reported as an early crash")

    def test_retry_count_is_three_attempts(self):
        from gdxdev import retro
        self.assertEqual(retro.EARLY_CRASH_ATTEMPTS, 3)   # initial + 2 retries

    def test_retro_run_stages_the_replay(self):
        """retro.do_run must call stage() with the instance's private paths."""
        import inspect
        from gdxdev import retro
        src = inspect.getsource(retro.do_run)
        self.assertIn("replay.stage(", src,
                      "retro run accepts --replay but never stages it")
        self.assertIn("sdmc_root=inst.sdmc()", src)
        self.assertIn("gdx_dir=inst.gdx_dir()", src)
        self.assertIn("replay=replay_block", inspect.getsource(retro.do_run),
                      "the manifest must record the staged replay, not null")


class _OnlyPopenExplodes(object):
    """A stand-in for the `subprocess` module whose Popen detonates and whose everything
    else is the real thing."""

    def __init__(self, real, boom):
        self._real = real
        self.Popen = boom

    def __getattr__(self, name):
        return getattr(self._real, name)


class RetroDryRunTest(unittest.TestCase):
    """NEXT.md B45. `--dry-run` was parsed by `_common()`, documented in `--help`, and
    read by NOBODY in `retro run`: the flag that exists to be safe booted a real
    RetroArch instance on a real private SD. These tests detonate on every side effect a
    real run has, so a dry run that reaches any of them fails here instead of on the host.
    """

    SIDE_EFFECTS = ("ensure_core", "ensure_core_3d", "acquire_instance", "seed_sd",
                    "write_config", "_attempt", "suppress_restore_prompt",
                    "wait_for_host", "send_command", "kill_group", "strip_run_control")

    def setUp(self):
        from gdxdev import retro
        self.retro = retro
        self.tmp = tempfile.mkdtemp(prefix="gdx-dry-")
        self.dsx = os.path.join(self.tmp, "G-Diffuser-3DS.3dsx")
        with open(self.dsx, "wb") as f:
            f.write(b"3DSX\x00fake")
        self.out = util.Out(quiet=True)
        self.tripped = []
        self._saved = {}
        for name in self.SIDE_EFFECTS:
            self._saved[name] = getattr(retro, name)
            setattr(retro, name, self._detonator(name))
        # and the launch itself, one level below everything above. Only `retro`'s own
        # view of the module is swapped: patching subprocess.Popen globally would also
        # detonate on the `git` calls the manifest's tree block makes.
        self._popen = retro.subprocess
        retro.subprocess = _OnlyPopenExplodes(retro.subprocess,
                                              self._detonator("subprocess.Popen"))
        # runs.create() would leave a directory behind: "change nothing" includes that
        self._create = retro.runs.create
        retro.runs.create = self._detonator("runs.create")

    def _detonator(self, name):
        def boom(*a, **kw):
            self.tripped.append(name)
            raise AssertionError("--dry-run called %s()" % name)
        return boom

    def tearDown(self):
        for name, fn in self._saved.items():
            setattr(self.retro, name, fn)
        self.retro.subprocess = self._popen
        self.retro.runs.create = self._create
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _args(self, *extra):
        from gdxdev import cli
        return cli.build_parser().parse_args(
            ["retro", "run", self.dsx, "--dry-run", "--quiet"] + list(extra))

    def _ctx(self):
        return {"root": util.repo_root(), "track": "dryrun-unit",
                "run_root": os.path.join(self.tmp, "runs")}

    def test_a_dry_run_launches_nothing(self):
        code = self.retro.do_run(self._args(), self.out, self._ctx())
        self.assertEqual(code, util.EXIT_OK)
        self.assertEqual(self.tripped, [])

    def test_a_dry_run_with_a_replay_launches_nothing(self):
        code = self.retro.do_run(self._args("--replay", "race-mutecity-start"),
                                 self.out, self._ctx())
        self.assertEqual(code, util.EXIT_OK)
        self.assertEqual(self.tripped, [])

    def test_a_dry_run_writes_no_run_directory(self):
        self.retro.do_run(self._args(), self.out, self._ctx())
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "runs")))

    def test_a_dry_run_stages_no_replay_file(self):
        """replay.stage(dry_run=True) still VALIDATES and reports; it must not copy."""
        from gdxdev import replay as rp
        inst = self.retro.Instance("dryrun-unit")
        self.retro.do_run(self._args("--replay", "race-mutecity-start"),
                          self.out, self._ctx())
        self.assertFalse(os.path.exists(rp.rec_dir(inst.sdmc())))

    def test_a_dry_run_reports_what_it_would_launch(self):
        out = util.Out(quiet=False, as_json=True)
        buf = io.StringIO()
        saved, sys.stdout = sys.stdout, buf
        try:
            self.retro.do_run(self._args("--replay", "race-mutecity-start",
                                         "--gfx", "vulkan"), out, self._ctx())
        finally:
            sys.stdout = saved
        text = buf.getvalue()
        emitted = json.loads(text[text.index("{"):])
        self.assertTrue(emitted["dry_run"])
        self.assertIn("retroarch", emitted["cmd"][0].lower())
        self.assertIn(self.dsx, emitted["cmd"])
        self.assertEqual(emitted["gfx"], ["Vulkan"])
        self.assertEqual(emitted["replay"]["id"], "race-mutecity-start")
        self.assertTrue(emitted["tree"]["branch_point"])

    def test_a_real_run_is_not_short_circuited(self):
        """The guard against 'fixed --dry-run by making every run a dry run'."""
        from gdxdev import cli
        args = cli.build_parser().parse_args(["retro", "run", self.dsx, "--quiet"])
        # A real run refuses up front when RetroArch is missing (any non-macOS host, CI
        # included); point it at a file that exists so the detonators are what stop it.
        saved_bin = self.retro.RETRO_BIN
        self.retro.RETRO_BIN = sys.executable
        try:
            with self.assertRaises(AssertionError):
                self.retro.do_run(args, self.out, self._ctx())
        finally:
            self.retro.RETRO_BIN = saved_bin
        self.assertTrue(self.tripped, "a real run must still reach the launch path")


class RetroSecondsTest(unittest.TestCase):
    """NEXT.md B45, second half: `--seconds 60` truncated every corpus replay."""

    def _args(self, *extra):
        from gdxdev import cli
        return cli.build_parser().parse_args(["retro", "run", "x.3dsx"] + list(extra))

    def test_the_parser_no_longer_hardcodes_sixty(self):
        self.assertIsNone(self._args().seconds,
                          "a fixed --seconds default hides which value a run chose")

    def test_a_bare_boot_keeps_the_short_default(self):
        from gdxdev import retro
        self.assertEqual(retro.resolve_seconds(self._args(), None)[0],
                         retro.DEFAULT_SECONDS)

    def test_a_staged_replay_gets_the_long_default(self):
        from gdxdev import retro
        secs, why = retro.resolve_seconds(self._args(), {"id": "race-mutecity-start"})
        self.assertEqual(secs, retro.REPLAY_SECONDS)
        self.assertGreaterEqual(secs, 240)      # residuals2-progress.md measured 240-420 s
        self.assertIn("B45", why)

    def test_an_explicit_seconds_always_wins(self):
        from gdxdev import retro
        self.assertEqual(retro.resolve_seconds(self._args("--seconds", "5"),
                                               {"id": "x"})[0], 5)


class TimeoutVerdictTest(unittest.TestCase):
    """NEXT.md B45: a run cut short by --seconds was reported exactly like a crash."""

    LOG = ("[rec] replay armed\n"
           "[watchdog] beat=1 frame=60(+60)\n"
           "[watchdog] beat=2 frame=120(+60)\n")
    BLOCK = {"kind": "gdxrec", "shots": [], "last_tick": 6000, "schedule": []}

    def _judge(self, **kw):
        from gdxdev import emu
        return emu.judge("", self.LOG, util.Out(quiet=True), replay_kind="gdxrec",
                         replay_block=self.BLOCK, **kw)

    def test_the_clock_running_out_is_a_timeout_not_a_failure(self):
        r = self._judge(emu_alive_at_end=True, deadline_hit=True, seconds=60)
        self.assertEqual(r["verdict"], "TIMEOUT")
        self.assertTrue(r["timed_out"])
        self.assertIn("cut short", r["detail"])
        self.assertIn("NOT crashed", r["detail"])
        self.assertIn("6000", r["detail"])      # names the tick it was short of

    def test_a_short_replay_that_completed_on_one_beat_passes(self):
        """probeboot (420 ticks) quits via replay_quit before the second ~300-frame
        watchdog beat; one beat is not a stalled heartbeat when the replay finished."""
        from gdxdev import emu
        log = ("[rec] replay armed\n[watchdog] beat=1 frame=247(+247)\n"
               "[rec] end tick=420 lines=420\n"
               "[oracle] quit requested: [rec] end with replay_quit=1\n")
        block = {"kind": "gdxrec", "shots": [], "last_tick": 419, "schedule": []}
        r = emu.judge("", log, util.Out(quiet=True), replay_kind="gdxrec",
                      replay_block=block, emu_alive_at_end=False, deadline_hit=False)
        self.assertEqual(r["verdict"], "PASS", r["detail"])
        r = emu.judge("", "[rec] replay armed\n[watchdog] beat=1 frame=247(+247)\n",
                      util.Out(quiet=True), replay_kind="gdxrec", replay_block=block,
                      emu_alive_at_end=False, deadline_hit=False)
        self.assertEqual(r["verdict"], "FAIL")

    def test_a_dead_core_is_still_a_failure(self):
        r = self._judge(emu_alive_at_end=False, deadline_hit=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertFalse(r["timed_out"])
        self.assertIn("core died", r["detail"])

    def test_a_fatal_outranks_the_clock(self):
        """A run that crashed AND hit the deadline is a crash, not a timeout."""
        from gdxdev import emu
        r = emu.judge("", self.LOG + "[fatal] assert in gfx\n", util.Out(quiet=True),
                      replay_kind="gdxrec", replay_block=self.BLOCK,
                      emu_alive_at_end=True, deadline_hit=True, seconds=60)
        self.assertEqual(r["verdict"], "FAIL")

    def test_timeout_does_not_exit_zero(self):
        import inspect
        from gdxdev import retro
        self.assertIn('result["verdict"] == "PASS"', inspect.getsource(retro.do_run))

    def test_the_result_line_says_the_word(self):
        from gdxdev import emu
        r = self._judge(emu_alive_at_end=True, deadline_hit=True, seconds=60)
        line = emu.result_line({"build": {}, "replay": {"id": "r", "kind": "gdxrec"},
                                "wall_s": 60}, r)
        self.assertTrue(line.startswith("RESULT TIMEOUT"), line)

    def test_the_attempt_records_why_it_ended(self):
        import inspect
        from gdxdev import retro
        src = inspect.getsource(retro._attempt)
        self.assertIn('_LAST_ATTEMPT["deadline_hit"]', src)
        self.assertIn("deadline_hit = rc is None and not guest_done", src)


class ImpliedReplayQuitTest(unittest.TestCase):
    """NEXT.md B19: `--replay` alone never ENDED a run — the guest played on, unscripted,
    until --seconds expired."""

    KEY = ("debug", "replay_quit")

    def _args(self, *extra):
        from gdxdev import cli
        return cli.build_parser().parse_args(["retro", "run", "x.3dsx"] + list(extra))

    def test_a_staged_replay_implies_replay_quit(self):
        settings, explicit = {}, set()
        replay.apply_implied(settings, explicit, kind="gdxrec")
        self.assertEqual(settings[self.KEY], "1")

    def test_an_explicit_ini_wins(self):
        settings = {self.KEY: "0"}
        replay.apply_implied(settings, {self.KEY}, kind="gdxrec")
        self.assertEqual(settings[self.KEY], "0",
                         "the wobble and dynlod runs need to play past the replay end")

    def test_the_override_flag_exists_and_is_explicit(self):
        from gdxdev import retro
        settings, explicit = retro.build_settings(self._args("--no-replay-quit"))
        self.assertEqual(settings[self.KEY], "0")
        self.assertIn(self.KEY, explicit)
        replay.apply_implied(settings, explicit, kind="gdxrec")
        self.assertEqual(settings[self.KEY], "0")

    def test_the_ini_spelling_of_the_override_also_wins(self):
        from gdxdev import retro
        settings, explicit = retro.build_settings(
            self._args("--ini", "debug.replay_quit=0"))
        replay.apply_implied(settings, explicit, kind="gdxrec")
        self.assertEqual(settings[self.KEY], "0")

    QUIT_AT = ("debug", "quit_at_tick")

    def test_autoinput_on_a_private_sd_implies_quit_at_tick(self):
        """replay_quit only acts on a .gdxrec's [rec] end; a .txt script never logs one,
        so without this storm2 & co. always ran to the --seconds kill."""
        settings = {}
        replay.apply_implied(settings, set(), kind="autoinput", last=21000, private_sd=True)
        self.assertEqual(settings[self.QUIT_AT],
                         str(21000 + replay.AUTOINPUT_QUIT_MARGIN))

    def test_quit_at_tick_is_not_implied_for_gdxrec_or_the_shared_sd(self):
        for kind, private in (("gdxrec", True), ("autoinput", False)):
            settings = {}
            replay.apply_implied(settings, set(), kind=kind, last=6000, private_sd=private)
            self.assertNotIn(self.QUIT_AT, settings, (kind, private))

    def test_quit_at_tick_respects_the_play_past_override_and_an_explicit_value(self):
        settings = {self.KEY: "0"}
        replay.apply_implied(settings, {self.KEY}, kind="autoinput", last=900,
                             private_sd=True)
        self.assertNotIn(self.QUIT_AT, settings)
        settings = {self.QUIT_AT: "500"}
        replay.apply_implied(settings, {self.QUIT_AT}, kind="autoinput", last=900,
                             private_sd=True)
        self.assertEqual(settings[self.QUIT_AT], "500")

    def test_both_lanes_imply_it(self):
        import inspect
        from gdxdev import emu, retro
        for fn in (emu.do_run, retro.do_run):
            self.assertIn("replay.apply_implied(", inspect.getsource(fn),
                          "%s stages a replay without implying replay_quit" % fn)

    def test_the_override_is_reachable_from_both_lanes(self):
        from gdxdev import cli
        ap = cli.build_parser()
        self.assertTrue(ap.parse_args(["retro", "run", "x", "--no-replay-quit"])
                        .no_replay_quit)
        self.assertTrue(ap.parse_args(["emu", "run", "x", "--no-replay-quit"])
                        .no_replay_quit)

    def test_the_clean_exit_it_causes_is_not_a_fatal(self):
        """replay_quit leaves through exit(), which logs `[fatal-trace] atexit:`. If that
        counted as a fatal, implying replay_quit would fail every run that obeyed it
        (residuals-progress.md, STATUS.md item 7)."""
        census = prof.error_census("[fatal-trace] atexit: process exiting via exit()\n")
        self.assertEqual(census["fatals"], 0)


class TreeClaimTest(unittest.TestCase):
    """NEXT.md B46 / PROTOCOL.md: a parity cell is a claim about a tree. A long-lived
    branch that does not name its branch point cannot be checked against main from where
    it stands — which cost one agent a full bisect of B42, a bug already fixed on main."""

    def _repo(self, d):
        def g(*a):
            import subprocess
            return subprocess.run(["git"] + list(a), cwd=d, text=True,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL).stdout
        g("init", "-q", "-b", "main")
        g("config", "user.email", "t@t"); g("config", "user.name", "t")
        open(os.path.join(d, "f"), "w").write("1")
        g("add", "f"); g("commit", "-qm", "one")
        return g

    def test_a_current_branch_reports_no_drift(self):
        from gdxdev import build as buildmod
        with tempfile.TemporaryDirectory() as d:
            g = self._repo(d)
            g("checkout", "-qb", "work")
            t = buildmod.tree_info(d, base="main")
            self.assertEqual(t["behind"], 0)
            self.assertTrue(t["current"])
            self.assertIsNone(buildmod.tree_warning(t))

    def test_a_branch_behind_main_is_named_as_such(self):
        from gdxdev import build as buildmod
        with tempfile.TemporaryDirectory() as d:
            g = self._repo(d)
            point = g("rev-parse", "HEAD").strip()
            g("checkout", "-qb", "work")
            open(os.path.join(d, "w"), "w").write("w")
            g("add", "w"); g("commit", "-qm", "work")
            g("checkout", "-q", "main")
            open(os.path.join(d, "m"), "w").write("m")
            g("add", "m"); g("commit", "-qm", "main moves on")
            g("checkout", "-q", "work")
            t = buildmod.tree_info(d, base="main")
            self.assertEqual(t["branch"], "work")
            self.assertEqual(t["branch_point"], point)
            self.assertEqual(t["behind"], 1)
            self.assertEqual(t["ahead"], 1)
            self.assertFalse(t["current"])
            warn = buildmod.tree_warning(t)
            self.assertIn("behind", warn)
            self.assertIn("pre-existing", warn)

    def test_the_receipt_carries_the_branch_point(self):
        from gdxdev import build as buildmod
        line = buildmod.tree_receipt({"branch": "native/x", "head": "a" * 40,
                                      "base": "native/main", "base_head": "b" * 40,
                                      "branch_point": "c" * 40, "behind": 2, "ahead": 5,
                                      "dirty": True, "current": False})
        self.assertIn("native/x@aaaaaaaa", line)
        self.assertIn("branch point cccccccc", line)
        self.assertIn("native/main@bbbbbbbb", line)
        self.assertIn("DIRTY", line)

    def test_every_lane_writes_one(self):
        import inspect
        from gdxdev import emu, hwrun, retro
        for fn in (emu.do_run, retro.do_run, hwrun._run_slice,
                   hwrun._write_matrix_manifest):
            self.assertIn("tree=buildmod.tree_info(", inspect.getsource(fn),
                          "%s writes a manifest with no branch point" % fn)

    def test_this_very_worktree_resolves(self):
        from gdxdev import build as buildmod
        t = buildmod.tree_info(util.repo_root())
        self.assertTrue(t["branch_point"], "the live worktree must name its branch point")
        self.assertEqual(runs._tree_problems(t), [])


class Stereo3dTest(unittest.TestCase):
    """`--3d`: the host knob that makes the guest's right-eye pass run at all.

    The invariant most of these guard is "nothing changes when off" — the whole parity
    corpus was recorded without this flag.
    """

    def test_qt_enum_is_azahars_not_citras(self):
        """Azahar inserted SideBySideFull at 2, pushing Anaglyph to 3 (settings.h:86).

        An enum carried over from a Citra-era note would select the wrong mode, which
        looks like "3D did not work" rather than like a bug."""
        from gdxdev import stereo3d
        self.assertEqual(stereo3d.MODES["off"][0], 0)
        self.assertEqual(stereo3d.MODES["side-by-side"][0], 1)
        self.assertEqual(stereo3d.MODES["side-by-side-full"][0], 2)
        self.assertEqual(stereo3d.MODES["anaglyph"][0], 3)
        self.assertEqual(stereo3d.MODES["interlaced"][0], 4)
        self.assertEqual(stereo3d.MODES["reverse-interlaced"][0], 5)

    def test_depth_zero_is_refused(self):
        """factor_3d 0 leaves the slider at 0, i.e. exactly the state --3d exists to fix."""
        from gdxdev import stereo3d
        for bad in (0, -1, 101, "x"):
            with self.assertRaises(GdxError):
                stereo3d.depth_or_die(bad)
        self.assertEqual(stereo3d.depth_or_die(100), 100)

    def test_unknown_mode_is_a_usage_error(self):
        from gdxdev import stereo3d
        with self.assertRaises(GdxError) as cm:
            stereo3d.mode_or_die("sidebyside")
        self.assertEqual(cm.exception.code, util.EXIT_USAGE)

    def test_3d_implies_stereo_and_both_eyes(self):
        from gdxdev import stereo3d
        s = stereo3d.guest_settings()
        self.assertEqual(s[("stereo", "enabled")], "1")
        self.assertEqual(s[("debug", "fh_eye")], "both",
                         "without fh_eye=both there is no eye=R stream to compare")

    def test_qt_config_patch_writes_the_default_sibling_and_restores(self):
        """QtConfig::ReadBasicSetting throws the stored value away when <key>\\default is
        true, so render_3d=1 on its own is inert. And qt-config.ini is the user's own
        preference file: the run must hand it back byte for byte."""
        from gdxdev import stereo3d
        d = tempfile.mkdtemp()
        try:
            path = os.path.join(d, "qt-config.ini")
            original = ("[UI]\ntheme=dark\n\n[Layout]\nfactor_3d=100\n"
                        "factor_3d\\default=false\nrender_3d=0\nrender_3d\\default=true\n"
                        "swap_eyes_3d=false\n\n[Renderer]\nrenderer_debug=false\n")
            with open(path, "w") as f:
                f.write(original)

            class _Out:
                def say(self, *a, **k):
                    pass

            with stereo3d.QtStereo("anaglyph", 40, _Out(), path=path):
                got = util.read_text(path)
                self.assertIn("render_3d=3", got)
                self.assertIn("render_3d\\default=false", got)
                self.assertIn("factor_3d=40", got)
                self.assertIn("factor_3d\\default=false", got)
                self.assertIn("[Renderer]", got)
                self.assertIn("swap_eyes_3d=false", got)
            self.assertEqual(util.read_text(path), original)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_core_options_redirect_applies_to_a_plain_mono_run(self):
        """emufix (2026-10-16): a plain run (no --3d, no --model) used to write NO
        redirect, so it read/wrote the SHARED Azahar.opt like every other concurrent
        instance -- a torn or half-written shared file handed to a booting core was the
        mono lane's ~50% early-crash rate. The redirect, and the private per-instance
        options file, must now land for every run, with values the core actually
        declares (not the old illegal "1x (Native)" / "Default Top-Bottom Screen" /
        "enabled" guesses, which silently fell back to the option default)."""
        from gdxdev import retro
        d = tempfile.mkdtemp()
        try:
            inst = retro.Instance("t3d")
            inst.dir = d
            retro.write_config(inst, gfx="Vulkan", core="/tmp/none.dylib")
            self.assertIn("rgui_config_directory", util.read_text(inst.cfg),
                          "mono runs must get their own private core-options file, "
                          "exactly like --3d, or they fall back to the shared one")
            opt_path = os.path.join(d, "config", "Azahar", "Azahar.opt")
            self.assertTrue(os.path.exists(opt_path),
                            "the file RetroArch actually reads must exist for a plain run")
            opt = util.read_text(opt_path)
            self.assertIn('citra_is_new_3ds = "New 3DS"', opt,
                          "the old mono value, \"enabled\", is not legal and silently "
                          "fell back to the option default")
            self.assertIn('citra_resolution_factor = "1"', opt,
                          "\"1x (Native)\" is not a legal value for this option")
            self.assertIn('citra_layout_option = "default"', opt,
                          "\"Default Top-Bottom Screen\" is not a legal value for this option")
            self.assertIn('citra_graphics_api = "Vulkan"', opt)
            # Both files get the SAME text -- one template, written to both places, not
            # two near-duplicate ones that can drift apart.
            self.assertEqual(util.read_text(inst.core_options), opt)

            retro.write_config(inst, gfx="Vulkan", core="/tmp/none.dylib",
                               three_d=("side-by-side", 100))
            # The file RetroArch actually reads (core_options_path is inert; see
            # CFG_3D_EXTRA). Both halves have to be there or the option never lands.
            opt = util.read_text(opt_path)
            self.assertIn('citra_render_3d = "side-by-side"', opt)
            self.assertIn('citra_factor_3d = "100"', opt)
            self.assertIn('citra_is_new_3ds = "New 3DS"', opt,
                          "under --3d the file IS read, so the value must be a legal one")
            self.assertEqual(len([l for l in opt.splitlines()
                                  if l.startswith("citra_is_new_3ds")]), 1)
            self.assertIn("rgui_config_directory", util.read_text(inst.cfg))
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_core_options_never_touch_the_shared_file(self):
        """The fix must redirect every run's options into ITS OWN instance directory,
        never into ~/Library/Application Support/RetroArch/config/Azahar/Azahar.opt --
        the file the user's own Azahar Qt lane and every OTHER concurrent instance share."""
        from gdxdev import retro
        d = tempfile.mkdtemp()
        try:
            inst = retro.Instance("tshared")
            inst.dir = d
            retro.write_config(inst, gfx="Vulkan", core="/tmp/none.dylib")
            cfg_text = util.read_text(inst.cfg)
            m = [l for l in cfg_text.splitlines() if l.startswith("rgui_config_directory")]
            self.assertEqual(len(m), 1)
            self.assertIn(d, m[0], "the redirect must point inside THIS instance's dir")
            self.assertNotIn("RetroArch/config/Azahar", m[0])
        finally:
            shutil.rmtree(d, ignore_errors=True)


class RetroModelTest(unittest.TestCase):
    """`--model old`: the Old 3DS lane (NEXT.md C4).

    Three properties, each of which has already burned somebody on this lane:
    (a) with no --model the EFFECTIVE model is unchanged -- still New 3DS, the core's own
    compiled default, now written explicitly rather than left unset (emufix, 2026-10-16:
    the redirect itself is written on every run, mono included, to stop the lane sharing
    ~/Library/Application Support/RetroArch/config/Azahar/Azahar.opt across concurrent
    instances); (b) the value written is one the CORE DECLARES ("Old 3DS", not "disabled"),
    because an illegal value silently falls back to the option default and the run would
    quietly be a New 3DS run wearing an Old 3DS label; (c) the manifest's platform.model
    follows the flag, since that field is what any later comparison reads.
    """

    def test_no_model_still_writes_the_redirect_with_the_default(self):
        from gdxdev import retro
        d = tempfile.mkdtemp()
        try:
            inst = retro.Instance("tmodel")
            inst.dir = d
            retro.write_config(inst, gfx="Vulkan", core="/tmp/none.dylib")
            self.assertIn("rgui_config_directory", util.read_text(inst.cfg))
            opt = util.read_text(os.path.join(d, "config", "Azahar", "Azahar.opt"))
            self.assertIn('citra_is_new_3ds = "New 3DS"', opt,
                          "no --model must still PIN the core's own default explicitly, "
                          "not merely rely on it, now that the file is read")
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_model_old_pins_the_core_option(self):
        from gdxdev import retro
        d = tempfile.mkdtemp()
        try:
            inst = retro.Instance("tmodel")
            inst.dir = d
            retro.write_config(inst, gfx="Vulkan", core="/tmp/none.dylib", model="old")
            self.assertIn("rgui_config_directory", util.read_text(inst.cfg))
            opt = util.read_text(os.path.join(d, "config", "Azahar", "Azahar.opt"))
            self.assertIn('citra_is_new_3ds = "Old 3DS"', opt)
            # RetroArch's config parser returns the FIRST entry for a duplicated key, so
            # the option must appear EXACTLY once. Appending a second assignment wrote a
            # file that said "Old 3DS" and booted a New 3DS; only the guest's own
            # [mem-budget] receipt caught it.
            self.assertEqual(len([l for l in opt.splitlines()
                                  if l.startswith("citra_is_new_3ds")]), 1)
            self.assertNotIn('citra_is_new_3ds = "New 3DS"', opt)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_model_and_3d_compose(self):
        from gdxdev import retro
        d = tempfile.mkdtemp()
        try:
            inst = retro.Instance("tmodel")
            inst.dir = d
            retro.write_config(inst, gfx="Vulkan", core="/tmp/none.dylib", model="old",
                               three_d=("side-by-side", 100))
            opt = util.read_text(os.path.join(d, "config", "Azahar", "Azahar.opt"))
            self.assertIn('citra_is_new_3ds = "Old 3DS"', opt)
            self.assertIn('citra_render_3d = "side-by-side"', opt)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_unknown_model_is_a_usage_error(self):
        from gdxdev import retro
        from gdxdev.util import GdxError
        self.assertIsNone(retro.model_or_die(None))
        self.assertEqual(retro.model_or_die("OLD"), "old")
        with self.assertRaises(GdxError):
            retro.model_or_die("old3ds")


class FhDiffEyeTest(unittest.TestCase):
    """`fhdiff --eye both`: one capture writes an eye=L and an eye=R receipt sharing a
    frame number, so the two streams must be compared separately, never against each
    other."""

    TOOL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "fhdiff.py")

    BOTH = ("[fh] frame=10 tick=9 eye=L w=400 h=240 hash=aaaa1111 tag=auto mode=1\n"
            "[fh] frame=10 tick=9 eye=R w=400 h=240 hash=bbbb2222 tag=auto mode=1\n"
            "[fh] frame=11 tick=10 eye=L w=400 h=240 hash=cccc3333 tag=auto mode=1\n"
            "[fh] frame=11 tick=10 eye=R w=400 h=240 hash=dddd4444 tag=auto mode=1\n")

    def _run(self, a_text, b_text, *extra):
        import subprocess
        d = tempfile.mkdtemp()
        try:
            pa, pb = os.path.join(d, "a.log"), os.path.join(d, "b.log")
            for p, t in ((pa, a_text), (pb, b_text)):
                with open(p, "w") as f:
                    f.write(t)
            p = subprocess.run([sys.executable, self.TOOL, pa, pb] + list(extra),
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            return p.returncode, p.stdout
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_identical_two_eye_logs_pass(self):
        rc, outp = self._run(self.BOTH, self.BOTH, "--eye", "both")
        self.assertEqual(rc, 0, outp)
        self.assertIn("eyes=L,R status=PASS", outp)
        self.assertIn("[eye=L] compared=2 divergent=0", outp)
        self.assertIn("[eye=R] compared=2 divergent=0", outp)

    def test_a_right_eye_only_divergence_fails(self):
        """The whole point: a lever that breaks only the right eye is invisible in mono."""
        rc, outp = self._run(self.BOTH, self.BOTH.replace("dddd4444", "dddd9999"),
                             "--eye", "both")
        self.assertEqual(rc, 1, outp)
        self.assertIn("[eye=L] compared=2 divergent=0", outp)
        self.assertIn("[eye=R] compared=2 divergent=1", outp)
        self.assertIn("status=FAIL", outp)

    def test_missing_right_eye_says_the_pass_did_not_run(self):
        mono = "".join(l for l in self.BOTH.splitlines(True) if "eye=R" not in l)
        rc, outp = self._run(self.BOTH, mono, "--eye", "both")
        self.assertEqual(rc, 2, outp)
        self.assertIn("right-eye pass did not run", outp)

    def test_eyes_are_never_matched_against_each_other(self):
        """L-vs-R would report divergence on a perfectly good stereo pair."""
        rc, outp = self._run(self.BOTH, self.BOTH, "--eye", "both")
        self.assertEqual(rc, 0, outp)
        self.assertNotIn("DIFF", outp)


class RetroInstanceLockTest(unittest.TestCase):
    """Orphan RetroArch processes shared private SDs with live runs (reported by the
    coordinator, 2026-09-13: 9 orphans, ppid 1, several on instance names a live run was
    using). These cover the four guards that make that impossible."""

    def setUp(self):
        from gdxdev import retro
        self.retro = retro
        self.tmp = tempfile.mkdtemp()
        self._root = retro.INSTANCE_ROOT
        retro.INSTANCE_ROOT = self.tmp

    def tearDown(self):
        self.retro.INSTANCE_ROOT = self._root
        del self.retro._ACTIVE[:]
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _inst(self, name="t"):
        return self.retro.Instance(name)

    def test_acquire_then_second_acquire_is_refused_with_exit_4(self):
        inst = self._inst()
        out = util.Out(quiet=True)
        self.retro.acquire_instance(inst, {"track": "devcli"}, "/tmp/run1", out)
        self.assertTrue(os.path.isfile(inst.lock))
        # A second driver (this process is alive, so the lock is live) must be refused.
        with self.assertRaises(GdxError) as cm:
            self.retro.acquire_instance(inst, {"track": "other"}, "/tmp/run2", out)
        self.assertEqual(cm.exception.code, util.EXIT_LOCK)
        msg = str(cm.exception)
        self.assertIn(str(os.getpid()), msg, "the refusal must name the owning pid")
        self.assertIn("/tmp/run1", msg, "the refusal must name the owning run dir")

    def test_stale_lock_from_a_dead_driver_is_cleared(self):
        inst = self._inst()
        os.makedirs(inst.dir, exist_ok=True)
        with open(inst.lock, "w") as f:
            json.dump({"driver_pid": 999999, "retro_pid": None,
                       "instance": inst.name, "run_dir": "/tmp/old"}, f)
        self.retro.acquire_instance(inst, {"track": "devcli"}, "/tmp/new", util.Out(quiet=True))
        self.assertEqual(self.retro.read_lock(inst)["driver_pid"], os.getpid())

    def test_release_removes_the_lock(self):
        inst = self._inst()
        self.retro.acquire_instance(inst, {}, "/tmp/r", util.Out(quiet=True))
        self.retro.release_instance(inst)
        self.assertFalse(os.path.exists(inst.lock))

    def test_set_child_records_the_pid_for_reap(self):
        inst = self._inst()
        self.retro.acquire_instance(inst, {}, "/tmp/r", util.Out(quiet=True))
        self.retro.set_child(inst, 4242)
        self.assertEqual(self.retro.read_lock(inst)["retro_pid"], 4242)

    def test_kill_group_refuses_to_kill_the_driver_itself(self):
        """Caught a real self-kill: the 'refuse our own group' branch fell through to
        os.kill(pid), so kill_group(os.getpid()) SIGTERMed the driver. The suicide guard
        must return False and leave us running."""
        self.assertFalse(self.retro.kill_group(os.getpid(), grace=0.2))
        self.assertTrue(util.pid_alive(os.getpid()), "kill_group killed its own driver")

    def test_kill_group_kills_a_real_child_in_its_own_session(self):
        import subprocess
        child = subprocess.Popen(["sleep", "60"], start_new_session=True)
        self.assertNotEqual(os.getpgid(child.pid), os.getpgid(0))
        self.assertTrue(self.retro.kill_group(child.pid, grace=2.0))
        child.wait()
        self.assertFalse(util.pid_alive(child.pid))

    def test_a_zombie_counts_as_gone(self):
        import subprocess
        child = subprocess.Popen(["true"], start_new_session=True)
        while util.pid_alive(child.pid) and not self.retro._gone(child.pid):
            time.sleep(0.05)
        self.assertTrue(self.retro._gone(child.pid),
                        "a terminated-but-unreaped child must read as gone")
        child.wait()

    def test_kill_group_on_a_dead_pid_is_a_noop(self):
        self.assertFalse(self.retro.kill_group(999999))
        self.assertFalse(self.retro.kill_group(None))

    def test_instance_name_parsed_from_the_cfg_argument(self):
        cmd = ("/Applications/RetroArch.app/Contents/MacOS/RetroArch -c "
               "/Users/x/Library/Application Support/gdx-retro/r3/retroarch.cfg -L core.dylib")
        m = self.retro._CFG_RE.search(cmd)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(2), "r3")


class RetroReapClassificationTest(unittest.TestCase):
    """Which processes `reap` kills. The spare-cases matter more than the kill-case: this
    machine runs several tracks at once, and a wrong kill destroys someone's run."""

    def _classify(self, procs, locks):
        from gdxdev import retro
        calls = {"killed": []}
        real_procs, real_lock, real_names, real_kill = (
            retro.our_retro_processes, retro.read_lock,
            retro.instance_names, retro.kill_group)
        retro.our_retro_processes = lambda: procs
        retro.instance_names = lambda: list(locks)
        retro.read_lock = lambda inst: locks.get(inst.name)
        retro.kill_group = lambda pid, **kw: calls["killed"].append(pid) or True
        try:
            args = type("A", (), {"dry_run": False})()
            retro.do_reap(args, util.Out(quiet=True), {"track": "devcli"})
        finally:
            retro.our_retro_processes, retro.read_lock = real_procs, real_lock
            retro.instance_names, retro.kill_group = real_names, real_kill
        return calls["killed"]

    def test_reparented_to_init_is_killed(self):
        procs = [{"pid": 111, "ppid": 1, "etime": "50:00", "instance": "r1"}]
        self.assertEqual(self._classify(procs, {}), [111])

    def test_lock_naming_a_dead_driver_is_killed(self):
        procs = [{"pid": 222, "ppid": 5, "etime": "10:00", "instance": "r2"}]
        locks = {"r2": {"driver_pid": 999999, "retro_pid": 222}}
        self.assertEqual(self._classify(procs, locks), [222])

    def test_live_driver_is_spared(self):
        procs = [{"pid": 333, "ppid": 5, "etime": "01:00", "instance": "r3"}]
        locks = {"r3": {"driver_pid": os.getpid(), "retro_pid": 333}}
        self.assertEqual(self._classify(procs, locks), [])

    def test_no_lock_but_live_parent_is_spared(self):
        """The regression that would have killed other tracks: processes started before
        this fix have no lock file and perfectly live drivers. Absence of a lock is NOT
        evidence of an orphan."""
        procs = [{"pid": 444, "ppid": 5, "etime": "04:00", "instance": "fx2"}]
        self.assertEqual(self._classify(procs, {}), [])


class RetroDefaultInstanceTest(unittest.TestCase):
    def test_run_defaults_the_instance_to_the_unique_run_id(self):
        import inspect
        from gdxdev import cli, retro
        src = inspect.getsource(retro.do_run)
        self.assertIn('getattr(args, "instance", None) or rd.run_id', src)
        self.assertIn("acquire_instance(inst", src)
        # and the parser must not supply a fixed default that hides it
        ap = cli.build_parser()
        args = ap.parse_args(["retro", "run", "x.3dsx"])
        self.assertIsNone(args.instance,
                          "a fixed --instance default lets two agents share a private SD")


class SeedIniHygieneTest(unittest.TestCase):
    """A fresh instance must not inherit another agent's experiment. Measured: a stability
    sweep inherited `input_replay` from the shared Qt SD and every boot logged
    "[rec] replay open failed" for a replay it was never given."""

    SHARED = ("[debug]\nconsole = 1\ninput_replay = someones-run\nreplay_quit = 1\n"
              "fh_every = 1\ndet = 1\ngputrace = 1\ntrectbatch = 0\n"
              "quit_at_tick = 3400\ntmemfast = 1\ntrifast = 1\n"
              "\n[native]\ninterp = 1\nmachines = 1\nblendflush = 1\n"
              "\n[display]\nborder_mode = 2\n"
              "\n[stereo]\nenabled = 1\n"
              "\n[input]\ndeadzone = 18\n")

    def _strip(self, text):
        from gdxdev import retro
        with tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False) as f:
            f.write(text)
            path = f.name
        try:
            return retro.strip_run_control(path), inifile.read(path)
        finally:
            os.unlink(path)

    def test_every_debug_and_native_key_is_dropped(self):
        """By SECTION, not by a key list. A key list has to be chased every time another
        track invents a lever — `trectbatch` was missed exactly that way."""
        removed, got = self._strip(self.SHARED)
        self.assertEqual(got.get("debug", {}), {})
        self.assertEqual(got.get("native", {}), {})
        for key in ("input_replay", "replay_quit", "fh_every", "det", "gputrace",
                    "trectbatch", "quit_at_tick", "tmemfast", "trifast"):
            self.assertIn("debug.%s" % key, removed)
        self.assertIn("native.interp", removed)

    def test_user_preferences_survive(self):
        _removed, got = self._strip(self.SHARED)
        self.assertEqual(got["display"]["border_mode"], "2")
        self.assertEqual(got["input"]["deadzone"], "18")
        self.assertEqual(got["stereo"]["enabled"], "1")

    def test_stripping_an_ini_with_no_levers_is_a_noop(self):
        removed, got = self._strip("[display]\nborder_mode = 1\n")
        self.assertEqual(removed, [])
        self.assertEqual(got["display"]["border_mode"], "1")

    def test_both_lanes_set_the_same_measurement_ini(self):
        """Nothing is inherited any more, so the retro lane must set what it relies on."""
        import inspect
        from gdxdev import emu, retro
        self.assertIn("MEASUREMENT_INI", inspect.getsource(retro.build_settings))
        for key in (("debug", "filelog"), ("debug", "verbose")):
            self.assertIn(key, emu.MEASUREMENT_INI)

    def test_the_render_thread_matches_the_shipped_preset_on_both_lanes(self):
        """rtparity: the gate measures the configuration that SHIPS.

        The Qt lane merges into the shared virtual SD, so the key must be pinned or a stale
        value defines every run. The shipped `auto` preset sets `renderthread = 0` (the NRR
        refuses under the render thread), and a pinned "1" silently turned the NRR off on
        every New 3DS lane run. `--ini debug.renderthread=1` stays an explicit bisect lever;
        an --ini flag must still win, or the lever would be unreachable.
        """
        from gdxdev import emu, inifile
        self.assertEqual(emu.MEASUREMENT_INI.get(("debug", "renderthread")), "0")
        settings = dict(emu.MEASUREMENT_INI)
        section, key, value = inifile.parse_spec("debug.renderthread=1")
        settings[(section, key)] = value
        self.assertEqual(settings[("debug", "renderthread")], "1")

    def test_apt_signal_lines_are_not_crashes(self):
        """Azahar logs "with signal 00000001" on every healthy boot."""
        from gdxdev import retro
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("applet_manager.cpp:CancelAndSendParameter: ... with signal 00000001\n")
            path = f.name
        try:
            self.assertEqual(retro._crash_signature(path), "")
        finally:
            os.unlink(path)


class ReplayCompletionTest(unittest.TestCase):
    """A guest core that dies MID-replay still leaves an advancing heartbeat and zero
    [fatal] lines. Reported 2026-09-17: an EXC_BAD_ACCESS at frame ~433, 8 s into a
    6000-frame replay, was reported "PASS heartbeat reached frame 432, 0 [fatal] lines"."""

    def _block(self, kind="gdxrec", shots=("a", "b"), last_tick=6000):
        return {"id": "r", "kind": kind, "sha256": "x", "events": 1, "path": None,
                "shots": list(shots), "last_tick": last_tick}

    def _beats(self, upto):
        return "".join("OutputDebugString: frame %d\n" % i for i in range(1, upto + 1))

    def test_gdxrec_without_end_receipt_is_incomplete(self):
        info = replay.completion(self._block(), "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n",
                                 max_frame=432, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertIn("[rec] end", info["detail"])

    def test_gdxrec_with_end_and_all_shots_is_complete(self):
        log = "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n[rec] end tick=6000 lines=6001\n"
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertTrue(info["replay_complete"], info["detail"])
        self.assertEqual((info["shots_seen"], info["shots_expected"]), (2, 2))

    # ---- [debug] shots=0: score on the schedule tag, not on the capture ----------
    # The lever (native/main@cd142bc4) skips the ~280 KB BMP write so a timing run is
    # uncontaminated, and it returns from gdx_request_frame_dump before EITHER capture
    # receipt is emitted. Scoring on captures therefore marked every such run
    # "49 of 49 SHOT(s) never captured" while the replay had in fact reached tick 6001
    # with 0 fatals -- observed on /tmp/gdx-runs/stereocost/20260926-014231-sc-owned-2d.

    SHOTS_OFF = "[rec] [debug] shots=0 -- SHOT tags still log, BMP writes SKIPPED\n"

    def test_shots_off_scores_on_the_rec_tag(self):
        log = (self.SHOTS_OFF + "[rec] SHOT 'a' at tick 119\n[rec] SHOT 'b' at tick 419\n"
               + "[rec] end tick=6000 lines=6001\n")
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertTrue(info["replay_complete"], info["detail"])
        self.assertTrue(info["shots_disabled"])
        self.assertEqual((info["shots_seen"], info["shots_expected"]), (2, 2))

    def test_shots_off_still_asserts_the_whole_schedule(self):
        """Only the CAPTURE assertion is dropped. A label the replay never reached is
        still a failure, and the wording says 'reached' rather than 'captured'."""
        log = (self.SHOTS_OFF + "[rec] SHOT 'a' at tick 119\n"
               + "[rec] end tick=6000 lines=6001\n")
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertIn("never reached", info["detail"])
        self.assertIn("b", info["missing_shots"])

    def test_without_the_receipt_captures_are_still_required(self):
        """No shots=0 receipt: a run carrying only schedule tags and no capture at all
        must still fail, or the fallback would silently weaken every ordinary run."""
        log = ("[rec] SHOT 'a' at tick 119\n[rec] SHOT 'b' at tick 419\n"
               + "[rec] end tick=6000 lines=6001\n")
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertFalse(info["shots_disabled"])
        self.assertIn("never captured", info["detail"])

    def test_missing_shots_are_named(self):
        log = "[fh] frame=1 tag=a\n[rec] end tick=6000 lines=6001\n"
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertEqual(info["missing_shots"], ["b"])
        self.assertIn("b", info["detail"])

    def test_periodic_auto_tag_is_not_a_shot(self):
        log = ("[fh] frame=1 tag=auto\n" * 50) + "[rec] end tick=6000 lines=1\n"
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertEqual(info["shots_seen"], 0)
        self.assertFalse(info["replay_complete"])

    def test_autotest_receipt_also_counts_a_shot(self):
        log = ("[autotest] SHOT captured -> autotest/a_L.bmp bytes=1 frame=2\n"
               "[autotest] SHOT captured -> autotest/b_L.bmp bytes=1 frame=3\n"
               "[rec] end tick=6000 lines=1\n")
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertTrue(info["replay_complete"], info["detail"])

    def test_autoinput_short_of_the_last_tick_is_incomplete(self):
        b = self._block(kind="autoinput", shots=(), last_tick=21000)
        info = replay.completion(b, "", max_frame=432, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertIn("21000", info["detail"])

    def test_autoinput_reaching_the_last_tick_is_complete(self):
        b = self._block(kind="autoinput", shots=("card",), last_tick=100)
        info = replay.completion(b, "[autotest] SHOT captured -> autotest/card_L.bmp\n",
                                 max_frame=120, emu_alive_at_end=True)
        self.assertTrue(info["replay_complete"], info["detail"])

    def test_emulator_dying_early_is_incomplete_even_with_everything_else(self):
        log = "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n"
        info = replay.completion(self._block(), log, max_frame=432, emu_alive_at_end=False)
        self.assertFalse(info["replay_complete"])
        self.assertIn("core died", info["detail"])

    def test_replay_quit_exit_is_legitimate(self):
        log = ("[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n"
               "[rec] end tick=6000 lines=6001\n"
               "[oracle] quit requested: [rec] end with replay_quit=1\n")
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=False)
        self.assertTrue(info["replay_complete"], info["detail"])

    def test_no_replay_means_no_completion_requirement(self):
        info = replay.completion(None, "", max_frame=10, emu_alive_at_end=True)
        self.assertIsNone(info["replay_complete"])

    def test_judge_fails_the_reported_crash_scenario(self):
        """The regression itself: heartbeat to 432, no [fatal], replay armed, core dead."""
        from gdxdev import emu
        sd = ("[rec] replay armed rec/r.gdxrec frames=6001\n"
              "[fh] frame=1 tag=a\n")
        r = emu.judge(self._beats(432), sd, util.Out(quiet=True),
                      replay_kind="gdxrec", replay_block=self._block(),
                      emu_alive_at_end=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertIn("did not complete", r["detail"])
        self.assertEqual(r["shots_seen"], 1)
        self.assertEqual(r["shots_expected"], 2)

    def test_judge_passes_a_genuinely_complete_replay(self):
        from gdxdev import emu
        sd = ("[rec] replay armed rec/r.gdxrec frames=6001\n"
              "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n"
              "[rec] end tick=6000 lines=6001\n")
        r = emu.judge(self._beats(6000), sd, util.Out(quiet=True),
                      replay_kind="gdxrec", replay_block=self._block(),
                      emu_alive_at_end=True)
        self.assertEqual(r["verdict"], "PASS", r["detail"])
        self.assertTrue(r["replay_complete"])
        self.assertIn("replay complete (2/2 SHOTs)", r["detail"])

    def test_shot_labels_parse_real_files(self):
        gd = os.path.join(util.repo_root(), "replays", "race-mutecity-start.gdxrec")
        if os.path.exists(gd):
            labels = replay.shot_labels(gd, "gdxrec")
            self.assertTrue(labels)
            self.assertNotIn("label", labels)      # the header comment is not a SHOT
            # derived, not frozen: the schedule belongs to the coverage track
            self.assertEqual(labels, [l for _, l in replay.shot_schedule(gd, "gdxrec")])
            self.assertEqual(replay.last_tick(gd, "gdxrec"), 6000)
        ai = os.path.join(replay.replays_dir(), "storm2.txt")
        self.assertEqual(replay.shot_labels(ai, "autoinput"),
                         ["card", "drive1", "drive2", "drive3", "drive4", "drive5"])
        self.assertEqual(replay.last_tick(ai, "autoinput"), 21000)


class GuestExitTest(unittest.TestCase):
    """The EMULATOR outliving the GUEST is the normal case: a .3dsx that exits leaves
    Azahar up with a dead window, so `emu run` waited out the whole of --seconds after
    the run was already over (integration-verify-2, item 1)."""

    def test_atexit_receipt_means_the_guest_is_done(self):
        from gdxdev import emu
        self.assertEqual(
            emu.guest_exit_reason("[fatal-trace] atexit: process exiting via exit()\n"),
            "exited via exit()")

    def test_oracle_quit_means_the_guest_is_done(self):
        from gdxdev import emu
        self.assertEqual(
            emu.guest_exit_reason("[oracle] quit requested: [rec] end with replay_quit=1\n"),
            "oracle quit")

    def test_a_healthy_running_log_is_not_an_exit(self):
        from gdxdev import emu
        self.assertIsNone(emu.guest_exit_reason(
            "[watchdog] beat=3 frame=180(+60) stage=5\n[prof] frame=64 n=64 br=1.0\n"))

    def test_the_loop_polls_faster_than_it_snapshots(self):
        from gdxdev import emu
        self.assertLess(emu.POLL_S, emu.SNAPSHOT_S)

    def test_the_wait_loop_checks_the_guest_not_just_the_process(self):
        import inspect
        from gdxdev import emu
        src = inspect.getsource(emu.do_run)
        self.assertIn("guest_exit_reason(", src)


class QuitAtTickTest(unittest.TestCase):
    """A replay cut short deliberately by [debug] quit_at_tick is COMPLETE when the tick
    was reached — same as replay_quit (integration-verify-2, item 2)."""

    INIT = "[fh] init enabled=1 every=0 start=0 eye=L scale=0 quit_at_tick=1000 build=b\n"
    QUIT = "[oracle] quit requested: [debug] quit_at_tick reached\n"

    def _block(self):
        return {"id": "r", "kind": "gdxrec", "sha256": "x", "events": 1, "path": None,
                "shots": ["early", "late"], "last_tick": 6000,
                "schedule": [(100, "early"), (5000, "late")]}

    def test_quit_at_tick_is_complete_without_the_end_receipt(self):
        log = self.INIT + "[fh] frame=1 tag=early\n" + self.QUIT
        info = replay.completion(self._block(), log, max_frame=1000,
                                 emu_alive_at_end=False)
        self.assertTrue(info["replay_complete"], info["detail"])
        self.assertEqual(info["quit_at_tick"], 1000)

    def test_shots_scheduled_after_the_cut_are_not_expected(self):
        log = self.INIT + "[fh] frame=1 tag=early\n" + self.QUIT
        info = replay.completion(self._block(), log, max_frame=1000,
                                 emu_alive_at_end=False)
        self.assertEqual(info["shots_expected"], 1)      # "late" is at tick 5000
        self.assertEqual(info["missing_shots"], [])

    def test_a_shot_before_the_cut_is_still_required(self):
        log = self.INIT + self.QUIT          # "early" never captured
        info = replay.completion(self._block(), log, max_frame=1000,
                                 emu_alive_at_end=False)
        self.assertFalse(info["replay_complete"])
        self.assertEqual(info["missing_shots"], ["early"])

    def test_without_the_quit_receipt_the_end_receipt_is_still_required(self):
        log = self.INIT + "[fh] frame=1 tag=early\n[fh] frame=2 tag=late\n"
        info = replay.completion(self._block(), log, max_frame=6000,
                                 emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertIn("[rec] end", info["detail"])

    def test_quit_at_tick_parsing(self):
        self.assertEqual(replay.quit_at_tick(self.INIT + self.QUIT), 1000)
        self.assertIsNone(replay.quit_at_tick(self.INIT))   # configured but never reached

    def test_shot_schedule_carries_ticks(self):
        ai = os.path.join(replay.replays_dir(), "storm2.txt")
        sched = replay.shot_schedule(ai, "autoinput")
        self.assertEqual(sched[0], (3400, "card"))
        self.assertEqual([l for _t, l in sched], replay.shot_labels(ai, "autoinput"))


class ErrorCensusTest(unittest.TestCase):
    """ONE classifier for every path. emu.judge used to re-derive its own fatal count,
    which is how `[fatal-trace] atexit` kept failing clean runs after prof.py was fixed
    (integration-verify-2, item 3)."""

    CRASH = "[fatal] assert failed: x (a.c:1 in f)\n"
    ATEXIT = "[fatal-trace] atexit: process exiting via exit()\n"

    def test_one_crash_in_both_sinks_counts_once(self):
        """logFatal (main_3ds.cpp:412) writes the SAME line to the Azahar log AND the SD
        filelog, so summing the sinks reported 2 fatals for one assert."""
        c = prof.error_census(self.CRASH, self.CRASH)
        self.assertEqual(c["fatals"], 1)

    def test_a_crash_in_only_one_sink_still_counts(self):
        self.assertEqual(prof.error_census(self.CRASH, "")["fatals"], 1)

    def test_two_distinct_crashes_count_twice(self):
        both = self.CRASH + "[fatal] second one\n"
        self.assertEqual(prof.error_census(both, both)["fatals"], 2)

    def test_atexit_is_benign_in_the_census(self):
        c = prof.error_census(self.ATEXIT, self.ATEXIT)
        self.assertEqual(c["fatals"], 0)
        self.assertEqual(c["benign"], 2)

    def test_judge_and_prof_agree_through_the_shared_classifier(self):
        from gdxdev import emu
        beats = "".join("OutputDebugString: frame %d\n" % i for i in range(1, 5))
        r = emu.judge(beats + self.ATEXIT, self.ATEXIT, util.Out(quiet=True))
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["fatals"], 0)
        r2 = emu.judge(beats + self.CRASH, self.CRASH, util.Out(quiet=True))
        self.assertEqual(r2["fatals"], 1)
        self.assertIn("1 [fatal] line(s)", r2["detail"])

    def test_judge_uses_the_census_rather_than_its_own_sum(self):
        import inspect
        from gdxdev import emu
        self.assertIn("prof.error_census(", inspect.getsource(emu.judge))

    def test_is_benign_is_the_single_predicate(self):
        self.assertTrue(prof.is_benign("x [fatal-trace] atexit: process exiting"))
        self.assertFalse(prof.is_benign("[fatal-trace] pc=0x1 lr=0x2 in Machine_Draw"))


class ResultLineTest(unittest.TestCase):
    def test_result_line_carries_sha256_not_just_the_build_id(self):
        """GDX3DS_BUILD_ID is configure-time and can name the wrong commit after a
        merge-then-rebuild, so the artifact hash is what a report must quote."""
        from gdxdev import emu
        m = {"build": {"id": "native/devcli@deadbeef", "sha256": "a" * 64},
             "replay": {"id": "r", "kind": "gdxrec"}, "wall_s": 8.2}
        r = {"verdict": "FAIL", "max_frame": 432, "fatals": 0, "replay_complete": False,
             "shots_seen": 1, "shots_expected": 8}
        line = emu.result_line(m, r)
        self.assertTrue(line.startswith("RESULT FAIL"))
        self.assertIn("sha256=" + "a" * 16, line)
        self.assertIn("wall_s=8.2", line)
        self.assertIn("complete=False", line)
        self.assertIn("shots=1/8", line)


class ReapPruneTest(unittest.TestCase):
    def setUp(self):
        from gdxdev import retro
        self.retro = retro
        self.tmp = tempfile.mkdtemp()
        self._root = retro.INSTANCE_ROOT
        retro.INSTANCE_ROOT = self.tmp

    def tearDown(self):
        self.retro.INSTANCE_ROOT = self._root
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _mk(self, name, age_days):
        d = os.path.join(self.tmp, name)
        os.makedirs(d, exist_ok=True)
        f = os.path.join(d, "retroarch.cfg")
        open(f, "w").write("x\n")
        old = time.time() - age_days * 86400
        os.utime(f, (old, old))
        os.utime(d, (old, old))
        return d

    def _prune(self, days=2, dry=False, claimed=None, live=()):
        real = self.retro.our_retro_processes
        self.retro.our_retro_processes = lambda: [{"instance": n} for n in live]
        try:
            args = type("A", (), {"days": days, "dry_run": dry})()
            return self.retro._prune_instances(args, util.Out(quiet=True), claimed or {})
        finally:
            self.retro.our_retro_processes = real

    def test_old_idle_dirs_are_pruned(self):
        self._mk("old", 5)
        self._mk("fresh", 0)
        self.assertEqual(self._prune(), ["old"])
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "old")))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "fresh")))

    def test_default_threshold_is_two_days(self):
        self._mk("d1", 1)
        self._mk("d3", 3)
        self.assertEqual(self._prune(), ["d3"])

    def test_a_running_instance_is_never_pruned(self):
        self._mk("busy", 9)
        self.assertEqual(self._prune(live=["busy"]), [])
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "busy")))

    def test_a_live_driver_lock_protects_an_old_dir(self):
        self._mk("held", 9)
        claimed = {"held": {"driver_pid": os.getpid()}}
        self.assertEqual(self._prune(claimed=claimed), [])

    def test_a_future_dated_asset_does_not_block_pruning(self):
        """seed_sd copies the user's dspfirm.cdc, which is dated 2035 on this machine.
        A newest-file-in-tree heuristic therefore put every instance nine years in the
        future and pruned nothing, ever."""
        d = self._mk("old", 9)
        sd = os.path.join(d, "saves", "Azahar", "Azahar", "sdmc", "3ds")
        os.makedirs(sd, exist_ok=True)
        f = os.path.join(sd, "dspfirm.cdc")
        open(f, "w").write("x")
        future = time.time() + 9 * 365 * 86400
        os.utime(f, (future, future))
        self.assertEqual(self._prune(), ["old"])

    def test_last_used_reads_a_file_gdx_dev_writes(self):
        d = self._mk("x", 3)
        inst = self.retro.Instance("x")
        self.assertAlmostEqual(self.retro._last_used(inst),
                               os.path.getmtime(os.path.join(d, "retroarch.cfg")),
                               places=3)

    def test_dry_run_deletes_nothing(self):
        self._mk("old", 9)
        self.assertEqual(self._prune(dry=True), ["old"])
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "old")))


class HwIniDeleteTest(unittest.TestCase):
    """Host side of `hw.py ini --delete` / `replay --off`.

    `ini <section> <key> ""` could not clear a key: the control protocol sent
    "INI debug input_replay " and the guest kept the old value, so a replay stayed armed
    across reloads (coordinator, 2026-09-20). These tests stub the transport — nothing
    here may touch the console, which is mid-manual-test.
    """

    def setUp(self):
        sys.path.insert(0, os.path.join(util.tools_dev()))
        import hw
        self.hw = hw
        self.sent = []
        test = self

        class FakeControl(object):
            def __init__(self, ip, port=None, timeout=None):
                test.sent.append(("connect", ip))

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def command(self, line):
                test.sent.append(line)
                return "OK"

            def put_file(self, line, path):
                test.sent.append((line, path))
                return "OK"

        self._real_control = hw.Control
        self._real_load_ip = hw.load_ip
        hw.Control = FakeControl
        hw.load_ip = lambda explicit: explicit or "10.0.0.1"
        # hw.py prints each reply; keep it out of the test report.
        self._stdout = sys.stdout
        sys.stdout = io.StringIO()

    def tearDown(self):
        sys.stdout = self._stdout
        self.hw.Control = self._real_control
        self.hw.load_ip = self._real_load_ip

    def _args(self, **kw):
        base = {"ip": None, "section": "debug", "key": "input_replay", "value": None,
                "delete": False, "save": False, "file": None, "off": False, "dest": None}
        base.update(kw)
        return type("A", (), base)()

    def _lines(self):
        return [x for x in self.sent if isinstance(x, str)]

    def test_delete_sends_ini_del(self):
        self.assertEqual(self.hw.cmd_ini(self._args(delete=True)), 0)
        self.assertEqual(self._lines(), ["INI DEL debug input_replay"])

    def test_delete_with_save_also_saves(self):
        self.hw.cmd_ini(self._args(delete=True, save=True))
        self.assertEqual(self._lines(), ["INI DEL debug input_replay", "INI SAVE"])

    def test_the_set_path_is_unchanged(self):
        self.hw.cmd_ini(self._args(value="1", key="tmemfast"))
        self.assertEqual(self._lines(), ["INI debug tmemfast 1"])

    def test_set_with_save_is_unchanged(self):
        self.hw.cmd_ini(self._args(value="1", key="tmemfast", save=True))
        self.assertEqual(self._lines(), ["INI debug tmemfast 1", "INI SAVE"])

    def test_delete_refuses_a_value(self):
        with self.assertRaises(SystemExit):
            self.hw.cmd_ini(self._args(delete=True, value="x"))
        self.assertEqual(self._lines(), [], "must not send anything before dying")

    def test_set_without_a_value_is_refused(self):
        """The old failure mode: `ini debug input_replay ""` looked like it worked."""
        with self.assertRaises(SystemExit):
            self.hw.cmd_ini(self._args(value=None))
        self.assertEqual(self._lines(), [])

    def test_replay_off_deletes_and_saves(self):
        self.assertEqual(self.hw.cmd_replay(self._args(off=True)), 0)
        self.assertEqual(self._lines(),
                         ["INI DEL debug input_replay", "INI SAVE"])

    def test_replay_off_never_pushes_a_file(self):
        self.hw.cmd_replay(self._args(off=True))
        self.assertEqual([x for x in self.sent if isinstance(x, tuple)
                          and x[0] != "connect"], [])

    def test_replay_without_a_file_or_off_is_refused(self):
        with self.assertRaises(SystemExit):
            self.hw.cmd_replay(self._args())

    def test_replay_still_pushes_and_arms(self):
        with tempfile.NamedTemporaryFile(suffix=".gdxrec", delete=False) as f:
            f.write(b"gdxrec 1\n")
            path = f.name
        try:
            self.hw.cmd_replay(self._args(file=path))
        finally:
            os.unlink(path)
        base = os.path.basename(path)
        self.assertIn(("PUT /3ds/gdiffuser/rec/" + base, path), self.sent)
        self.assertIn("REPLAY /3ds/gdiffuser/rec/" + base, self._lines())

    # ---- end to end through hw.main(), so the parser itself is covered ----------
    def test_main_ini_delete(self):
        self.assertEqual(self.hw.main(["ini", "--delete", "debug", "input_replay"]), 0)
        self.assertEqual(self._lines(), ["INI DEL debug input_replay"])

    def test_main_ini_delete_save(self):
        self.hw.main(["ini", "--delete", "--save", "debug", "input_replay"])
        self.assertEqual(self._lines(), ["INI DEL debug input_replay", "INI SAVE"])

    def test_main_ini_set_still_works(self):
        self.hw.main(["ini", "debug", "tmemfast", "1"])
        self.assertEqual(self._lines(), ["INI debug tmemfast 1"])

    def test_main_replay_off(self):
        self.assertEqual(self.hw.main(["replay", "--off"]), 0)
        self.assertEqual(self._lines(), ["INI DEL debug input_replay", "INI SAVE"])

    def test_main_ini_set_with_no_value_is_refused(self):
        with self.assertRaises(SystemExit):
            self.hw.main(["ini", "debug", "input_replay"])
        self.assertEqual(self._lines(), [])


class GuestIniDelProtocolTest(unittest.TestCase):
    """The guest half, checked at the source so host and console cannot drift."""

    def _src(self, rel):
        p = os.path.join(util.repo_root(), rel)
        if not os.path.exists(p):
            self.skipTest("%s not present" % rel)
        return util.read_text(p)

    def test_net_handles_ini_del(self):
        net = self._src("port/3ds/net/gdx3ds_net.c")
        self.assertIn("REQ_INI_DEL", net)
        self.assertIn('strcmp(tok[1], "DEL")', net)
        self.assertIn("gdx3ds_config_unset(sReqA, sReqB)", net)
        self.assertIn('netLog("[net] ini del', net)

    def test_del_is_checked_before_the_set_form(self):
        """Otherwise `INI DEL debug input_replay` sets a section literally named DEL."""
        net = self._src("port/3ds/net/gdx3ds_net.c")
        i = net.index('strcmp(tok[1], "DEL")')
        j = net.index("netPostRequest(REQ_INI_SET")
        self.assertLess(i, j)

    def test_config_exposes_unset(self):
        self.assertIn("int gdx3ds_config_unset(", self._src("port/3ds/os/gdx3ds_config.h"))
        self.assertIn("int gdx3ds_config_unset(", self._src("port/3ds/os/gdx3ds_config.c"))

    def test_the_host_test_is_wired_into_ci(self):
        ci = self._src("tools/ci-3ds.sh")
        self.assertIn("gdx3ds_config_host_tests", ci)


class HwPutGetTest(unittest.TestCase):
    """Host side of `hw.py put` / `hw.py get` (n60lead hwlink round): a generic file push
    (reused by `replay` already) and the new pull. Transport stubbed, same shape as
    HwIniDeleteTest -- nothing here may touch the console."""

    def setUp(self):
        self.sent = []
        test = self

        class FakeControl(object):
            def __init__(self, ip, port=None, timeout=None):
                test.sent.append(("connect", ip))

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def command(self, line):
                test.sent.append(line)
                return "OK"

            def put_file(self, line, path):
                test.sent.append(("put", line, path))
                return "OK PUT sdmc:" + line.split(None, 1)[1]

            def get_file(self, sdmc_path, out_path):
                test.sent.append(("get", sdmc_path, out_path))
                with open(out_path, "wb") as f:
                    f.write(b"fake")
                return 4

        self._real_control = hw.Control
        self._real_load_ip = hw.load_ip
        hw.Control = FakeControl
        hw.load_ip = lambda explicit: explicit or "10.0.0.1"
        self._stdout = sys.stdout
        sys.stdout = io.StringIO()
        self._tmp = tempfile.mkdtemp()

    def tearDown(self):
        sys.stdout = self._stdout
        hw.Control = self._real_control
        hw.load_ip = self._real_load_ip
        shutil.rmtree(self._tmp, ignore_errors=True)

    def _put_args(self, **kw):
        base = {"ip": None, "file": None, "dest": None}
        base.update(kw)
        return type("A", (), base)()

    def _get_args(self, **kw):
        base = {"ip": None, "path": None, "out": None}
        base.update(kw)
        return type("A", (), base)()

    def test_put_pushes_the_exact_dest(self):
        local = os.path.join(self._tmp, "new.ini")
        with open(local, "w") as f:
            f.write("[net]\nenabled = 1\n")
        self.assertEqual(
            hw.cmd_put(self._put_args(file=local, dest="/3ds/gdiffuser/gdiffuser.ini")), 0)
        self.assertEqual(
            self.sent, [("connect", "10.0.0.1"),
                       ("put", "PUT /3ds/gdiffuser/gdiffuser.ini", local)])

    def test_put_refuses_a_relative_dest(self):
        local = os.path.join(self._tmp, "new.ini")
        open(local, "w").close()
        with self.assertRaises(SystemExit):
            hw.cmd_put(self._put_args(file=local, dest="3ds/gdiffuser/gdiffuser.ini"))
        self.assertEqual(self.sent, [], "must not connect before dying")

    def test_put_refuses_a_missing_local_file(self):
        with self.assertRaises(SystemExit):
            hw.cmd_put(self._put_args(file=os.path.join(self._tmp, "nope"),
                                      dest="/3ds/gdiffuser/gdiffuser.ini"))
        self.assertEqual(self.sent, [])

    def test_get_pulls_under_the_gdiffuser_prefix(self):
        out = os.path.join(self._tmp, "log.txt")
        self.assertEqual(
            hw.cmd_get(self._get_args(path="/3ds/gdiffuser/log.txt", out=out)), 0)
        self.assertEqual(self.sent, [("connect", "10.0.0.1"),
                                     ("get", "/3ds/gdiffuser/log.txt", out)])
        with open(out, "rb") as f:
            self.assertEqual(f.read(), b"fake")

    def test_get_refuses_a_path_outside_the_sandbox(self):
        """Mirrors the console's own confinement (netPathUnderGdiffuser) so a caller finds
        out from `hw.py` itself, not a round trip to the console just to get ERR GET."""
        with self.assertRaises(SystemExit):
            hw.cmd_get(self._get_args(path="/3ds/G-Diffuser-3DS.3dsx"))
        self.assertEqual(self.sent, [], "must not connect before dying")

    def test_get_default_out_is_under_artifacts(self):
        # Redirect hw.REPO to a scratch dir for this one case: with no --out, cmd_get's
        # default path is REPO/artifacts/<basename>, and a real run would otherwise write
        # (via FakeControl.get_file, which actually touches disk like the real one does)
        # straight into this checkout's own artifacts/ directory.
        real_repo = hw.REPO
        hw.REPO = self._tmp
        try:
            hw.cmd_get(self._get_args(path="/3ds/gdiffuser/log.txt"))
        finally:
            hw.REPO = real_repo
        got_out = [c for c in self.sent if isinstance(c, tuple) and c[0] == "get"][0][2]
        self.assertEqual(os.path.basename(got_out), "log.txt")
        self.assertEqual(os.path.basename(os.path.dirname(got_out)), "artifacts")
        self.assertTrue(got_out.startswith(self._tmp), "must not default into the real repo")

    # ---- end to end through hw.main(), so the parser itself is covered ----------
    def test_main_put(self):
        local = os.path.join(self._tmp, "new.ini")
        open(local, "w").close()
        self.assertEqual(hw.main(["put", local, "/3ds/gdiffuser/gdiffuser.ini"]), 0)
        self.assertEqual(self.sent, [("connect", "10.0.0.1"),
                                     ("put", "PUT /3ds/gdiffuser/gdiffuser.ini", local)])

    def test_main_get(self):
        out = os.path.join(self._tmp, "log.txt")
        self.assertEqual(hw.main(["get", "/3ds/gdiffuser/log.txt", "--out", out]), 0)
        self.assertEqual(self.sent, [("connect", "10.0.0.1"),
                                     ("get", "/3ds/gdiffuser/log.txt", out)])


class HwGetFramingTest(unittest.TestCase):
    """Control.get_file's wire framing against a REAL socket (a connected
    socket.socketpair stands in for the console): 'OK GET <size> <crc32hex>\\n' followed by
    exactly <size> raw bytes, including when a single recv() handed read_line() part of
    that payload along with the reply line -- the leftover-buffer case `get_file` has to
    seed `data` from rather than drop."""

    def _control_fed(self, wire_bytes):
        """`mine` plays the console's half of the control socket. `theirs` stays open
        until the test ends (closing it right away, before get_file sends its own `GET
        ...` command line, would EPIPE that send on a connected SOCK_STREAM pair --
        closing a peer breaks writes to it immediately, not just reads from it)."""
        c = hw.Control.__new__(hw.Control)
        mine, theirs = socket.socketpair()
        c.sock = mine
        c.buf = b""
        theirs.sendall(wire_bytes)
        self.addCleanup(theirs.close)
        self.addCleanup(mine.close)
        return c

    def _out_path(self):
        fd, path = tempfile.mkstemp()
        os.close(fd)
        self.addCleanup(lambda: os.path.exists(path) and os.unlink(path))
        return path

    def test_payload_arriving_with_the_reply_line_is_not_dropped(self):
        payload = b"[net] init port=17492...\n[net] link up build=abc\n"
        crc = binascii.crc32(payload) & 0xFFFFFFFF
        wire = ("OK GET %d %08X\n" % (len(payload), crc)).encode("ascii") + payload
        c = self._control_fed(wire)
        out = self._out_path()
        size = c.get_file("/3ds/gdiffuser/log.txt", out, progress=False)
        self.assertEqual(size, len(payload))
        with open(out, "rb") as f:
            self.assertEqual(f.read(), payload)

    def test_crc_mismatch_raises_and_still_leaves_no_partial_claim(self):
        payload = b"corrupted on the wire"
        wire = ("OK GET %d %08X\n" % (len(payload), 0)).encode("ascii") + payload
        c = self._control_fed(wire)
        with self.assertRaises(IOError):
            c.get_file("/3ds/gdiffuser/log.txt", self._out_path(), progress=False)

    def test_err_reply_raises_without_touching_the_output_file(self):
        c = self._control_fed(b"ERR GET not-found-or-unreadable sdmc:/3ds/gdiffuser/x\n")
        out = self._out_path()
        os.unlink(out)
        with self.assertRaises(IOError):
            c.get_file("/3ds/gdiffuser/x", out, progress=False)
        self.assertFalse(os.path.exists(out), "a refused GET must not create the output file")


class GuestGetProtocolTest(unittest.TestCase):
    """The guest half, checked at the source so host and console cannot drift (same spirit
    as GuestIniDelProtocolTest above, for the new GET command)."""

    def _src(self, rel):
        p = os.path.join(util.repo_root(), rel)
        if not os.path.exists(p):
            self.skipTest("%s not present" % rel)
        return util.read_text(p)

    def test_net_handles_get(self):
        net = self._src("port/3ds/net/gdx3ds_net.c")
        self.assertIn("REQ_GET", net)
        self.assertIn('strcmp(tok[0], "GET")', net)
        self.assertIn("netPathUnderGdiffuser", net)
        self.assertIn("netGetFileForHost", net)

    def test_get_is_confined_to_the_same_prefix_hw_py_checks(self):
        net = self._src("port/3ds/net/gdx3ds_net.c")
        hwsrc = self._src("tools/dev/hw.py")
        self.assertIn('#define NET_GET_PREFIX "/3ds/gdiffuser/"', net)
        self.assertIn('"/3ds/gdiffuser/"', hwsrc)

    def test_get_runs_on_the_main_thread_via_the_request_queue(self):
        """Not handled straight on the net thread like PUT/RELOAD: GET's target can be
        log.txt, which another thread may be appending to right now (see the comment above
        netGetFileForHost). It must go through the same REQ_* hand-off as INI/CAPTURE."""
        net = self._src("port/3ds/net/gdx3ds_net.c")
        i = net.index('strcmp(tok[0], "GET")')
        j = net.index("netPostRequest(REQ_GET", i)
        self.assertGreater(j, i)

    def test_get_is_size_capped(self):
        net = self._src("port/3ds/net/gdx3ds_net.c")
        self.assertIn("NET_GET_CAP", net)
        self.assertIn("too-large", net)


class DelegateTest(unittest.TestCase):
    def test_missing_peer_exits_7(self):
        from gdxdev import delegate
        if os.path.exists(delegate.peer_path("framediff.py")):
            self.skipTest("oracle track has merged; the absence path is untestable here")
        with self.assertRaises(GdxError) as cm:
            delegate.require("framediff.py")
        self.assertEqual(cm.exception.code, util.EXIT_NODELEGATE)
        self.assertIn("oracle", str(cm.exception))


class StdlibOnlyTest(unittest.TestCase):
    def test_no_third_party_imports(self):
        """05-devcli-plan.md §6: stdlib-only drift. Import with -S (no site-packages)."""
        import subprocess
        pkg = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        code = ("import sys; sys.path.insert(0, %r); "
                "import gdxdev.cli, gdxdev.emu, gdxdev.prof, gdxdev.sd, gdxdev.retro"
                % pkg)
        p = subprocess.run([sys.executable, "-S", "-c", code],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertEqual(p.returncode, 0, p.stdout)


# =====================================================================================
# hw-run: the unattended hardware config matrix (NEXT.md A9)
#
# The console is not on. Everything below runs against the committed slices of the real
# 2026-09-20 session (tests/fixtures/hw-*.log) and a mocked control link, so the whole
# flow — ini application, reload between configs, marker-delimited slicing, completion
# checking, manifests — is provable with the hardware powered off.
# =====================================================================================
from gdxdev import hwrun  # noqa: E402

# The SHOTs the committed hardware fixtures actually contain — i.e. the canonical
# replay's schedule AS IT STOOD when those logs were captured on the console. It is NOT
# the replay's current schedule: the coverage track rederived that file with
# `covsched derive --rewrite` and it now schedules 49 SHOTs. These tests feed the
# fixtures, so they must expect the fixtures' own set; the test that checks the live file
# derives from the live file (test_the_canonical_replays_schedule_is_read_off_the_file).
FIXTURE_SHOTS = ["title", "mainmenu", "machsel", "race00", "race01", "race02",
                 "race04", "race03"]


def canon_schedule():
    """[(tick, label), ...] the canonical replay schedules TODAY, read from the file."""
    path, kind = replay.resolve("race-mutecity-start")
    return replay.shot_schedule(path, kind)


def _fix(name):
    return util.read_text(os.path.join(FIXTURES, name))


class HwRunConfigSpecTest(unittest.TestCase):
    def test_a_config_is_a_comma_separated_list_of_section_dot_key(self):
        self.assertEqual(hwrun.parse_config("native.all=0,native.interp=1"),
                         [("native", "all", "0"), ("native", "interp", "1")])

    def test_a_bare_key_defaults_to_debug(self):
        self.assertEqual(hwrun.parse_config("gputrace=1"), [("debug", "gputrace", "1")])

    def test_an_empty_value_means_DELETE_not_set_to_empty(self):
        """`INI <k> ""` leaves an entry behind that survives INI SAVE — that is how a
        replay stayed armed across reloads (hw.py:cmd_ini)."""
        self.assertEqual(hwrun.parse_config("debug.input_replay="),
                         [("debug", "input_replay", None)])

    def test_label_names_the_delete(self):
        self.assertEqual(hwrun.config_label(hwrun.parse_config("a=1,b=")), "a=1,b=DEL")

    def test_an_empty_config_is_a_usage_error(self):
        with self.assertRaises(GdxError) as cm:
            hwrun.parse_config("  ,  ")
        self.assertEqual(cm.exception.code, util.EXIT_USAGE)


class HwRunMarkerTest(unittest.TestCase):
    """Slicing by a marker the GUEST emits, against a stream holding two slices."""

    @classmethod
    def setUpClass(cls):
        cls.text = _fix("hw-markers.log")

    def test_the_marker_needle_matches_the_guest_receipt_format(self):
        """port/3ds/net/gdx3ds_net.c:476 logs `[net] cmd INI [<sec>] <key>=<val> rc=<n>`."""
        needle = hwrun.mark_needle("abc123-c00r01-b")
        self.assertIn(needle, self.text)
        line = [l for l in self.text.splitlines() if needle in l][0]
        self.assertIn("[net] cmd INI [debug] hwrun_mark=", line)

    def test_a_slice_excludes_both_marker_lines(self):
        body, status = hwrun.slice_marked(self.text, "abc123-c00r01-b", "abc123-c00r01-e")
        self.assertEqual(status, "ok")
        self.assertNotIn("hwrun_mark", body)

    def test_a_slice_is_exactly_its_own_config_and_nothing_else(self):
        """The byte-offset version DID leak: artifacts/hwfps3-r1-all1-interp1.log opens
        with the PREVIOUS run's race03/race04 SHOTs, because the offset was taken before
        the earlier config had finished flushing."""
        for token, source in (("c00r01", "hw-fps-all0-interp0.log"),
                              ("c01r01", "hw-fps-all1-interp1.log")):
            body, status = hwrun.slice_marked(self.text, "abc123-%s-b" % token,
                                              "abc123-%s-e" % token)
            self.assertEqual(status, "ok")
            want = "\n".join(l for l in _fix(source).splitlines()
                              if not l.startswith("#"))
            self.assertEqual(body, want)
            self.assertEqual(body.count("[rec] replay armed"), 1)
            self.assertEqual(body.count("[rec] end tick="), 1)

    def test_slicing_is_stable_when_another_writer_appends(self):
        a1, _ = hwrun.slice_marked(self.text, "abc123-c00r01-b", "abc123-c00r01-e")
        grown = self.text + "\n19:40:00 [net] ctl accept from 10.0.0.1\n" * 50
        a2, _ = hwrun.slice_marked(grown, "abc123-c00r01-b", "abc123-c00r01-e")
        self.assertEqual(a1, a2)

    def test_a_missing_end_marker_gives_everything_after_the_begin(self):
        body, status = hwrun.slice_marked(self.text, "abc123-c01r01-b", "never-sent")
        self.assertEqual(status, "open")
        self.assertIn("[rec] end tick=", body)

    def test_a_missing_begin_marker_yields_nothing_rather_than_the_whole_log(self):
        body, status = hwrun.slice_marked(self.text, "never-sent", "abc123-c01r01-e")
        self.assertEqual((body, status), ("", "missing"))

    def test_tokens_are_unique_per_config_and_repeat(self):
        seen = set()
        for cfg in range(3):
            for rep in range(1, 4):
                seen.update(hwrun.mark_tokens("20260923-010203-hw-run", cfg, rep))
        self.assertEqual(len(seen), 18)

    def test_a_token_fits_the_guest_log_buffer(self):
        """netLog writes into msg[224] and netReadCtl reads into buf[256]."""
        beg, _ = hwrun.mark_tokens("20260923-010203-hw-run", 9, 9)
        line = "[net] cmd INI [debug] hwrun_mark=%s rc=0" % beg
        self.assertLess(len(line), 200)
        self.assertNotIn(" ", beg)


class HwRunFpsWindowTest(unittest.TestCase):
    """The in-race fps window, against the two trace-off slices of the real session.

    The fixtures are console logs from the HAND-PLACED schedules, whose first in-race SHOT
    is the literal `race00`. `IN_RACE_ANCHOR` is now `mode:01` because coverage2 rederived
    the corpus and mints `m<mode>_<frame>` labels instead, so these tests name the anchor
    their own fixtures carry rather than riding the default — the default is about today's
    recordings, the fixtures are about that session.
    """

    FIXTURE_ANCHOR = "race00"

    def test_the_window_reproduces_the_published_beat_sequence(self):
        """docs/research/native/integration-verify-2026-09-17.md records
        `21 51 59 30 46 60 55 39 15 43 43 43` for all=0 interp=0."""
        vals, anchor = hwrun.fps_window(_fix("hw-fps-all0-interp0.log"),
                                        anchor=self.FIXTURE_ANCHOR)
        self.assertIsNotNone(anchor)
        self.assertEqual([round(v) for v in vals],
                         [21, 51, 59, 30, 46, 60, 55, 39, 15, 43, 43, 43])

    def test_sixty_seconds_is_twelve_five_second_beats(self):
        vals, _ = hwrun.fps_window(_fix("hw-fps-all1-interp1.log"),
                                   anchor=self.FIXTURE_ANCHOR)
        self.assertEqual(len(vals), 12)
        self.assertEqual(prof.WATCHDOG_PERIOD_S, 5.0)

    def test_the_window_is_counted_in_beats_not_host_timestamps(self):
        """The link flushes in bursts — beats 30/31/32 of the session all carry the same
        host second — so a wall-clock window would cut wherever the network hiccuped."""
        text = _fix("hw-fps-all0-interp0.log")
        stamps = [l.split()[0] for l in text.splitlines() if "[watchdog]" in l]
        self.assertLess(len(set(stamps)), len(stamps), "the fixture must contain a burst")
        vals, _ = hwrun.fps_window(text, anchor=self.FIXTURE_ANCHOR)
        self.assertEqual(len(vals), 12)

    def test_no_anchor_means_no_window_rather_than_the_whole_run(self):
        vals, anchor = hwrun.fps_window(_fix("hw-fps-all0-interp0.log"), anchor="nosuch")
        self.assertEqual((vals, anchor), ([], None))

    def test_a_shorter_window_takes_fewer_beats(self):
        vals, _ = hwrun.fps_window(_fix("hw-fps-all0-interp0.log"), seconds=30,
                                   anchor=self.FIXTURE_ANCHOR)
        self.assertEqual([round(v) for v in vals], [21, 51, 59, 30, 46, 60])

    def test_pooled_stats_match_the_published_table(self):
        """Published: all=0 min 15.0 mean 42.0 8% at cap; all=1 min 12.8 mean 42.8 17%.
        Those were pooled over three repeats; one repeat reproduces min and the cap
        fraction, and the mean to a tenth."""
        a, _ = hwrun.fps_window(_fix("hw-fps-all0-interp0.log"),
                                anchor=self.FIXTURE_ANCHOR)
        b, _ = hwrun.fps_window(_fix("hw-fps-all1-interp1.log"),
                                anchor=self.FIXTURE_ANCHOR)
        sa, sb = hwrun.fps_stats(a), hwrun.fps_stats(b)
        self.assertEqual((sa["n"], sb["n"]), (12, 12))
        self.assertAlmostEqual(sa["min"], 15.4, places=1)
        self.assertAlmostEqual(sb["min"], 13.6, places=1)
        self.assertAlmostEqual(sa["mean"], 42.0, places=0)
        self.assertAlmostEqual(sb["mean"], 42.8, places=0)
        self.assertAlmostEqual(sa["at_cap"], 1 / 12.0, places=3)
        self.assertAlmostEqual(sb["at_cap"], 2 / 12.0, places=3)

    def test_percentiles_are_nearest_rank_like_the_rest_of_the_cli(self):
        st = hwrun.fps_stats([1.0, 2.0, 3.0, 4.0])
        self.assertEqual(st["p50"], 2.0)
        self.assertIn(st["p50"], [1.0, 2.0, 3.0, 4.0])

    def test_empty_is_none_not_a_crash(self):
        self.assertIsNone(hwrun.fps_stats([]))

    def test_the_default_anchor_resolves_against_the_live_corpus(self):
        """The bug 075f214 fixed, pinned: the default anchor silently resolved to nothing
        after the corpus was rederived, and an empty fps series is the same shape as a
        slice that never reached the race."""
        labels = [l for _, l in canon_schedule()]
        fake = "\n".join("[rec] SHOT '%s' at tick %d" % (l, t)
                          for t, l in canon_schedule())
        self.assertIsNotNone(
            hwrun.find_anchor(fake.splitlines(), hwrun.IN_RACE_ANCHOR),
            "IN_RACE_ANCHOR %r matches nothing in the canonical replay's %d labels: %s"
            % (hwrun.IN_RACE_ANCHOR, len(labels), labels[:6]))


class HwRunCompletionTest(unittest.TestCase):
    """`sleep 150` truncated two of the three configs in the published [prof] A/B. The
    completion check is what makes that visible instead of silent."""

    def test_the_published_all1_interp1_prof_slice_is_complete(self):
        c = hwrun.completion(_fix("hw-prof-all1-interp1.log"), FIXTURE_SHOTS)
        self.assertTrue(c["replay_complete"], c["detail"])
        self.assertEqual(c["shots_seen"], 8)
        self.assertTrue(c["end_receipt"])

    def test_the_published_all0_interp0_prof_slice_was_cut_short(self):
        c = hwrun.completion(_fix("hw-prof-all0-interp0.log"), FIXTURE_SHOTS)
        self.assertFalse(c["replay_complete"])
        self.assertFalse(c["end_receipt"])
        self.assertEqual(c["missing_shots"], ["race04", "race03"])
        self.assertIn("[rec] end", c["detail"])

    def test_both_trace_off_slices_are_complete(self):
        for name in ("hw-fps-all0-interp0.log", "hw-fps-all1-interp1.log"):
            c = hwrun.completion(_fix(name), FIXTURE_SHOTS)
            self.assertTrue(c["replay_complete"], "%s: %s" % (name, c["detail"]))

    def test_the_hardware_shot_wording_is_recognised(self):
        """gdx3ds_oracle.c says 'SHOT captured'; the 3DS bridge says 'SHOT dumped', and
        `[fh] tag=` needs fh=1, which perf runs turn off. All three must count."""
        self.assertEqual(hwrun.shots_seen("[rec] SHOT 'race00' at tick 3300"), {"race00"})
        self.assertEqual(hwrun.shots_seen("[autotest] SHOT dumped -> autotest/race01.bmp"),
                         {"race01"})
        self.assertEqual(
            hwrun.shots_seen("[autotest] SHOT scanout dumped -> autotest/race02_scan.bmp"),
            {"race02"})
        self.assertIn("race03", hwrun.shots_seen("[fh] frame=1 tag=race03"))

    def test_a_replay_that_never_loaded_is_not_complete(self):
        c = hwrun.completion("[watchdog] beat=1 frame=60(+60)", FIXTURE_SHOTS)
        self.assertFalse(c["replay_complete"])
        self.assertIn("never loaded", c["detail"])

    def test_the_canonical_replays_schedule_is_read_off_the_file(self):
        """Derived, never frozen: the schedule is the coverage track's to rewrite
        (NEXT.md B47/B48), and a hardcoded copy of it here goes stale silently — it did,
        and this test failed on main for exactly that reason."""
        path, kind = replay.resolve("race-mutecity-start")
        sched = replay.shot_schedule(path, kind)
        self.assertTrue(sched, "the canonical replay schedules no SHOTs at all")
        self.assertEqual(replay.shot_labels(path, kind), [l for _, l in sched])
        self.assertEqual(len(set(l for _, l in sched)), len(sched), "duplicate labels")
        self.assertLessEqual(max(t for t, _ in sched), replay.last_tick(path, kind))


class HwRunProfTableTest(unittest.TestCase):
    """The summary tables, against the slices the published table was computed from."""

    def _buckets(self, name):
        p = prof.parse_text(_fix(name))
        return {n.split()[0]: prof.section_summary(sel)
                for n, sel in prof.bucket_by_nd(p.prof)}

    def test_the_crowd_bucket_reproduces_the_published_milliseconds(self):
        a = self._buckets("hw-prof-all0-interp0.log")["crowd"]
        b = self._buckets("hw-prof-all1-interp1.log")["crowd"]
        self.assertAlmostEqual(a["total"]["mean"], 33.97, places=2)
        self.assertAlmostEqual(b["total"]["mean"], 24.54, places=2)
        self.assertAlmostEqual(a["vtx"]["mean"], 4.21, places=2)
        self.assertAlmostEqual(b["vtx"]["mean"], 0.10, places=2)
        self.assertAlmostEqual(a["tri"]["mean"], 11.66, places=2)
        self.assertAlmostEqual(b["tri"]["mean"], 1.30, places=2)
        self.assertAlmostEqual(a["dsp"]["mean"], 11.08, places=2)
        self.assertAlmostEqual(b["dsp"]["mean"], 15.62, places=2)

    def test_the_menu_and_mid_buckets_match_too(self):
        a, b = (self._buckets("hw-prof-all0-interp0.log"),
                self._buckets("hw-prof-all1-interp1.log"))
        self.assertAlmostEqual(a["menu"]["total"]["mean"], 13.24, places=2)
        self.assertAlmostEqual(b["menu"]["total"]["mean"], 12.63, places=2)
        self.assertAlmostEqual(a["mid"]["total"]["mean"], 19.21, places=2)
        self.assertAlmostEqual(b["mid"]["total"]["mean"], 17.69, places=2)

    def test_gputrace_is_visible_in_the_trace_on_slices_only(self):
        self.assertTrue(prof.parse_text(_fix("hw-prof-all1-interp1.log")).gputrace)
        self.assertIsNone(prof.parse_text(_fix("hw-fps-all1-interp1.log")).gputrace)

    def test_no_error_receipts_in_any_slice(self):
        for name in ("hw-prof-all0-interp0.log", "hw-prof-all1-interp1.log",
                     "hw-fps-all0-interp0.log", "hw-fps-all1-interp1.log"):
            c = prof.error_census(_fix(name))
            self.assertEqual(c["fatals"], 0, name)

    def test_repeats_pool_into_one_bucket_set(self):
        r = [{"cfg": 0, "label": "a=0", "parsed": prof.parse_text(_fix("hw-prof-all0-interp0.log")),
              "fps": [10.0]},
             {"cfg": 0, "label": "a=0", "parsed": prof.parse_text(_fix("hw-prof-all0-interp0.log")),
              "fps": [20.0]},
             {"cfg": 1, "label": "a=1", "parsed": prof.parse_text(_fix("hw-prof-all1-interp1.log")),
              "fps": [30.0]}]
        groups = hwrun.pool_by_config(r)
        self.assertEqual([g[0] for g in groups], [0, 1])
        self.assertEqual(len(hwrun.pooled_windows(groups[0][2])), 180)
        self.assertEqual(hwrun.pooled_fps(groups[0][2]), [10.0, 20.0])


class HwRunPongTest(unittest.TestCase):
    def test_pong_is_parsed_for_the_manifest_uptime(self):
        d = hwrun.parse_pong("OK PONG native/main@8b3ecfab 1156 43210")
        self.assertEqual(d, {"build": "native/main@8b3ecfab", "frame": 1156,
                             "uptime_ms": 43210})

    def test_a_non_pong_reply_is_none(self):
        self.assertIsNone(hwrun.parse_pong("ERR unknown command FOO"))
        self.assertIsNone(hwrun.parse_pong(""))
        self.assertIsNone(hwrun.parse_pong("OK PONG short"))


# ---------------------------------------------------------------- mocked console ----
class _FakeSink(hwrun.LogSink):
    """A log stream the fake console writes into, so the real waiting, slicing and
    completion code runs unchanged with no hardware and no sockets."""

    mode = "fake"

    def __init__(self):
        self.lines = []
        self.rotations = []

    def text(self):
        return "\n".join(self.lines)

    def rotate(self, path):
        self.rotations.append(path)

    def write(self, text):
        self.lines.extend(text.splitlines())


class HwRunSequenceTest(unittest.TestCase):
    """Drives do_hw_run end to end against a mocked transport: the console is off."""

    def setUp(self):
        sys.path.insert(0, util.tools_dev())
        import hw
        self.hw = hw
        self.sink = _FakeSink()
        self.sent = []
        test = self
        # The fixtures are real console logs from when the canonical replay scheduled 8
        # SHOTs; it now schedules 49. Append the receipts for whatever it schedules today
        # so a mocked "complete" slice stays complete when that file is rewritten again.
        tail = "\n".join("[rec] SHOT '%s' at tick %d" % (label, tick)
                          for tick, label in canon_schedule())
        self.bodies = [_fix("hw-fps-all0-interp0.log") + "\n" + tail,
                       _fix("hw-fps-all1-interp1.log") + "\n" + tail]
        self.body_at = 0

        class FakeControl(object):
            def __init__(self, ip, port=None, timeout=None):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def command(self, line):
                test.sent.append(line)
                if line == "PING":
                    return "OK PONG native/main@deadbeef 1200 45000"
                if line.startswith("INI debug hwrun_mark "):
                    # The guest logs the receipt itself (gdx3ds_net.c:476) — that is the
                    # whole point of using an ini set as the marker.
                    token = line.split()[-1]
                    test.sink.write("[net] cmd INI [debug] hwrun_mark=%s rc=0" % token)
                    if token.endswith("-b"):
                        # The replay runs AFTER the begin marker, which is the ordering
                        # the runner depends on: reload, relaunch, mark, then the run.
                        test.sink.write(test.bodies[test.body_at % len(test.bodies)])
                        test.body_at += 1
                    return "OK INI"
                return "OK"

            def put_file(self, header, path, progress=True):
                test.sent.append(("PUT" if header.startswith("PUT") else "RELOAD", header))
                if header == "RELOAD":
                    test.sink.write("[net] link up build=native/main@deadbeef")
                return "OK"

        self._saved = (hw.Control, hw.load_ip, hwrun.open_sink,
                       hw.wait_for_relaunch_via_ping)
        hw.Control = FakeControl
        hw.load_ip = lambda explicit: explicit or "10.0.0.1"
        hw.wait_for_relaunch_via_ping = lambda ip, t, old: ("[net] link up", [])
        hwrun.open_sink = lambda *a, **k: self.sink
        hwrun._import_hw = lambda: hw

        self.tmp = tempfile.mkdtemp(prefix="gdx-hwrun-test-")
        self.dsx = os.path.join(self.tmp, "G-Diffuser-3DS.3dsx")
        with open(self.dsx, "wb") as f:
            f.write(b"3DSX\x00fake")
        self.out = util.Out(quiet=True)

    def tearDown(self):
        (self.hw.Control, self.hw.load_ip, hwrun.open_sink,
         self.hw.wait_for_relaunch_via_ping) = self._saved
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _args(self, **kw):
        base = {"config": ["native.all=0,native.interp=0", "native.all=1,native.interp=1"],
                "replay": "race-mutecity-start", "repeats": 1, "seconds": 5, "settle": 0,
                "poll": 0, "wait": 1, "gputrace": False, "base_ini": None,
                "restore": None, "no_restore": False,
                "fps_window": hwrun.DEFAULT_FPS_WINDOW_S,
                "fps_anchor": hwrun.IN_RACE_ANCHOR, "threedsx": self.dsx, "ip": "10.0.0.1",
                "name": "unit", "dry_run": False, "json": False, "quiet": True}
        base.update(kw)
        return type("A", (), base)()

    def _ctx(self):
        return {"root": util.repo_root(), "track": "hwrun-unit",
                "run_root": os.path.join(self.tmp, "runs")}

    def _lines(self):
        return [x for x in self.sent if isinstance(x, str)]

    def _run(self, **kw):
        ctx = self._ctx()
        code = hwrun.do_hw_run(self._args(**kw), self.out, ctx)
        root = runs.track_root(ctx["run_root"], ctx["track"])
        rd = runs.RunDir(os.path.join(root, sorted(os.listdir(root))[-1]))
        return code, rd

    # ---- sequencing
    def test_every_config_gets_its_levers_saved_then_a_reload(self):
        code, _ = self._run()
        self.assertEqual(code, util.EXIT_OK)
        lines = self._lines()
        first = lines.index("INI native all 0")
        self.assertEqual(lines[first:first + 3],
                         ["INI native all 0", "INI native interp 0", "INI SAVE"])
        # the RELOAD comes after the SAVE, never before it
        reloads = [i for i, x in enumerate(self.sent)
                   if isinstance(x, tuple) and x[0] == "RELOAD"]
        saves = [i for i, x in enumerate(self.sent) if x == "INI SAVE"]
        self.assertTrue(saves[1] < reloads[0] < saves[2])

    def test_one_reload_per_slice_plus_one_for_the_restore(self):
        self._run(repeats=2)
        reloads = [x for x in self.sent if isinstance(x, tuple) and x[0] == "RELOAD"]
        self.assertEqual(len(reloads), 2 * 2 + 1)

    def test_repeats_are_the_outer_loop_so_drift_hits_every_config(self):
        self._run(repeats=2)
        order = [l for l in self._lines() if l.startswith("INI native all ")]
        self.assertEqual(order, ["INI native all 0", "INI native all 1",
                                 "INI native all 0", "INI native all 1"])

    def test_the_replay_is_pushed_once_and_armed_through_the_ini(self):
        self._run()
        puts = [x for x in self.sent if isinstance(x, tuple) and x[0] == "PUT"]
        self.assertEqual(len(puts), 1)
        self.assertEqual(puts[0][1],
                         "PUT /3ds/gdiffuser/rec/race-mutecity-start.gdxrec")
        self.assertEqual(puts[0][1].count(".gdxrec"), 1, "no doubled extension")
        self.assertIn("INI debug input_replay race-mutecity-start", self._lines())

    def test_the_marker_is_set_then_deleted_so_ini_save_can_never_persist_it(self):
        self._run()
        marks = [l for l in self._lines() if "hwrun_mark" in l]
        self.assertEqual(len(marks), 8)          # 2 slices x (begin,end) x (set,del)
        for i in range(0, len(marks), 2):
            self.assertTrue(marks[i].startswith("INI debug hwrun_mark "))
            self.assertEqual(marks[i + 1], "INI DEL debug hwrun_mark")
        # and no INI SAVE ever happens between a set and its delete
        seq = self._lines()
        for i, l in enumerate(seq):
            if l.startswith("INI debug hwrun_mark "):
                self.assertEqual(seq[i + 1], "INI DEL debug hwrun_mark")

    def test_the_marker_is_emitted_after_the_reload_not_before(self):
        self._run()
        first_mark = next(i for i, x in enumerate(self.sent)
                          if isinstance(x, str) and x.startswith("INI debug hwrun_mark"))
        first_reload = next(i for i, x in enumerate(self.sent)
                            if isinstance(x, tuple) and x[0] == "RELOAD")
        self.assertLess(first_reload, first_mark)

    def test_gputrace_is_applied_and_recorded(self):
        _, rd = self._run(gputrace=True)
        self.assertIn("INI debug gputrace 1", self._lines())
        self.assertTrue(rd.read_manifest()["gputrace"])

    def test_the_default_base_ini_pins_determinism_and_turns_hashing_off(self):
        self._run()
        lines = self._lines()
        self.assertIn("INI debug det 1", lines)
        self.assertIn("INI debug fh 0", lines)
        self.assertIn("INI debug gputrace 0", lines)

    # ---- restore
    def test_the_restore_deletes_the_replay_rather_than_emptying_it(self):
        self._run()
        self.assertIn("INI DEL debug input_replay", self._lines())
        self.assertNotIn("INI debug input_replay ", self._lines()[-6:])

    def test_no_restore_leaves_the_console_armed(self):
        self._run(no_restore=True)
        tail = self._lines()[-4:]
        self.assertNotIn("INI DEL debug input_replay", tail)

    def test_the_restore_still_runs_when_a_slice_blows_up(self):
        original = hwrun._run_slice
        hwrun._run_slice = lambda *a, **k: (_ for _ in ()).throw(GdxError("boom"))
        try:
            with self.assertRaises(GdxError):
                self._run()
        finally:
            hwrun._run_slice = original
        self.assertIn("INI DEL debug input_replay", self._lines())

    # ---- artifacts
    def test_each_slice_gets_a_schema_valid_manifest(self):
        _, rd = self._run()
        for slug in ("cfg00-rep01", "cfg01-rep01"):
            m = runs.RunDir(rd.sub(slug)).read_manifest()
            self.assertEqual(runs.validate(m), [])
            self.assertEqual(m["lane"], "hw")
            self.assertEqual(m["replay"]["id"], "race-mutecity-start")
            self.assertEqual(m["build"]["sha256"], util.sha256_file(self.dsx))
            self.assertIsNotNone(m["platform"]["guest_uptime_ms_end"])
            self.assertEqual(m["result"]["marker_status"], "ok")

    def test_the_manifest_records_the_per_config_levers(self):
        _, rd = self._run()
        a = runs.RunDir(rd.sub("cfg00-rep01")).read_manifest()["ini"]
        b = runs.RunDir(rd.sub("cfg01-rep01")).read_manifest()["ini"]
        self.assertEqual(a["native.all"], "0")
        self.assertEqual(b["native.all"], "1")
        self.assertEqual(a["debug.input_replay"], "race-mutecity-start")

    def test_the_matrix_manifest_lists_the_slices(self):
        _, rd = self._run()
        m = rd.read_manifest()
        self.assertEqual(runs.validate(m), [])
        self.assertEqual(m["result"]["slices"], ["cfg00-rep01", "cfg01-rep01"])
        self.assertEqual(m["result"]["verdict"], "PASS")

    def test_each_slice_log_holds_only_its_own_config(self):
        _, rd = self._run()
        a = util.read_text(rd.sub("cfg00-rep01", "log.txt"))
        self.assertNotIn("hwrun_mark", a)
        self.assertEqual(a.count("[rec] end tick="), 1)

    def test_the_sink_is_rotated_once_per_slice(self):
        self._run(repeats=2)
        self.assertEqual(len(self.sink.rotations), 4)

    # ---- verdicts
    def test_an_incomplete_replay_fails_the_run(self):
        self.bodies = ["\n".join(_fix("hw-fps-all0-interp0.log").splitlines()[:20])] * 2
        code, rd = self._run(seconds=1)
        self.assertEqual(code, util.EXIT_FAIL)
        self.assertEqual(rd.read_manifest()["result"]["verdict"], "FAIL")
        self.assertFalse(runs.RunDir(rd.sub("cfg00-rep01"))
                         .read_manifest()["result"]["replay_complete"])

    def test_a_fatal_receipt_fails_the_run(self):
        self.bodies = [_fix("hw-fps-all0-interp0.log") + "\n[fatal] assert in gfx\n"] * 2
        code, _ = self._run()
        self.assertEqual(code, util.EXIT_FAIL)

    def test_an_autoinput_replay_is_refused_on_hardware(self):
        with self.assertRaises(GdxError) as cm:
            self._run(replay="boot")
        self.assertEqual(cm.exception.code, util.EXIT_STAGING)

    def test_no_config_is_a_usage_error(self):
        with self.assertRaises(GdxError) as cm:
            self._run(config=[])
        self.assertEqual(cm.exception.code, util.EXIT_USAGE)

    # ---- dry run
    def test_dry_run_opens_no_connection_and_writes_nothing(self):
        ctx = self._ctx()
        code = hwrun.do_hw_run(self._args(dry_run=True), self.out, ctx)
        self.assertEqual(code, util.EXIT_OK)
        self.assertEqual(self.sent, [], "a dry run must not talk to the console")
        self.assertFalse(os.path.exists(ctx["run_root"]),
                         "a dry run must not create a run directory")

    def test_dry_run_names_every_step_it_would_take(self):
        buf = io.StringIO()
        saved, sys.stdout = sys.stdout, buf
        try:
            hwrun.do_hw_run(self._args(dry_run=True), util.Out(), self._ctx())
        finally:
            sys.stdout = saved
        text = buf.getvalue()
        for needle in ("PUT /3ds/gdiffuser/rec/race-mutecity-start.gdxrec",
                       "INI native all 0", "INI native all 1", "INI SAVE",
                       "RELOAD", "hwrun_mark", "INI DEL debug hwrun_mark",
                       "rotate log sink", "[rec] end", "INI DEL debug input_replay"):
            self.assertIn(needle, text, needle)


class HwRunCliTest(unittest.TestCase):
    def test_the_subcommand_is_wired_into_gdx_dev(self):
        from gdxdev import cli
        args = cli.build_parser().parse_args(
            ["hw-run", "--config", "native.all=1", "--repeats", "3", "--gputrace"])
        self.assertIs(args.func, hwrun.do_hw_run)
        self.assertEqual(args.config, ["native.all=1"])
        self.assertEqual(args.repeats, 3)
        self.assertTrue(args.gputrace)
        self.assertEqual(args.replay, "race-mutecity-start")

    def test_hw_run_does_not_reimplement_the_control_protocol(self):
        """It goes through delegate.require('hw.py') so a branch without the hwlink
        track gets exit 7 naming the track, not an ImportError."""
        src = util.read_text(os.path.join(util.tools_dev(), "gdxdev", "hwrun.py"))
        self.assertIn('delegate.require("hw.py")', src)
        for forbidden in ("socket.create_connection", "3dslink", "sendall"):
            self.assertNotIn(forbidden, src)


class FhDiffShiftTest(unittest.TestCase):
    """`--shift` vanished once already: a merge pasted a whole main() INSIDE another
    function's docstring, so the file still parsed, CI still passed, and the flag simply
    stopped existing — taking every `--shift` result in the rtparity evidence with it.
    Assert the flag is reachable AND that it actually realigns, not merely that it parses."""

    def _run(self, *args):
        import subprocess
        fh = os.path.join(util.repo_root(), "tools", "dev", "fhdiff.py")
        return subprocess.run([sys.executable, fh] + list(args),
                              capture_output=True, text=True)

    def test_shift_is_reachable(self):
        self.assertIn("--shift", self._run("--help").stdout)

    def test_shift_realigns_an_offset_stream(self):
        with tempfile.TemporaryDirectory() as d:
            a = os.path.join(d, "a.log")
            b = os.path.join(d, "b.log")
            with open(a, "w") as f:
                f.write("[fh] frame=10 tick=9 eye=L hash=aaaa\n"
                        "[fh] frame=11 tick=10 eye=L hash=bbbb\n")
            with open(b, "w") as f:   # same content, one frame later
                f.write("[fh] frame=11 tick=10 eye=L hash=aaaa\n"
                        "[fh] frame=12 tick=11 eye=L hash=bbbb\n")
            self.assertIn("divergent=1", self._run(a, b).stdout)
            aligned = self._run(a, b, "--shift", "-1").stdout
            self.assertIn("compared=2", aligned)
            self.assertIn("divergent=0", aligned)





class IniSpecTypoTest(unittest.TestCase):
    """`--ini` takes one key per flag; the comma-separated typo used to pass silently."""

    def test_a_comma_list_is_refused_with_the_fix_spelled_out(self):
        # `--ini a=1,b=2` wrote the WHOLE string as the value of `a`, so b never reached
        # the guest. Two runs then differed only in levers that had not been applied to
        # either, which read as a parity failure and cost a bisect across three commits.
        with self.assertRaises(GdxError) as caught:
            inifile.parse_spec("debug.det=1,debug.renderthread=1,native.all=1")
        msg = str(caught.exception)
        self.assertIn("one key per flag", msg)
        self.assertIn("--ini debug.renderthread=1", msg)
        self.assertIn("--ini native.all=1", msg)

    def test_a_value_containing_equals_is_refused(self):
        with self.assertRaises(GdxError):
            inifile.parse_spec("debug.x=a=b")

    def test_ordinary_specs_still_parse(self):
        self.assertEqual(inifile.parse_spec("native.all=1"), ("native", "all", "1"))
        self.assertEqual(inifile.parse_spec("det=1"), (inifile.DEFAULT_SECTION, "det", "1"))
        # n60launch: [variant.N] sections carry a dot of their own.
        self.assertEqual(inifile.parse_spec("variant.N.ndsp_core1=1"), ("variant.N", "ndsp_core1", "1"))
        self.assertEqual(inifile.parse_spec("variant.P.native.all=0"), ("variant.P", "native.all", "0"))
        # A path value keeps working -- only '=' and ',' are refused.
        self.assertEqual(inifile.parse_spec("debug.input_replay=race-mutecity-start"),
                         ("debug", "input_replay", "race-mutecity-start"))


class HwRunRelaunchSeamTest(unittest.TestCase):
    """The three faults the first real matrix run after the WiFi link came back hit.

    All three live in the seam between RELOAD and the first control command, and all
    three were found on hardware rather than here, which is why they are pinned now.
    """

    def test_arming_receipt_is_read_from_the_boot_log(self):
        # The guest prints `[rec] replay armed` while it boots, which is strictly before
        # hw-run can set its begin marker -- setting it needs a control connection that
        # does not exist yet. A slice with 49/49 SHOTs and a `[rec] end` was scored
        # "the replay never loaded" because the receipt was sought in the marker window.
        sliced = "\n".join(["[rec] SHOT 'm00_119' at tick 119",
                            "[rec] SHOT 'm01_3225' at tick 3225",
                            "[rec] end"])
        boot = "22:26:37 [rec] replay armed sdmc:/3ds/gdiffuser/rec/x.gdxrec frames=6001"
        expected = ["m00_119", "m01_3225"]
        without = hwrun.completion(sliced, expected)
        self.assertFalse(without["replay_complete"])
        self.assertIn("never loaded", without["detail"])
        with_boot = hwrun.completion(sliced, expected, boot_text=boot)
        self.assertTrue(with_boot["replay_complete"], with_boot["detail"])
        self.assertEqual(with_boot["shots_seen"], 2)

    def test_transport_errors_are_retried(self):
        # `[net] link up` says the log link is dialling out, not that the control port
        # can serve: the relaunched guest accepts and then RSTs the first command. An
        # unretried error aborted a whole six-slice matrix twice, losing the remaining
        # slices AND the restore.
        attempts = {"n": 0}

        class FlakyControl:
            def __init__(self, ip, timeout=None):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def command(self, line):
                attempts["n"] += 1
                if attempts["n"] < 3:
                    raise ConnectionResetError(54, "Connection reset by peer")
                return "OK PONG native/main@abc 1 2"

        class HW:
            Control = FlakyControl

        console = hwrun.Console(HW(), "1.2.3.4", out=None)
        console.TRANSPORT_BACKOFF_S = 0.0
        self.assertTrue(console.command("PING").startswith("OK"))
        self.assertEqual(attempts["n"], 3)

    def test_a_protocol_refusal_is_not_retried(self):
        # An ERR reply is the console answering, not the link failing. Retrying it would
        # turn one refusal into six.
        attempts = {"n": 0}

        class ErrControl:
            def __init__(self, ip, timeout=None):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def command(self, line):
                attempts["n"] += 1
                return "ERR nope"

        class HW:
            Control = ErrControl

        console = hwrun.Console(HW(), "1.2.3.4", out=None)
        console.TRANSPORT_BACKOFF_S = 0.0
        with self.assertRaises(GdxError):
            console.command("INI a b c")
        self.assertEqual(attempts["n"], 1)

    def test_mode_anchor_survives_a_schedule_rederive(self):
        # coverage2 renamed every SHOT to m<mode>_<frame>; the literal `race00` the fps
        # window anchored on stopped existing and resolved to an empty series, which is
        # the same shape as a slice that never raced.
        lines = ["[rec] SHOT 'm00_119' at tick 119",
                 "[rec] SHOT 'm09_3208' at tick 3208",
                 "[rec] SHOT 'm01_3225' at tick 3225"]
        self.assertEqual(hwrun.find_anchor(lines, "mode:01"), 2)
        self.assertEqual(hwrun.find_anchor(lines, "mode:1"), 2)
        self.assertEqual(hwrun.find_anchor(lines, "m01_3225"), 2)
        self.assertIsNone(hwrun.find_anchor(lines, "race00"))
        self.assertEqual(hwrun.anchor_candidates({"m00_119", "m01_3225"}), ["m00", "m01"])


class HwPushVerifyTest(unittest.TestCase):
    """3dslink exits 0 on a partial transfer, and its last progress line is unterminated."""

    def test_a_short_byte_count_is_not_a_failed_push(self):
        # 3dslink's progress reporting stalls at ~46% when its stdout is a pipe, so a low
        # final count says nothing about what arrived. Two pushes were declared TRUNCATED on
        # that evidence while the guest was in fact running -- the console is the authority.
        seen = {"n": 0}

        class UpControl:
            def __init__(self, ip, timeout=None):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def command(self, line):
                seen["n"] += 1
                return "OK PONG native/main@abc 427 10042"

        real = hw.Control
        hw.Control = UpControl
        try:
            self.assertTrue(hw._wait_for_guest("1.2.3.4", 5).startswith("OK"))
        finally:
            hw.Control = real
        self.assertEqual(seen["n"], 1)

    def test_an_unreachable_console_is_reported_not_asserted(self):
        class DownControl:
            def __init__(self, ip, timeout=None):
                raise ConnectionRefusedError(61, "Connection refused")

        real = hw.Control
        hw.Control = DownControl
        try:
            self.assertIsNone(hw._wait_for_guest("1.2.3.4", 1))
        finally:
            hw.Control = real

    def test_final_unterminated_progress_line_is_counted(self):
        # Scanning only the completed pieces dropped the final line, so a 100% push was
        # twice reported as TRUNCATED at 46.18% -- at the same byte count both times,
        # which is what a flaky link does not do.
        script = ("import sys;"
                  "sys.stdout.write('Sending G-Diffuser-3DS.3dsx, 3265100 bytes\\r');"
                  "sys.stdout.write('1507717 sent (46.18%), 130 blocks\\r');"
                  "sys.stdout.write('3265100 sent (100.00%), 282 blocks');"
                  "sys.stdout.flush()")
        real_popen = subprocess.Popen

        def fake_popen(cmd, **kw):
            return real_popen([sys.executable, "-c", script], **kw)

        subprocess.Popen = fake_popen
        try:
            rc, sent, total = hw._run_3dslink("1.2.3.4", "x.3dsx")
        finally:
            subprocess.Popen = real_popen
        self.assertEqual(rc, 0)
        self.assertEqual(total, 3265100)
        self.assertEqual(sent, 3265100)


class HarnessEntryPointTest(unittest.TestCase):
    """`if __name__ == "__main__": unittest.main()` must be the LAST statement.

    It was not. `unittest.main()` sat 100 lines from the end, and since a module runs
    top to bottom and `unittest.main` exits the process, the two classes defined after
    it -- HwRunRelaunchSeamTest and HwPushVerifyTest, five tests pinning the relaunch
    seam and the push verifier -- were never collected and never ran. CI printed
    `PASS: gdxdev_harness_tests (OK)`, which was true of the 236 it ran and silent
    about the 5 it did not. Same shape as B45/B50/B52: a gate reporting something
    other than what it knows.

    This test is the fence. It reads its own source rather than trusting the runner,
    because a test that fails to be collected cannot fail.
    """

    SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_gdxdev.py")

    def _source(self):
        with open(self.SRC) as fh:
            return fh.read()

    def test_nothing_is_defined_after_the_runner(self):
        src = self._source()
        marker = 'if __name__ == "__main__":'
        self.assertIn(marker, src)
        # rindex, not index: this test's own source quotes the marker.
        tail = src[src.rindex(marker):]
        stray = re.findall(r"^(class|def)\s+(\w+)", tail, re.M)
        self.assertEqual(
            [], stray,
            "these are defined after unittest.main() and can never run: %s" % (stray,))

    def test_every_test_in_the_file_was_collected(self):
        src = self._source()
        in_file = set(re.findall(r"^class (\w*Test\w*)\(", src, re.M))
        loaded = set(n for n, obj in globals().items()
                     if isinstance(obj, type) and issubclass(obj, unittest.TestCase))
        self.assertEqual(set(), in_file - loaded,
                         "declared in the file but not importable/collected")


class DriverOptionShadowTest(unittest.TestCase):
    """NEXT.md B52 — a subcommand must not discard a driver-level option.

    argparse's subparser action parses into a FRESH namespace and then copies every
    key of it onto the driver's namespace, including keys holding only the
    SUBPARSER's default. `gdx-dev --track X retro run ...` therefore filed the run
    under the worktree's name, and `gdx-dev --dry-run retro run ...` really ran.
    Found when covdrift.sh reported `RESULT PASS ... shots=49/49` for every replay
    and then `NO RUN` for every replay.

    The first test is the general fence -- it walks the whole parser tree, so a
    subcommand added later that repeats a driver-level option without
    argparse.SUPPRESS fails here rather than in someone's run directory.
    """

    def setUp(self):
        from gdxdev import cli
        self.cli = cli
        self.ap = cli.build_parser()

    def _walk(self, parser, path, ancestors, found):
        mine, subs = {}, []
        for a in parser._actions:
            if isinstance(a, argparse._SubParsersAction):
                subs.append(a)
                continue
            if a.dest in (argparse.SUPPRESS, "help"):
                continue
            mine[a.dest] = a
        for dest, action in sorted(mine.items()):
            for apath, anc in ancestors:
                if dest in anc and action.default is not argparse.SUPPRESS:
                    found.append(("%s" % (" ".join(path) or "<driver>"), dest,
                                  " ".join(apath) or "<driver>", action.default))
        nxt = ancestors + [(path, mine)]
        for sa in subs:
            seen = set()
            for name, sp in sa.choices.items():
                if id(sp) in seen:
                    continue
                seen.add(id(sp))
                self._walk(sp, path + [name], nxt, found)

    def test_no_subcommand_anywhere_shadows_an_outer_option(self):
        found = []
        self._walk(self.ap, [], [], found)
        self.assertEqual(
            [], found,
            "these redeclare an outer option with a default that will overwrite it: %s"
            % (found,))

    def test_every_driver_option_is_declared_by_the_driver(self):
        dests = set()
        for a in self.ap._actions:
            if not isinstance(a, argparse._SubParsersAction):
                dests.add(a.dest)
        for name in self.cli.DRIVER_OPTIONS:
            self.assertIn(name, dests)

    def test_driver_track_reaches_retro_run(self):
        args = self.ap.parse_args(["--track", "TT", "retro", "run", "x.3dsx"])
        self.assertEqual("TT", args.track)

    def test_driver_dry_run_reaches_retro_run(self):
        # The B45 flag, at the level above the one B45 fixed. Silently discarded
        # meant `gdx-dev --dry-run retro run` booted a real RetroArch instance.
        args = self.ap.parse_args(["--dry-run", "retro", "run", "x.3dsx"])
        self.assertTrue(args.dry_run)

    def test_driver_json_and_run_root_reach_retro_run(self):
        args = self.ap.parse_args(
            ["--json", "--run-root", "/tmp/rr", "retro", "run", "x.3dsx"])
        self.assertTrue(args.json)
        self.assertEqual("/tmp/rr", args.run_root)

    def test_driver_track_reaches_every_leaf_that_takes_one(self):
        for argv in (["build"],
                     ["emu", "run", "x.3dsx"],
                     ["emu", "status"],
                     ["prof"],
                     ["runs", "list"],
                     ["report"],
                     ["retro", "run", "x.3dsx"],
                     ["retro", "status"],
                     ["hw-run"],
                     ["sd", "pull", "/tmp/x"]):
            args = self.ap.parse_args(["--track", "TT"] + argv)
            self.assertEqual("TT", args.track, "lost by %s" % (" ".join(argv),))

    def test_the_subcommand_still_wins_when_both_are_typed(self):
        args = self.ap.parse_args(
            ["--track", "DRIVER", "retro", "run", "x.3dsx", "--track", "SUB"])
        self.assertEqual("SUB", args.track)

    def test_neither_given_still_defaults(self):
        args = self.ap.parse_args(["retro", "run", "x.3dsx"])
        self.assertIsNone(args.track)
        self.assertFalse(args.dry_run)

    def test_the_driver_track_is_what_lands_in_the_run_root(self):
        """End to end through the real CLI: `runs list` prints track and run-root."""
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
        dev = os.path.join(root, "tools", "dev", "gdx-dev")
        d = tempfile.mkdtemp()
        try:
            out = subprocess.run(
                [sys.executable, dev, "--track", "zz-shadow-probe",
                 "--run-root", d, "runs", "list"],
                capture_output=True, text=True, timeout=120)
            self.assertIn("zz-shadow-probe", out.stdout + out.stderr)
            self.assertIn(d, out.stdout + out.stderr)
        finally:
            shutil.rmtree(d, ignore_errors=True)


class PatchGateTest(unittest.TestCase):
    """NEXT.md B50 — the decomp patch gate must name the patches that are missing.

    The gate roundtrips the FULL ordered stack (reverse-all, then forward-all), which
    is right for an order-dependent stack but leaves a third state unexplained: the
    first M of N applied, the normal tree right after a merge that ADDS patches.
    Forward-all dies on patch 0, reverse-all dies on patch N-1, and the old gate
    printed "partial application / conflicting local edits" and pointed at
    `git -C decomp diff` -- which shows the healthy drift of a correctly patched tree.

    These run against a throwaway git repo, which is the reason the gate was lifted
    out of tools/ci-3ds.sh into tools/patch-gate.sh at all: before, the only way to
    exercise it was to damage a real worktree.
    """

    GATE = os.path.abspath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "patch-gate.sh"))

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = self.tmp
        self.sub = os.path.join(self.root, "sub")
        self.pdir = os.path.join(self.root, "patches")
        os.makedirs(self.sub)
        os.makedirs(self.pdir)
        self._git("init", "-q")
        self._git("config", "user.email", "t@t")
        self._git("config", "user.name", "t")
        # 30 lines, and the three hunks land 12 lines apart, so no two patches share
        # context. That is deliberate: it makes the LAST patch apply on its own even
        # with its predecessor missing, which is exactly the coincidence the real
        # decomp stack showed and the reason a short suffix cannot be trusted.
        self._write("\n".join("line %d" % i for i in range(1, 31)) + "\n")
        self._git("add", "f.txt")
        self._git("commit", "-qm", "base")
        self.base = self._git("rev-parse", "HEAD").strip()
        self.patches = []
        for n, at in ((1, 3), (2, 15), (3, 27)):
            text = open(os.path.join(self.sub, "f.txt")).read()
            text = text.replace("line %d\n" % at,
                                "line %d\nADDED BY PATCH %d\n" % (at, n))
            self._write(text)
            name = "p%d.patch" % n
            with open(os.path.join(self.pdir, name), "w") as fh:
                fh.write(self._git("diff"))
            # Commit between steps so the NEXT `git diff` is a pure delta and the
            # stack is genuinely ordered rather than cumulative.
            self._git("commit", "-qam", "p%d" % n)
            self.patches.append(name)
        self._git("reset", "--hard", "-q", self.base)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write(self, text):
        with open(os.path.join(self.sub, "f.txt"), "w") as fh:
            fh.write(text)

    def _git(self, *a):
        return subprocess.run(["git", "-C", self.sub] + list(a),
                              capture_output=True, text=True, check=True).stdout

    def _reset_to(self, applied):
        """Put the worktree in the state where the first `applied` patches are on."""
        self._git("reset", "--hard", "-q", self.base)
        for name in self.patches[:applied]:
            subprocess.run(["git", "-C", self.sub, "apply",
                            os.path.join(self.pdir, name)], check=True)

    def _gate(self, env=None):
        e = dict(os.environ)
        if env:
            e.update(env)
        return subprocess.run(
            ["bash", self.GATE, self.root, self.pdir] +
            ["sub:%s" % n for n in self.patches],
            capture_output=True, text=True, env=e, timeout=120)

    def _applied_count(self):
        body = open(os.path.join(self.sub, "f.txt")).read()
        return sum(1 for n in (1, 2, 3) if "ADDED BY PATCH %d" % n in body)

    # -- the two states the old gate already handled, kept as a no-regression fence --

    def test_a_clean_tree_gets_the_whole_stack(self):
        self._reset_to(0)
        r = self._gate()
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("clean tree", r.stdout)
        self.assertIn("PATCHGATE: already=0 applied=3", r.stdout)
        self.assertEqual(3, self._applied_count())

    def test_a_fully_applied_tree_roundtrips(self):
        self._reset_to(3)
        r = self._gate()
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("full-stack roundtrip", r.stdout)
        self.assertIn("PATCHGATE: already=3 applied=0", r.stdout)
        self.assertEqual(3, self._applied_count())

    # ------------------------------------------------------------------ B50 --

    def test_the_missing_tail_is_named_and_applied(self):
        self._reset_to(1)
        r = self._gate()
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("2 of 3 patches were never applied", r.stdout)
        self.assertIn("p2.patch", r.stdout)
        self.assertIn("p3.patch", r.stdout)
        # and it does NOT blame the tree's own edits the way the old gate did
        self.assertNotIn("conflicting local edits", r.stdout + r.stderr)
        self.assertEqual(3, self._applied_count())

    def test_one_missing_patch_is_named_as_one(self):
        self._reset_to(2)
        r = self._gate()
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("1 of 3 patches were never applied", r.stdout)
        self.assertIn("p3.patch", r.stdout)
        self.assertNotIn("p2.patch", r.stdout)
        self.assertEqual(3, self._applied_count())

    def test_the_repair_is_idempotent(self):
        self._reset_to(1)
        self.assertEqual(0, self._gate().returncode)
        again = self._gate()
        self.assertEqual(0, again.returncode)
        self.assertIn("full-stack roundtrip", again.stdout)
        self.assertEqual(3, self._applied_count())

    def test_the_full_stack_roundtrip_is_still_the_correctness_check(self):
        """A suffix that lands by coincidence must not pass as a repair.

        The real decomp stack does exactly this: with the last TWO patches missing,
        the last one still applies on its own. If the gate stopped at the first
        suffix that fitted it would report "1 missing" and leave a tree that does
        not roundtrip. It must keep going and find the true suffix.
        """
        self._reset_to(1)
        r = self._gate()
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        # the roundtrip ran and agreed, on all three, not on the short suffix
        self.assertIn("all 3 verified by roundtrip", r.stdout)

    # ------------------------------------------------------- genuine drift --

    def test_real_drift_fails_and_classifies_every_patch(self):
        self._reset_to(3)
        body = open(os.path.join(self.sub, "f.txt")).read()
        self._write(body.replace("line 14", "line 14 -- a local edit nobody patched in"))
        r = self._gate()
        self.assertEqual(1, r.returncode)
        err = r.stderr
        self.assertIn("Per-patch state", err)
        for name in self.patches:
            self.assertIn(name, err)
        self.assertIn("git -C sub diff", err)

    def test_drift_leaves_the_tree_as_it_found_it(self):
        self._reset_to(3)
        before = open(os.path.join(self.sub, "f.txt")).read()
        self._write(before.replace("line 14", "line 14 -- local"))
        marked = open(os.path.join(self.sub, "f.txt")).read()
        self.assertEqual(1, self._gate().returncode)
        self.assertEqual(marked, open(os.path.join(self.sub, "f.txt")).read())

    def test_skip_patch_gate_downgrades_drift_to_a_warning(self):
        self._reset_to(3)
        body = open(os.path.join(self.sub, "f.txt")).read()
        self._write(body.replace("line 14", "line 14 -- local"))
        r = self._gate({"SKIP_PATCH_GATE": "1"})
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("SKIP_PATCH_GATE=1 set", r.stderr)

    # ------------------------------------------- the list ci-3ds.sh owns --

    def test_ci_still_owns_the_patch_list_and_calls_this_gate(self):
        ci = os.path.abspath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "..", "ci-3ds.sh"))
        src = open(ci).read()
        self.assertIn("PATCH_LIST=(", src)
        self.assertIn("tools/patch-gate.sh", src)
        listed = re.findall(r'"decomp:([^"]+)"', src)
        pdir = os.path.abspath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
            "port", "3ds", "patches"))
        on_disk = sorted(f for f in os.listdir(pdir) if f.endswith(".patch"))
        self.assertEqual(sorted(listed), on_disk)


class LodBurstWindowTest(unittest.TestCase):
    """`tools/dev/lodburst.py` — B38. The receipt/transition alignment, pinned.

    `gdx3ds_dynlod_receipt()` closes a 64-frame window and resets its accumulators;
    `DynLodLogTransition()` prints the instant the tier changes, INSIDE the window. So a
    `raise`/`lower` line that appears below window N's receipt was decided by frames of
    window N+1. Reading it as N's is the obvious mistake and it inverts the conclusion:
    on the one post-fix hardware log it moves every race-entry raise off the course-load
    window and onto the quiet menu window before it, which is the difference between
    "the load stall re-tiered the field" and "the controller raised for no reason".
    This test is the fence; the fixture below is the shape of a real log.
    """

    LOG = "\n".join([
        "[dynlod] init auto=1 floor=1 hi=15.0 lo=9.0 lowerN=90 dwell=15 band=6.0",
        "[ninterp] tasks=64 lists=448 cmds=38592 rect=6272 tmem=6208 "
        "| dl=384 ldblk=5248 | rej=0 | tex hit=10 miss=0 evict=0 clr=0 inval=0/0",
        "[dynlod] auto=1 floor=1 tier=1 raises=0/0 lowers=0/0 ms=4.6 max=13.0 n=64 "
        "hi=15.0 lo=9.0 since=65535 lowerN=90 dwell=15",
        "[segment] loaded venue=4 segment=10 base=0xad636c0",
        "[dynlod] raise 1->2 avg4=17.5 over=4/4 blk=- n=0 since=65535 hi=15.0 lo=9.0 floor=1",
        "[ninterp] tasks=64 lists=2261 cmds=103653 rect=8025 tmem=8714 "
        "| dl=2261 ldblk=6255 | rej=0 | tex hit=10 miss=46 evict=0 clr=1 inval=0/3",
        "[dynlod] auto=1 floor=1 tier=2 raises=1/1 lowers=0/0 ms=12.1 max=35.7 n=64 "
        "hi=15.0 lo=9.0 since=25 lowerN=90 dwell=15",
        "[native] machines=1 lists=150 baked=47 draws=245 verts=23035 "
        "us=2201/f (tnl=584 bind=410 emit=844 draw=317) linfree=25917K",
        "",
    ])

    def _parse(self):
        lb = _import_lodburst()
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
            fh.write(self.LOG)
            path = fh.name
        self.addCleanup(os.unlink, path)
        return lb, lb.parse(path)

    def test_the_transition_lands_in_the_window_it_was_decided_in(self):
        _, ws = self._parse()
        self.assertEqual(2, len(ws))
        self.assertEqual([], ws[0].tx, "the quiet window must not own the raise")
        self.assertEqual(1, len(ws[1].tx))
        self.assertIn("raise 1->2", ws[1].tx[0])

    def test_the_event_lands_in_the_window_it_happened_in(self):
        _, ws = self._parse()
        self.assertEqual([], ws[0].tags())
        self.assertIn("venue", ws[1].tags())

    def test_the_course_load_is_named_rather_than_called_in_race_load(self):
        lb, ws = self._parse()
        self.assertEqual("course load", lb.classify(ws[1], ws[0]))

    def test_receipts_are_joined_to_the_right_window(self):
        _, ws = self._parse()
        self.assertAlmostEqual(4.6, ws[0].dl["ms"])
        self.assertAlmostEqual(12.1, ws[1].dl["ms"])
        self.assertAlmostEqual(38592.0, ws[0].ni["cmds"])
        self.assertAlmostEqual(245.0, ws[1].mc["draws"])
        self.assertAlmostEqual(2201.0, ws[1].mc["us"])
        self.assertFalse(ws[0].racing)
        self.assertTrue(ws[1].racing)

    def test_an_untimed_shot_receipt_still_counts_as_a_shot(self):
        """Older logs' capture receipt stops at `frame=` — it must not read as shots=0
        beside a tag list that says SHOT, which is what this tool printed once."""
        lb = _import_lodburst()
        old = "[autotest] SHOT captured -> autotest/title_L.bmp bytes=288054 frame=351"
        new = old + " ms=42"
        for line, cost in ((old, 0), (new, 42)):
            body = self.LOG.replace("[segment] loaded venue=4 segment=10 base=0xad636c0", line)
            with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
                fh.write(body)
                path = fh.name
            self.addCleanup(os.unlink, path)
            ws = lb.parse(path)
            self.assertEqual(1, ws[1].shots, line)
            self.assertEqual(cost, ws[1].shot_ms, line)

    def test_prof_is_the_wall_when_dynlod_is_pinned_off(self):
        """A replay pins rival_detail_auto=0 unless [debug] replay_dynlod=1, and then every
        dynlod receipt reads ms=0.0 — so the whole hardware matrix is blind to B38 and the
        only wall left is [prof], which is emitted AFTER the dynlod receipt."""
        lb = _import_lodburst()
        body = "\n".join([
            "[ninterp] tasks=64 lists=448 cmds=38592 | dl=384 ldblk=5248 "
            "| rej=0 | tex hit=10 miss=0 evict=0 clr=0 inval=0/0",
            "[dynlod] auto=0 floor=1 tier=0 raises=0/0 lowers=0/0 ms=0.0 max=0.0 n=0 "
            "hi=15.0 lo=9.0 since=65535 lowerN=90 dwell=15",
            "[prof] frame=257 n=64 br=0.00 dsp=2.26 vtx=0.00 tri=0.97 imp=1.04 drw=0.10 "
            "mtx=0.00 oth=0.25 | vtxN=0",
            "",
        ])
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
            fh.write(body)
            path = fh.name
        self.addCleanup(os.unlink, path)
        ws = lb.parse(path)
        self.assertEqual(1, len(ws), "[prof] must land on the window already filed")
        self.assertAlmostEqual(4.62, ws[0].pf["total"], places=2)
        self.assertEqual("prof", lb.pick_wall(ws, None))
        self.assertAlmostEqual(4.62, lb.wall(ws[0], "prof"), places=2)


def _import_lodburst():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lodburst.py")
    spec = importlib.util.spec_from_file_location("lodburst", os.path.abspath(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class LaunchVariantGuestLogTest(unittest.TestCase):
    """n60launch: a launch-variant boot writes log-<X>.txt, not log.txt; the harness follows it."""

    def _inst(self, d):
        class _Inst:
            def gdx_dir(self_inner):
                return d
        return _Inst()

    def test_follows_this_boots_variant_log(self):
        from gdxdev import retro
        import time
        with tempfile.TemporaryDirectory() as d:
            inst = self._inst(d)
            old = os.path.join(d, "log-P.txt")
            open(old, "w").write("earlier boot")
            os.utime(old, (time.time() - 600, time.time() - 600))
            open(os.path.join(d, "log-N-prev.txt"), "w").write("prev")
            retro._BOOT_STARTED["t"] = time.time()
            self.assertEqual(retro._guest_log_path(inst), os.path.join(d, "log.txt"))
            open(os.path.join(d, "log-N.txt"), "w").write("this boot")
            self.assertEqual(retro._guest_log_path(inst), os.path.join(d, "log-N.txt"))
            open(os.path.join(d, "log.txt"), "w").write("main")
            self.assertEqual(retro._guest_log_path(inst), os.path.join(d, "log.txt"))



class RetroBareIniTest(unittest.TestCase):
    """preset-fix `retro run --bare-ini`: the instance ini starts EMPTY (no seeded user
    sections from the Qt SD), so an empty-ini parity leg sees only its own keys."""

    def setUp(self):
        from gdxdev import retro
        self.retro = retro
        self.tmp = tempfile.mkdtemp()
        self.src = os.path.join(self.tmp, "qt")
        os.makedirs(self.src)
        for name in ("fzerox.o2r", "gdiffuser.o2r"):
            open(os.path.join(self.src, name), "w").write("x")
        open(os.path.join(self.src, "gdiffuser.ini"), "w").write(
            "[stereo]\nenabled = 1\n[display]\nwidescreen = 1\n[debug]\nfh = 1\n")
        self.saved = (util.GDX_SDMC_DIR, util.GDX_INI, util.AZAHAR_SDMC, retro.INSTANCE_ROOT, retro._BARE_INI)
        util.GDX_SDMC_DIR = self.src
        util.GDX_INI = os.path.join(self.src, "gdiffuser.ini")
        util.AZAHAR_SDMC = os.path.join(self.tmp, "nosdmc")
        retro.INSTANCE_ROOT = os.path.join(self.tmp, "inst")

    def tearDown(self):
        (util.GDX_SDMC_DIR, util.GDX_INI, util.AZAHAR_SDMC, self.retro.INSTANCE_ROOT,
         self.retro._BARE_INI) = self.saved
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _seed(self, bare):
        self.retro._BARE_INI = bare
        inst = self.retro.Instance("b")
        class Quiet(object):
            def say(self, *a, **k):
                pass
        self.retro.seed_sd(inst, Quiet(), {("debug", "console"): "1"})
        return inifile.read(os.path.join(inst.gdx_dir(), "gdiffuser.ini"))

    def test_default_seeds_the_user_sections(self):
        got = self._seed(False)
        self.assertEqual(got["stereo"]["enabled"], "1")
        self.assertEqual(got["display"]["widescreen"], "1")
        self.assertNotIn("fh", got.get("debug", {}))

    def test_bare_ini_seeds_nothing_but_the_run_keys(self):
        got = self._seed(True)
        self.assertEqual(got, {"debug": {"console": "1"}})


if __name__ == "__main__":
    unittest.main(verbosity=2)
