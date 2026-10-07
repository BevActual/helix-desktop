"""PSS and CPU of a process and all its descendants (WebKit helpers included).  process_tree.py <pid> <label>"""
import os, sys, time
def tree(root):
    ppid = {}
    for p in os.listdir("/proc"):
        if p.isdigit():
            try: ppid[int(p)] = int(open(f"/proc/{p}/stat").read().rsplit(")", 1)[1].split()[1])
            except Exception: pass
    found, frontier = [root], [root]
    while frontier:
        nxt = [p for p, pp in ppid.items() if pp in frontier and p not in found]
        found += nxt; frontier = nxt
    return found
def pss(pid):
    try:
        for l in open(f"/proc/{pid}/smaps_rollup"):
            if l.startswith("Pss:"): return int(l.split()[1]) / 1024
    except Exception: return 0
def cpu(pid):
    try:
        f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split(); return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    except Exception: return 0
root, label = int(sys.argv[1]), sys.argv[2]
pids = tree(root)
rows = [(open(f"/proc/{p}/comm").read().strip(), p, pss(p)) for p in pids]
for n, p, m in sorted(rows, key=lambda r: -r[2])[:6]: print(f"    {n:18s} {p:<8d} {m:6.0f} MB")
total = sum(r[2] for r in rows)
shell = sum(m for n, _, m in rows if n != "helix-service")
print(f"  {label}: {len(pids)} processes, TOTAL PSS {total:.0f} MB (shell {shell:.0f} MB without helix-service), cpu-seconds so far {sum(cpu(p) for p in pids):.2f}")
