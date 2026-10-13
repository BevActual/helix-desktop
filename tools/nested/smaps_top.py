"""Where a process's PSS goes, grouped by backing file (heap/anon shown separately).  smaps_top.py <pid> [count]"""
import collections, re, sys
pid, count = int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 12
by_file, current = collections.Counter(), None
for line in open(f"/proc/{pid}/smaps"):
    if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
        parts = line.split(None, 5)
        name = parts[5].strip() if len(parts) > 5 else "[anon]"
        current = name if name else "[anon]"
    elif line.startswith("Pss:"):
        by_file[current] += int(line.split()[1])
total = sum(by_file.values())
print(f"  pid {pid} total PSS {total/1024:.0f} MB")
for name, kb in by_file.most_common(count):
    print(f"    {kb/1024:7.1f} MB  {name[:90]}")
