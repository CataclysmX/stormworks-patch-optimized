#!/bin/bash
# Hotspot profile of the running game, mapped back to Ghidra function addresses.
# Needs perf (sudo pacman -S perf) and kernel.perf_event_paranoid <= 1 (or run with sudo).
#   ./profile.sh [seconds]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
SEC=${1:-20}
PID=$(pgrep -f 'stormworks64.exe' | while read p; do
        [[ "$(cat /proc/$p/comm 2>/dev/null)" == stormworks* ]] && echo $p; done | head -1)
[ -z "$PID" ] && { echo "stormworks64.exe not running"; exit 1; }
OUT=$(mktemp -d)
echo "sampling pid $PID for ${SEC}s..."
perf record -F 499 -p "$PID" -o "$OUT/perf.data" -- sleep "$SEC" 2>/dev/null
perf script -i "$OUT/perf.data" -F comm,ip,dso 2>/dev/null > "$OUT/samples.txt"
python3 - "$OUT/samples.txt" "$DIR/functions.tsv" "$PID" <<'EOF'
import sys, bisect, collections
samples, funcs, pid = sys.argv[1], sys.argv[2], sys.argv[3]
# load base of stormworks64.exe from the process maps
base = None
for line in open(f"/proc/{pid}/maps"):
    if "stormworks64.exe" in line.lower():
        base = int(line.split("-")[0], 16) if base is None else min(base, int(line.split("-")[0], 16))
starts, names = [], []
for line in open(funcs):
    rva, size, name = line.rstrip("\n").split("\t")
    starts.append(int(rva, 16)); names.append(name)
per_thread = collections.Counter(); per_func = collections.Counter(); total = 0
for line in open(samples):
    parts = line.split()
    if len(parts) < 2: continue
    comm, ip = parts[0], parts[1]
    dso = parts[2] if len(parts) > 2 else ""
    total += 1
    per_thread[comm] += 1
    try: a = int(ip, 16)
    except ValueError: continue
    if base and "stormworks64" in dso.lower() or (base and base <= a < base + 0xd8b000):
        i = bisect.bisect_right(starts, a - base) - 1
        per_func[(comm, f"0x{starts[i]:x} {names[i]}" if i >= 0 else hex(a - base))] += 1
    else:
        per_func[(comm, dso.strip("()").split("/")[-1] or "?")] += 1
print(f"\n{total} samples\n== per thread ==")
for k, v in per_thread.most_common(12): print(f"{v*100/total:6.1f}%  {k}")
print("\n== hottest functions (thread / rva name) ==")
for (comm, f), v in per_func.most_common(40): print(f"{v*100/total:6.1f}%  {comm[:16]:16s} {f}")
EOF
rm -rf "$OUT"
