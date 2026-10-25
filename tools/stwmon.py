#!/usr/bin/env python3
"""Stormworks monitor (Linux / Proton): CPU per named thread + memory over time.

    ./stwmon.py [interval_seconds] [csv_path]

A thread pinned at ~100% is the bottleneck: "main" (game logic, Lua, rendering submission),
"physics" (Bullet step), "vehicle_tick" (per-vehicle simulation; with stwpatch the work moves
to "stw-vehicle-N"). RSS that keeps rising while you stay in the same place is the leak.
"""
import os, sys, time, glob

HZ = os.sysconf("SC_CLK_TCK")


def find_pid():
    for p in glob.glob("/proc/[0-9]*/cmdline"):
        try:
            cmd = open(p, "rb").read().replace(b"\0", b" ").lower()
        except OSError:
            continue
        if b"stormworks64.exe" in cmd and b"steam.exe" not in cmd:
            pid = int(p.split("/")[2])
            try:
                if open(f"/proc/{pid}/comm").read().strip().lower().startswith("stormworks"):
                    return pid
            except OSError:
                pass
    return None


def threads(pid):
    out = {}
    for t in glob.glob(f"/proc/{pid}/task/*"):
        try:
            comm = open(t + "/comm").read().strip()
            st = open(t + "/stat").read()
            fields = st[st.rindex(")") + 2:].split()
            out[int(t.rsplit("/", 1)[1])] = (comm, int(fields[11]) + int(fields[12]))
        except (OSError, ValueError):
            pass
    return out


def mem(pid):
    m = {}
    for line in open(f"/proc/{pid}/status"):
        k, _, v = line.partition(":")
        if k in ("VmRSS", "VmSwap", "RssAnon"):
            m[k] = int(v.split()[0]) / 1024
    return m


def main():
    interval = float(sys.argv[1]) if len(sys.argv) > 1 else 5
    csv = open(sys.argv[2], "a") if len(sys.argv) > 2 else None
    pid = find_pid()
    if not pid:
        sys.exit("stormworks64.exe not running")
    print(f"pid {pid}, interval {interval}s")
    if csv:
        csv.write("time,rss_mib,anon_mib,swap_mib,top_thread,top_cpu\n")
    prev, t0 = threads(pid), time.time()
    start_rss = None
    while os.path.exists(f"/proc/{pid}"):
        time.sleep(interval)
        cur, t1 = threads(pid), time.time()
        dt = t1 - t0
        rows = []
        for tid, (comm, ticks) in cur.items():
            if tid in prev:
                cpu = (ticks - prev[tid][1]) / HZ / dt * 100
                if cpu >= 2:
                    rows.append((cpu, comm, tid))
        rows.sort(reverse=True)
        m = mem(pid)
        start_rss = start_rss or m.get("VmRSS", 0)
        total = sum(r[0] for r in rows)
        print(time.strftime("%H:%M:%S"),
              f"RSS {m.get('VmRSS', 0):8.0f} MiB ({m.get('VmRSS', 0) - start_rss:+.0f})  "
              f"swap {m.get('VmSwap', 0):.0f}  total CPU {total:.0f}%")
        for cpu, comm, tid in rows[:10]:
            bar = "#" * int(cpu / 5)
            print(f"   {comm[:16]:16s} {tid:>7} {cpu:6.1f}% {bar}")
        if csv and rows:
            csv.write(f"{int(t1)},{m.get('VmRSS', 0):.0f},{m.get('RssAnon', 0):.0f},"
                      f"{m.get('VmSwap', 0):.0f},{rows[0][1]},{rows[0][0]:.0f}\n")
            csv.flush()
        prev, t0 = cur, t1


if __name__ == "__main__":
    main()
