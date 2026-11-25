#!/usr/bin/env python3
"""Discrepancy Detection - FL Studio pattern note pool vs .raid unique notes.

Counts the charted notes of every FL Studio pattern (minus note artifacting:
notes sitting beyond the pattern's playlist loop length, which playlist clips
never play), pools them, and compares against the .raid chart's stored
(pattern-table) event count.

Usage:
    discrepancy.py <file.flp|file.raid> [other-file]

  - .flp input: the sibling .raid (same basename) is used unless the second
    argument names the .raid explicitly.
  - .raid input: the sibling .flp is used unless the second argument names
    the .flp explicitly.

raid_engine.exe is located next to this script, in ../build/Release, or on
PATH.
"""

import os
import re
import shutil
import subprocess
import sys


def find_engine():
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(here, "raid_engine.exe"),
        os.path.join(here, "..", "build", "Release", "raid_engine.exe"),
        shutil.which("raid_engine.exe"),
        shutil.which("raid_engine"),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return None


def run_engine(engine, args):
    proc = subprocess.run(
        [engine] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace")
    return proc


def parse_flppatterns(text):
    pats = []
    total = None
    for line in text.splitlines():
        m = re.match(r'pat\s+(\d+)\s+"(.*)":\s+raw=(\d+)\s+charted=(\d+)\s+'
                     r'inbounds=(\d+)\s+artifacts=(\d+)\s+rawin=(\d+)\s+'
                     r'rawart=(\d+)\s+len=(\d+)t\s+used=(\d+)', line)
        if m:
            pats.append({
                "num": int(m.group(1)), "name": m.group(2),
                "raw": int(m.group(3)), "charted": int(m.group(4)),
                "inbounds": int(m.group(5)), "artifacts": int(m.group(6)),
                "rawin": int(m.group(7)), "rawart": int(m.group(8)),
                "len_ticks": int(m.group(9)), "used": int(m.group(10)),
            })
            continue
        m = re.match(r'\[flp-total\]\s+patterns=(\d+)\s+raw=(\d+)\s+'
                     r'charted=(\d+)\s+artifacts=(\d+)\s+pool=(\d+)\s+'
                     r'poolused=(\d+)\s+rawart=(\d+)\s+rawpool=(\d+)\s+'
                     r'rawpoolused=(\d+)', line)
        if m:
            total = {
                "patterns": int(m.group(1)), "raw": int(m.group(2)),
                "charted": int(m.group(3)), "artifacts": int(m.group(4)),
                "pool": int(m.group(5)), "poolused": int(m.group(6)),
                "rawart": int(m.group(7)), "rawpool": int(m.group(8)),
                "rawpoolused": int(m.group(9)),
            }
    return pats, total


def parse_info(text):
    info = {}
    m = re.search(r'logical events\s*:\s*(\d+)', text)
    if m:
        info["logical"] = int(m.group(1))
    m = re.search(r'patterns\s*:\s*(\d+)', text)
    if m:
        info["patterns"] = int(m.group(1))
    m = re.search(r'unique notes\s*:\s*(\d+)\s+in\s+(\d+)\s+stored events', text)
    if m:
        info["unique_notes"] = int(m.group(1))
        info["stored_events"] = int(m.group(2))
    m = re.search(r'duration\s*:\s*([\d.]+)\s*ms', text)
    if m:
        info["duration_ms"] = float(m.group(1))
    return info


def swap_ext(path, old, new):
    base, ext = os.path.splitext(path)
    if ext.lower() == old:
        return base + new
    return None


def main():
    args = [a for a in sys.argv[1:] if a.strip()]
    if not args:
        print(__doc__)
        return 2

    a, b = args[0], args[1] if len(args) > 1 else None
    flp = raid = None
    for cand in (a, b):
        if not cand:
            continue
        low = cand.lower()
        if low.endswith((".flp", ".fsc")):
            flp = cand
        elif low.endswith(".raid"):
            raid = cand
    if flp is None and raid is not None:
        flp = swap_ext(raid, ".raid", ".flp")
    if raid is None and flp is not None:
        raid = swap_ext(flp, ".flp", ".raid") or swap_ext(flp, ".fsc", ".raid")

    if not flp or not os.path.isfile(flp):
        print("error: no usable .flp/.fsc found (arg: %s)" % args[0])
        return 1
    if not raid or not os.path.isfile(raid):
        print("error: no .raid found - looked for %s" %
              (raid or swap_ext(flp, ".flp", ".raid") or "?"))
        return 1

    engine = find_engine()
    if not engine:
        print("error: raid_engine.exe not found next to script, in "
              "../build/Release, or on PATH")
        return 1

    print("== Discrepancy Detection ==")
    print("FL project : %s" % os.path.basename(flp))
    print("RAID chart : %s" % os.path.basename(raid))
    print("engine     : %s" % engine)
    print()

    # ---- FL side ----
    proc = run_engine(engine, ["flppatterns", flp])
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        print("flppatterns failed (code %d)" % proc.returncode)
        return 1
    pats, total = parse_flppatterns(proc.stdout)
    if total is None:
        print(proc.stdout)
        print("could not parse flppatterns output")
        return 1

    print("-- FL Studio patterns --")
    for ptn in pats:
        label = ptn["name"] if ptn["name"] else "(unnamed)"
        print("  pat %4d %-28s raw=%8d events=%6d inbounds=%6d artifacts=%4d "
              "rawart=%6d len=%7dt %s" % (
                  ptn["num"], label[:28], ptn["raw"], ptn["charted"],
                  ptn["inbounds"], ptn["artifacts"], ptn["rawart"],
                  ptn["len_ticks"],
                  "[playlist]" if ptn["used"] else "[unused]  "))
    print()
    print("FL pool (unique notes, in-bounds, each pattern once):")
    print("  raw notes (pre-merge, stack-weighted): %d"
          % total["rawpool"])
    print("    of which playlist-referenced:       %d" % total["rawpoolused"])
    print("  chart events (post-merge):           %d"
          % total["pool"])
    print("  excluded artifacts: %d events / %d raw notes"
          % (total["artifacts"], total["rawart"]))
    print()

    # ---- RAID side ----
    proc = run_engine(engine, ["info", raid])
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        print("info failed (code %d)" % proc.returncode)
        return 1
    info = parse_info(proc.stdout)
    if "stored_events" not in info:
        print(proc.stdout)
        print("could not parse raid info output")
        return 1

    print("-- RAID chart --")
    print("  unique notes (stack-weighted, pattern table): %s"
          % info.get("unique_notes", "?"))
    print("  stored events (pattern table, each def once): %d"
          % info["stored_events"])
    print("  logical events (expanded on the timeline):   %s"
          % info.get("logical", "?"))
    print("  pattern defs: %s   duration: %.1f s"
          % (info.get("patterns", "?"),
             info.get("duration_ms", 0.0) / 1000.0))
    print()

    # ---- verdict ----
    # Two comparisons: raw notes vs the raid's stack-weighted uniqueNotes
    # (the FNF "note count" - one press hits a whole stack), and merged
    # chart events vs stored pattern-table events.
    raw_fl = total["rawpool"]
    raw_rd = info.get("unique_notes")
    ev_fl = total["pool"]
    ev_rd = info["stored_events"]
    print("== Result ==")
    print("  FL raw-note pool    : %d   (in-bounds, patterns once)" % raw_fl)
    print("  RAID unique notes   : %s" % (raw_rd if raw_rd is not None else "?"))
    if raw_rd is not None:
        rdelta = raw_fl - raw_rd
        print("  raw delta (FL-RAID) : %+d" % rdelta)
    print("  FL event pool       : %d" % ev_fl)
    print("  RAID stored events  : %d" % ev_rd)
    print("  event delta         : %+d" % (ev_fl - ev_rd))
    print("  excluded artifacts  : %d events / %d raw notes"
          % (total["artifacts"], total["rawart"]))

    mismatches = []
    if raw_rd is not None:
        rd_ = raw_fl - raw_rd
        if rd_ == 0:
            pass
        elif rd_ == -total["rawart"] and total["rawart"]:
            pass  # artifacts excluded from pool but stored via clip windows
        else:
            mismatches.append(("raw notes", rd_))
    ed = ev_fl - ev_rd
    if ed != 0 and not (ed == -total["artifacts"] and total["artifacts"]):
        mismatches.append(("chart events", ed))

    if not mismatches:
        print("  VERDICT: MATCH - every in-bounds pattern note is stored.")
    else:
        for what, d in mismatches:
            if d > 0:
                print("  DISCREPANCY: raid is missing %d %s." % (d, what))
            else:
                print("  DISCREPANCY: raid holds %d extra %s."
                      % (-d, what))
    return 0


if __name__ == "__main__":
    sys.exit(main())
