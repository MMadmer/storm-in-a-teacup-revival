"""Summarizes a mod DLL's self samples of a StormBench report by function, using the mod's linker map.

Usage: po_symbols.py <label> [ModName]   (ModName defaults to PerformanceOptimizer)
"""
import bisect
import re
import sys
from pathlib import Path

mods = Path(__file__).resolve().parent.parent / "Mods"
module = sys.argv[2] if len(sys.argv) > 2 else "PerformanceOptimizer"
report = mods / "StormBench" / f"Benchmark-{sys.argv[1]}.txt"
text = (mods / module / f"{module}.map").read_text(encoding="utf-8", errors="replace")
base = int(re.search(r"Preferred load address is ([0-9a-fA-F]+)", text).group(1), 16)
symbols = sorted(
    (int(m.group(2), 16) - base, m.group(1))
    for m in re.finditer(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8})\s+[fi ]", text)
)
keys = [address for address, _ in symbols]

section = report.read_text(encoding="utf-8").split("== self")[1].split("== inclusive")[0]
pattern = re.compile(rf"\s*(\d+)\s+[\d.]+%\s+{re.escape(module)}\.dll\+0x([0-9A-F]+)")
totals = {}
for line in section.splitlines():
    if match := pattern.match(line):
        index = bisect.bisect_right(keys, int(match.group(2), 16)) - 1
        name = symbols[index][1] if index >= 0 else "?"
        totals[name] = totals.get(name, 0) + int(match.group(1))
for name, count in sorted(totals.items(), key=lambda item: -item[1])[:20]:
    print(count, name[:120])
