"""Read original monitor logs in a ZIP; never infer opcodes from CTS totals.

Usage: python tests/analyze_dab_loss_logs.py path/to/EPG_test_...zip
Objects already in flight at the start of a log are boundary observations.
The checkpoint bound assumes the observed 512-byte segment profile, not RF loss.
"""
import json
from pathlib import Path
import re
import sys
import zipfile


def analyze(text):
    irq = []
    closed = []
    active = None
    boundary_ready = 0
    for line in text.splitlines():
        if "[DAB/IRQ]" in line:
            irq.append({k: int(v) for k, v in re.findall(r"(ctsPoll|ctsIrq|DSRV|overflow|errors)=(\d+)", line)})
        match = re.search(r"\[SLS\] Header TID=(\d+) length=(\d+)", line)
        if match:
            tid, size = map(int, match.groups())
            if active is None or active["tid"] != tid:
                active = {"tid": tid, "size": size, "missing512LowerBound": 0}
        match = re.search(r"\[SLS\] Saved TID=(\d+) seg=(\d+) len=(\d+) bytes=(\d+)/(\d+)", line)
        if match and active is not None:
            tid, segment, length, received, size = map(int, match.groups())
            if tid == active["tid"]:
                expected_prefix = min(size, segment * 512 + length)
                active["missing512LowerBound"] = max(active["missing512LowerBound"], max(0, expected_prefix - received) // 512)
        match = re.search(r"\[SLS\] Partial TID=(\d+) abandoned", line)
        if match and active is not None and active["tid"] == int(match[1]):
            closed.append(dict(active, result="abandoned"))
            active = None
        if "[SLS/UI] READY:" in line:
            if active is not None:
                closed.append(dict(active, result="complete"))
                active = None
            else:
                boundary_ready += 1
    increments = [b["ctsPoll"] - a["ctsPoll"] for a, b in zip(irq, irq[1:]) if b["ctsPoll"] >= a["ctsPoll"]]
    return {
        "ctsPollIntervalIncrements": increments,
        "ctsPollMeanPerNominal30s": sum(increments) / len(increments) if increments else None,
        "dsrvEventIntervalIncrements": [b["DSRV"] - a["DSRV"] for a, b in zip(irq, irq[1:])],
        "complete": sum(o["result"] == "complete" for o in closed),
        "abandoned": sum(o["result"] == "abandoned" for o in closed),
        "boundaryReady": boundary_ready,
        "openAtEnd": active,
        "missing512LowerBound": sum(o["missing512LowerBound"] for o in closed),
        "expectedSegmentsClosed": sum((o["size"] + 511) // 512 for o in closed),
        "closed": closed,
        "opcodeBreakdown": "UNAVAILABLE in historical log",
    }


if __name__ == "__main__":
    with zipfile.ZipFile(Path(sys.argv[1])) as archive:
        for name in archive.namelist():
            if name.startswith("e24bc2a_") and name.endswith(".txt"):
                print(json.dumps({"file": name, **analyze(archive.read(name).decode("utf-8", errors="replace"))}, indent=2))
