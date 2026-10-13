"""CPU use per process (and the whole tree) over a window, as a percentage of one core.  idle_cpu.py <pid> [seconds]"""
import os, sys, time
def tree(root):
    pp = {}
    for p in os.listdir("/proc"):
        if p.isdigit():
            try: pp[int(p)] = int(open(f"/proc/{p}/stat").read().rsplit(")", 1)[1].split()[1])
            except Exception: pass
    out, fr = [root], [root]
    while fr:
        nxt = [p for p, q in pp.items() if q in fr and p not in out]; out += nxt; fr = nxt
    return out
def cpu(pid):
    try:
        f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split(); return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    except Exception: return 0.0
root, secs = int(sys.argv[1]), float(sys.argv[2]) if len(sys.argv) > 2 else 20
pids = tree(root); a = {p: cpu(p) for p in pids}; time.sleep(secs); b = {p: cpu(p) for p in pids}
total = 0.0
for p in pids:
    pct = 100 * (b[p] - a[p]) / secs; total += pct
    print(f"    {open(f'/proc/{p}/comm').read().strip()[:16]:16s} pid {p:<8d} {pct:5.2f}%")
print(f"  idle CPU over {secs:.0f}s: {total:.2f}% of one core (tree)")
