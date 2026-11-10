#!/usr/bin/env python3
"""Summarise an iowrap trace: which user clients were opened and how often
each selector was called, with the argument sizes seen.

    trace_summary.py out/trace_add.txt [other-trace.txt]

With two traces it also lists the selectors whose call counts differ.
"""
import re
import sys
from collections import Counter, OrderedDict

CALL = re.compile(r"^(call|async|struct|scalar)\s+conn=(\S+) sel=(\d+) in=(\d+)/(\d+) out=(\d+)/(\d+) kr=(\S+)")
OPEN = re.compile(r"^open\s+(\S+) type=(\d+) -> conn=(\S+) kr=(\S+)")
TRAP = re.compile(r"^trap(\d)\s+conn=(\S+) index=(\d+) kr=(\S+)")
MAP = re.compile(r"^map\s+conn=(\S+) type=(\d+) -> addr=\S+ size=(\d+)")


def load(path):
    conns, calls, order, maps = {}, Counter(), OrderedDict(), []
    for line in open(path):
        if m := OPEN.match(line):
            conns[m[3]] = f"{m[1]} (type {int(m[2]):#x})"
        elif m := CALL.match(line):
            key = (conns.get(m[2], m[2]), int(m[3]), f"in {m[4]} scalars + {m[5]} bytes, out {m[6]} + {m[7]}")
            calls[key] += 1
            order.setdefault(key, len(order))
        elif m := TRAP.match(line):
            key = (conns.get(m[2], m[2]), int(m[3]), f"fast trap with {m[1]} arguments")
            calls[key] += 1
            order.setdefault(key, len(order))
        elif m := MAP.match(line):
            maps.append((conns.get(m[1], m[1]), int(m[2]), int(m[3])))
    return conns, calls, order, maps


def main():
    conns, calls, order, maps = load(sys.argv[1])
    print("user clients opened:")
    for c in conns.values():
        print("  ", c)
    print("\nselectors in order of first use:")
    for key in sorted(calls, key=order.get):
        client, sel, shape = key
        print(f"  {client.split()[0]:<22} sel {sel:<4} x{calls[key]:<3} {shape}")
    if maps:
        print("\nshared memory mapped:")
        for client, typ, size in maps:
            print(f"  {client.split()[0]:<22} type {typ} size {size}")
    else:
        print("\nno IOConnectMapMemory64 calls seen")

    if len(sys.argv) > 2:
        _, other, _, _ = load(sys.argv[2])
        by_sel = lambda c: Counter({(k[0].split()[0], k[1]): 0 for k in c}) + Counter(
            {(k[0].split()[0], k[1]): v for k, v in c.items()})
        a, b = by_sel(calls), by_sel(other)
        print(f"\ncall-count differences ({sys.argv[1]} vs {sys.argv[2]}):")
        diff = [(k, a[k], b[k]) for k in sorted(set(a) | set(b)) if a[k] != b[k]]
        for (client, sel), x, y in diff:
            print(f"  {client:<22} sel {sel:<4} {x} vs {y}")
        if not diff:
            print("  none")


if __name__ == "__main__":
    main()
